#ifdef __linux__

#include "platform/linux_inotify_watcher.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>

namespace everything_lite {
namespace {
namespace fs = std::filesystem;

bool within(const std::string& path, const std::string& root) {
    return path == root || (path.size() > root.size() &&
        path.compare(0, root.size(), root) == 0 &&
        (root == "/" || path[root.size()] == '/'));
}

std::string normalized(const std::string& path) {
    if (path.empty()) return {};
    std::error_code ec;
    auto result = fs::absolute(fs::path(path), ec).lexically_normal().string();
    if (ec) return {};
    while (result.size() > 1 && result.back() == '/') result.pop_back();
    return result;
}

std::vector<std::string> normalizedRoots(const std::vector<std::string>& roots) {
    std::vector<std::string> sorted;
    for (const auto& root : roots) sorted.push_back(normalized(root));
    std::sort(sorted.begin(), sorted.end());
    std::vector<std::string> result;
    for (const auto& root : sorted) {
        bool covered = false;
        for (const auto& previous : result) if (within(root, previous)) covered = true;
        if (!covered) result.push_back(root);
    }
    return result;
}

std::string systemError(const std::string& operation, int code = errno) {
    return operation + ": " + std::strerror(code);
}

bool missing(const std::error_code& ec) {
    return ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory;
}

// Reject symlinks in every configured path component, including ancestors.
// IN_DONT_FOLLOW alone protects only the final component.
bool validateRoot(const std::string& root, std::string& error) {
    if (root.empty()) { error = "Watch roots must be nonempty paths"; return false; }
    fs::path prefix;
    for (const auto& component : fs::path(root)) {
        prefix /= component;
        std::error_code ec;
        const auto status = fs::symlink_status(prefix, ec);
        if (missing(ec)) return true; // Watch the nearest existing ancestor for recreation.
        if (ec) { error = "Cannot inspect watch root: " + ec.message(); return false; }
        if (fs::is_symlink(status)) {
            error = "Symlink watch roots or ancestors are not followed";
            return false;
        }
        if (!fs::is_directory(status)) { error = "Watch root must be a directory"; return false; }
    }
    return true;
}

struct WatchSet {
    int fd = -1;
    std::vector<std::string> roots;
    std::unordered_map<int, std::string> paths;
    std::unordered_set<std::string> installed;
    std::unordered_map<std::string, bool> rootPresent;
    ~WatchSet() { if (fd >= 0) ::close(fd); }
    bool relevant(const std::string& path) const {
        for (const auto& root : roots) if (within(path, root)) return true;
        return false;
    }
    bool ancestor(const std::string& path) const {
        for (const auto& root : roots) if (within(root, path)) return true;
        return false;
    }
    bool open(std::string& error) {
        if (fd >= 0) ::close(fd);
        paths.clear();
        installed.clear();
        fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd < 0) { error = systemError("Cannot initialize inotify"); return false; }
        return true;
    }
    bool addDirectory(const std::string& path, std::string& error) {
        if (installed.count(path)) return true;
        constexpr std::uint32_t mask = IN_CREATE | IN_DELETE | IN_CLOSE_WRITE | IN_ATTRIB |
            IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT |
            IN_ONLYDIR | IN_DONT_FOLLOW;
        const int wd = ::inotify_add_watch(fd, path.c_str(), mask);
        if (wd < 0) {
            if (errno == ENOENT) return true; // A queued topology event will repair a race.
            error = systemError("Cannot install inotify directory watch");
            return false;
        }
        // Different bind-mount aliases can refer to one inode. Silently choosing
        // one spelling would lose coverage, so report that unsupported ambiguity.
        auto previous = paths.find(wd);
        if (previous != paths.end() && previous->second != path) {
            error = "Multiple watch paths refer to the same directory inode";
            return false;
        }
        paths[wd] = path;
        installed.insert(path);
        return true;
    }
    bool addRootsAndParents(std::string& error) {
        for (const auto& root : roots) {
            if (!validateRoot(root, error)) return false;
            std::error_code ec;
            auto parent = fs::path(root).parent_path();
            for (;;) {
                ec.clear();
                const auto parentStatus = fs::symlink_status(parent, ec);
                if (!ec && fs::is_directory(parentStatus)) break;
                if (ec && !missing(ec)) { error = "Cannot inspect root ancestor: " + ec.message(); return false; }
                const auto next = parent.parent_path();
                if (next == parent || parent.empty()) { error = "No accessible root ancestor"; return false; }
                parent = next;
            }
            // Descendants do not receive MOVE_SELF when a higher ancestor moves.
            // Watch the existing ancestor chain so the configured pathname can be
            // repaired even when several missing components must be recreated.
            std::vector<std::string> ancestors;
            for (;;) {
                ancestors.push_back(parent.string());
                const auto next = parent.parent_path();
                if (next == parent) break;
                parent = next;
            }
            // Install top-down so an inode replacement below an installed parent
            // is queued before its child's pathname can be cached as installed.
            for (auto ancestor = ancestors.rbegin(); ancestor != ancestors.rend(); ++ancestor)
                if (!addDirectory(*ancestor, error)) return false;
            if (!validateRoot(root, error)) return false;
            ec.clear();
            const auto status = fs::symlink_status(root, ec);
            if (ec && !missing(ec)) { error = "Cannot inspect watch root: " + ec.message(); return false; }
            if (fs::is_directory(status) && !addDirectory(root, error)) return false;
            rootPresent[root] = fs::is_directory(status);
        }
        return true;
    }
    bool addSubtree(const std::string& root, const std::atomic_bool& running, std::string& error) {
        std::vector<fs::path> pending{root};
        bool complete = true;
        while (!pending.empty() && running.load()) {
            const auto path = std::move(pending.back());
            pending.pop_back();
            std::error_code ec;
            const auto status = fs::symlink_status(path, ec);
            if (missing(ec) || fs::is_symlink(status)) continue;
            if (ec) { error = "Cannot inspect subtree: " + ec.message(); complete = false; continue; }
            if (!fs::is_directory(status)) continue;
            if (!addDirectory(path.string(), error)) { complete = false; continue; }
            fs::directory_iterator iter(path, ec), end;
            if (ec) {
                if (!missing(ec)) { error = "Cannot enumerate subtree: " + ec.message(); complete = false; }
                continue;
            }
            while (iter != end && running.load()) {
                const auto childStatus = iter->symlink_status(ec);
                if (ec && !missing(ec)) { error = "Cannot inspect directory entry: " + ec.message(); complete = false; }
                if (!ec && fs::is_directory(childStatus) && !fs::is_symlink(childStatus))
                    pending.push_back(iter->path());
                ec.clear();
                iter.increment(ec);
                if (ec) {
                    if (!missing(ec)) { error = "Cannot enumerate subtree: " + ec.message(); complete = false; }
                    break;
                }
            }
        }
        return complete;
    }
    bool addTrees(const std::atomic_bool& running, std::string& error) {
        bool complete = true;
        for (const auto& root : roots) {
            if (!running.load()) break;
            if (!addSubtree(root, running, error)) complete = false;
        }
        return complete;
    }
};
} // namespace

struct LinuxInotifyWatcher::Impl {
    mutable std::mutex mutex;
    std::condition_variable joined;
    std::vector<std::string> roots;
    Callback callback;
    std::string error;
    std::atomic_bool running{false};
    std::atomic_bool reconfigure{false};
    bool joining = false;
    int wakeFd = -1;
    std::thread worker;
    std::thread::id workerId;

    void wakeLocked() {
        if (wakeFd < 0) return;
        std::uint64_t one = 1;
        const auto result = ::write(wakeFd, &one, sizeof(one));
        (void)result; // EAGAIN means a wake is already pending.
    }
    void setError(const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex);
        error = value;
    }
    void emit(const FileEvent& event) {
        Callback copy;
        {
            std::lock_guard<std::mutex> lock(mutex);
            copy = callback;
        }
        if (!copy || !running.load()) return;
        try { copy(event); }
        catch (const std::exception& e) { setError(std::string("Watcher callback failed: ") + e.what()); }
        catch (...) { setError("Watcher callback failed with an unknown exception"); }
    }
    void rescan(const WatchSet& watches, const std::string& issue = {}) {
        setError(issue);
        for (const auto& root : watches.roots) emit({root, true, issue});
    }
    bool rebuild(WatchSet& watches, bool newRoots, std::string& issue) {
        const auto previousRoots = watches.rootPresent;
        if (newRoots) {
            std::lock_guard<std::mutex> lock(mutex);
            watches.roots = roots;
        }
        if (watches.roots.empty()) { issue = "No watch roots configured"; return false; }
        if (!watches.open(issue) || !watches.addRootsAndParents(issue)) return false;
        const bool complete = watches.addTrees(running, issue);
        // Replacing the fd intentionally discards queued events. Preserve root
        // disappearance/recreation notifications even when they were in that tail.
        for (const auto& root : watches.roots) {
            const auto previous = previousRoots.find(root);
            if (previous != previousRoots.end() && previous->second != watches.rootPresent[root])
                emit({root, false, {}});
        }
        return complete;
    }
    void run(std::unique_ptr<WatchSet> watches, int wake) noexcept {
        try {
            std::string issue;
            bool healthy = watches->addTrees(running, issue);
            rescan(*watches, issue);
            auto retryAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            alignas(inotify_event) std::array<char, 64 * 1024> buffer{};
            while (running.load()) {
                pollfd fds[2]{{watches->fd, POLLIN, 0}, {wake, POLLIN, 0}};
                int timeout = -1;
                if (!healthy) {
                    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                        retryAt - std::chrono::steady_clock::now()).count();
                    timeout = static_cast<int>(std::max<std::int64_t>(0, remaining));
                }
                const int result = ::poll(fds, 2, timeout);
                if (result < 0 && errno == EINTR) continue;
                if (!running.load()) break;
                if (fds[1].revents & POLLIN) {
                    std::uint64_t count = 0;
                    const auto ignored = ::read(wake, &count, sizeof(count));
                    (void)ignored;
                }
                if (reconfigure.exchange(false) || (!healthy && std::chrono::steady_clock::now() >= retryAt)) {
                    issue.clear();
                    healthy = rebuild(*watches, true, issue);
                    retryAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                    rescan(*watches, issue);
                    continue;
                }
                if (result < 0 || (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    issue = result < 0 ? systemError("Cannot poll inotify") : "Inotify descriptor became unavailable";
                    rescan(*watches, issue);
                    issue.clear();
                    healthy = rebuild(*watches, false, issue);
                    retryAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                    rescan(*watches, issue);
                    continue;
                }
                if (!(fds[0].revents & POLLIN)) continue;
                const auto bytes = ::read(watches->fd, buffer.data(), buffer.size());
                if (bytes < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                if (bytes <= 0) {
                    rescan(*watches, bytes < 0 ? systemError("Cannot read inotify") : "Inotify stream unexpectedly ended");
                    issue.clear();
                    healthy = rebuild(*watches, false, issue);
                    retryAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                    rescan(*watches, issue);
                    continue;
                }
                bool reset = false;
                std::vector<std::string> additions;
                std::vector<FileEvent> events;
                std::unordered_map<std::uint32_t, std::string> movedSources;
                for (std::size_t offset = 0; offset + sizeof(inotify_event) <= static_cast<std::size_t>(bytes);) {
                    const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
                    const auto size = sizeof(inotify_event) + event->len;
                    if (size > static_cast<std::size_t>(bytes) - offset) { reset = true; break; }
                    offset += size;
                    if (event->mask & IN_Q_OVERFLOW) { reset = true; continue; }
                    auto found = watches->paths.find(event->wd);
                    if (found == watches->paths.end()) continue;
                    std::string path = found->second;
                    if (event->len && event->name[0]) {
                        if (path != "/") path.push_back('/');
                        path.append(event->name, strnlen(event->name, event->len));
                    }
                    if (event->mask & IN_IGNORED) {
                        watches->installed.erase(found->second);
                        watches->paths.erase(found);
                        reset = true;
                        continue;
                    }
                    const bool relevant = watches->relevant(path);
                    const bool self = (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) != 0;
                    const bool directory = (event->mask & IN_ISDIR) != 0 || self;
                    if (!relevant && !(directory && watches->ancestor(path))) continue;
                    if (directory && (event->mask & IN_MOVED_FROM) && event->cookie)
                        movedSources[event->cookie] = path;
                    if (directory && (event->mask & IN_MOVED_TO) && event->cookie) {
                        auto source = movedSources.find(event->cookie);
                        if (source != movedSources.end()) {
                            // Later records in this same read still use the old wd.
                            // Remap them before processing close-write/attribute events.
                            for (auto& watched : watches->paths)
                                if (within(watched.second, source->second))
                                    watched.second = path + watched.second.substr(source->second.size());
                            watches->installed.clear();
                            for (const auto& watched : watches->paths) watches->installed.insert(watched.second);
                            movedSources.erase(source);
                        }
                    }
                    bool movedOut = false;
                    for (const auto& source : movedSources) {
                        const bool originalMove = (event->mask & IN_MOVED_FROM) &&
                            event->cookie == source.first && path == source.second;
                        if (within(path, source.second) && !originalMove) movedOut = true;
                    }
                    if (movedOut) continue; // A compensation scan covers an unpaired move.
                    if (relevant) events.push_back({path, false, {}});
                    if (self || (directory && (event->mask & (IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)))) reset = true;
                    if (directory && (event->mask & IN_CREATE)) {
                        if (relevant) additions.push_back(path);
                        else reset = true; // A missing root or its ancestor reappeared.
                    }
                }
                issue.clear();
                if (reset) {
                    healthy = rebuild(*watches, false, issue);
                    retryAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                    rescan(*watches, issue);
                } else if (!additions.empty()) {
                    for (const auto& path : additions)
                        if (!watches->addSubtree(path, running, issue)) healthy = false;
                    if (!healthy && issue.empty()) {
                        std::lock_guard<std::mutex> lock(mutex);
                        issue = error;
                    }
                    rescan(*watches, issue);
                }
                // Installation precedes callbacks so consumers can immediately write
                // into the newly created or renamed subtree without stale mappings.
                for (const auto& event : events) emit(event);
            }
        } catch (const std::exception& e) {
            rescan(*watches, std::string("Inotify worker failed: ") + e.what());
        } catch (...) {
            rescan(*watches, "Inotify worker failed with an unknown exception");
        }
        running.store(false);
        std::lock_guard<std::mutex> lock(mutex);
        wakeFd = -1;
        ::close(wake);
    }
};

LinuxInotifyWatcher::LinuxInotifyWatcher() : impl_(std::make_unique<Impl>()) {}
LinuxInotifyWatcher::~LinuxInotifyWatcher() { stop(); }

void LinuxInotifyWatcher::setRoots(std::vector<std::string> roots) {
    auto normalizedList = normalizedRoots(roots);
    bool restart = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->roots = std::move(normalizedList);
        restart = impl_->running.load();
        if (std::this_thread::get_id() == impl_->workerId) {
            impl_->reconfigure.store(true);
            impl_->wakeLocked();
            return;
        }
    }
    if (restart) { stop(); start(); }
}

void LinuxInotifyWatcher::setCallback(Callback callback) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->callback = std::move(callback);
}

bool LinuxInotifyWatcher::start() {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    if (std::this_thread::get_id() == impl_->workerId) {
        if (impl_->running.load()) return true;
        impl_->error = "Restart after callback stop requires an external start call";
        return false;
    }
    impl_->joined.wait(lock, [&] { return !impl_->joining; });
    if (impl_->running.load()) return true;
    if (impl_->worker.joinable()) {
        lock.unlock();
        stop();
        return start();
    }
    impl_->error.clear();
    if (impl_->roots.empty()) { impl_->error = "No watch roots configured"; return false; }
    auto watches = std::make_unique<WatchSet>();
    watches->roots = impl_->roots;
    if (!watches->open(impl_->error) || !watches->addRootsAndParents(impl_->error)) return false;
    const int wake = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake < 0) { impl_->error = systemError("Cannot initialize watcher wake descriptor"); return false; }
    impl_->wakeFd = wake;
    impl_->running.store(true);
    impl_->reconfigure.store(false);
    try {
        impl_->worker = std::thread([state = impl_.get(), watches = std::move(watches), wake]() mutable {
            state->run(std::move(watches), wake);
        });
        impl_->workerId = impl_->worker.get_id();
    } catch (const std::exception& e) {
        impl_->running.store(false);
        impl_->wakeFd = -1;
        ::close(wake);
        impl_->error = std::string("Cannot create inotify worker: ") + e.what();
        return false;
    }
    return true;
}

void LinuxInotifyWatcher::stop() {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->running.store(false);
    impl_->wakeLocked();
    if (std::this_thread::get_id() == impl_->workerId) return;
    impl_->joined.wait(lock, [&] { return !impl_->joining; });
    // A concurrent start may have acquired the state lock between another
    // stop's join completion and this waiter. Stop that current worker too.
    impl_->running.store(false);
    impl_->wakeLocked();
    if (!impl_->worker.joinable()) return;
    impl_->joining = true;
    auto worker = std::move(impl_->worker);
    lock.unlock();
    worker.join();
    lock.lock();
    impl_->workerId = {};
    impl_->joining = false;
    impl_->joined.notify_all();
}

std::string LinuxInotifyWatcher::lastError() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->error;
}

} // namespace everything_lite
#endif
