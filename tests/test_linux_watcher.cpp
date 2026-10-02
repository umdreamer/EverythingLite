#include "platform/watcher_factory.h"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using everything_lite::FileEvent;

namespace {
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

struct TempTree {
    fs::path path;
    TempTree() {
        auto pattern = (fs::temp_directory_path() / "everything-lite-watcher-XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        const auto* created = ::mkdtemp(name.data());
        require(created != nullptr, "cannot create independent temporary tree");
        path = created;
    }
    ~TempTree() { std::error_code ec; fs::remove_all(path, ec); }
};

struct Events {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<FileEvent> values;
    void add(const FileEvent& event) {
        std::lock_guard<std::mutex> lock(mutex);
        values.push_back(event);
        changed.notify_all();
    }
    std::size_t mark() {
        std::lock_guard<std::mutex> lock(mutex);
        return values.size();
    }
    bool waitPath(const fs::path& path, std::size_t since) {
        std::unique_lock<std::mutex> lock(mutex);
        auto next = since;
        return changed.wait_for(lock, 5s, [&] {
            for (; next < values.size(); ++next)
                if (values[next].path == path.string() && !values[next].needs_full_rescan) return true;
            return false;
        });
    }
    bool waitRescan(std::size_t since) {
        std::unique_lock<std::mutex> lock(mutex);
        auto next = since;
        return changed.wait_for(lock, 5s, [&] {
            for (; next < values.size(); ++next)
                if (values[next].needs_full_rescan) return true;
            return false;
        });
    }
    bool waitError(std::size_t since) {
        std::unique_lock<std::mutex> lock(mutex);
        auto next = since;
        return changed.wait_for(lock, 5s, [&] {
            for (; next < values.size(); ++next)
                if (!values[next].error.empty() && values[next].needs_full_rescan) return true;
            return false;
        });
    }
    bool waitHealthyRescan(std::size_t since) {
        std::unique_lock<std::mutex> lock(mutex);
        auto next = since;
        return changed.wait_for(lock, 5s, [&] {
            for (; next < values.size(); ++next)
                if (values[next].error.empty() && values[next].needs_full_rescan) return true;
            return false;
        });
    }
    bool absent(const fs::path& path, std::size_t since) {
        std::unique_lock<std::mutex> lock(mutex);
        auto next = since;
        return !changed.wait_for(lock, 250ms, [&] {
            for (; next < values.size(); ++next)
                if (values[next].path == path.string()) return true;
            return false;
        });
    }
    std::size_t countPath(const fs::path& path, std::size_t since) {
        std::lock_guard<std::mutex> lock(mutex);
        std::size_t count = 0;
        for (std::size_t i = since; i < values.size(); ++i)
            if (values[i].path == path.string() && !values[i].needs_full_rescan) ++count;
        return count;
    }
};

void write(const fs::path& path, const std::string& content = "synthetic") {
    std::ofstream file(path, std::ios::app);
    file << content;
    file.close();
    require(file.good(), "cannot write synthetic file");
}

void behaviorTests() {
    TempTree tree;
    const auto root = tree.path / "root";
    const auto other = tree.path / "other";
    fs::create_directories(root / "existing" / "nested");
    fs::create_directories(other);
    Events events;
    auto watcher = everything_lite::createPlatformWatcher();
    watcher->setRoots({root.string(), (root / "existing").string(), root.string()});
    watcher->setCallback([&](const FileEvent& event) { events.add(event); });
    require(watcher->start(), "watcher start failed");

    auto mark = events.mark();
    const auto sample = root / "sample.txt";
    write(sample);
    require(events.waitPath(sample, mark), "creation of sample.txt was not delivered within 5 seconds");
    require(events.waitRescan(0), "initial installation must request a compensation scan");

    for (const auto& name : {std::string(u8"砀例甲"), std::string(u8"示例工匠")}) {
        mark = events.mark();
        const auto file = root / "existing" / "nested" / name;
        write(file);
        require(events.waitPath(file, mark), "UTF-8 path event missing");
        std::this_thread::sleep_for(100ms); // Let creation and close-write notifications drain.
        mark = events.mark();
        require(::chmod(file.c_str(), 0600) == 0, "chmod failed");
        require(events.waitPath(file, mark), "attribute event missing");
        std::this_thread::sleep_for(100ms);
        require(events.countPath(file, mark) == 1, "overlapping roots duplicated an attribute event");
        mark = events.mark();
        fs::remove(file);
        require(events.waitPath(file, mark), "deletion event missing");
    }

    mark = events.mark();
    fs::create_directories(root / "new" / "deep");
    write(root / "new" / "deep" / "immediate.txt");
    require(events.waitRescan(mark), "new subtree must request scan for installation race");
    require(events.waitPath(root / "new", mark), "new directory event missing");
    mark = events.mark();
    write(root / "new" / "deep" / "later.txt");
    require(events.waitPath(root / "new" / "deep" / "later.txt", mark), "new recursive directory is not watched");

    mark = events.mark();
    fs::rename(root / "new", root / "renamed");
    write(root / "renamed" / "deep" / "racing.txt");
    require(events.waitPath(root / "renamed", mark), "directory destination event missing");
    require(events.absent(root / "new" / "deep" / "racing.txt", mark), "rename race emitted a stale subtree path");
    mark = events.mark();
    write(root / "renamed" / "deep" / "after-rename.txt");
    require(events.waitPath(root / "renamed" / "deep" / "after-rename.txt", mark), "moved subtree retained stale watch paths");

    fs::create_directory_symlink(other, root / "link");
    mark = events.mark();
    write(other / "outside.txt");
    require(events.absent(root / "link" / "outside.txt", mark), "symlink target was followed");
    require(events.absent(other / "outside.txt", mark), "unconfigured external target was watched");

    mark = events.mark();
    fs::rename(root / "renamed", other / "moved-out");
    require(events.waitPath(root / "renamed", mark), "move-out event missing");
    mark = events.mark();
    write(other / "moved-out" / "deep" / "external.txt");
    require(events.absent(root / "renamed" / "deep" / "external.txt", mark), "moved-out subtree kept stale watch");
    mark = events.mark();
    fs::rename(other / "moved-out", root / "moved-in");
    require(events.waitPath(root / "moved-in", mark), "move-in event missing");
    mark = events.mark();
    write(root / "moved-in" / "deep" / "returned.txt");
    require(events.waitPath(root / "moved-in" / "deep" / "returned.txt", mark), "moved-in subtree not watched");

    mark = events.mark();
    fs::remove_all(root);
    require(events.waitPath(root, mark), "root deletion event missing");
    require(events.waitRescan(mark), "root loss must request compensation scan");
    std::this_thread::sleep_for(100ms); // Drain the deletion batch before testing recreation.
    mark = events.mark();
    fs::create_directories(root / "reborn");
    require(events.waitRescan(mark), "root reconstruction must finish installing recursive watches");
    mark = events.mark();
    write(root / "reborn" / "root-return.txt");
    require(events.waitPath(root / "reborn" / "root-return.txt", mark), "root reconstruction not monitored");
    mark = events.mark();
    write(root / "reborn" / "stable.txt");
    require(events.waitPath(root / "reborn" / "stable.txt", mark), "reconstructed root has no stable recursive watch");

    watcher->stop();
    mark = events.mark();
    write(root / "stopped.txt");
    require(events.absent(root / "stopped.txt", mark) && events.mark() == mark, "callbacks continued after stop completed");
    require(watcher->start(), "restart failed");
    mark = events.mark();
    write(root / "restarted.txt");
    require(events.waitPath(root / "restarted.txt", mark), "restart event missing");
    watcher->setRoots({other.string()});
    mark = events.mark();
    write(other / "new-root.txt");
    require(events.waitPath(other / "new-root.txt", mark), "setRoots failed to restart monitoring");
    mark = events.mark();
    write(root / "old-root.txt");
    require(events.absent(root / "old-root.txt", mark), "old root remains monitored after setRoots");

    watcher->setCallback([&](const FileEvent& event) {
        events.add(event);
        watcher->setCallback([&](const FileEvent& next) { events.add(next); });
        throw std::runtime_error("synthetic callback exception");
    });
    mark = events.mark();
    write(other / "throwing.txt");
    require(events.waitPath(other / "throwing.txt", mark), "reentrant callback was not dispatched");
    mark = events.mark();
    write(other / "after-throw.txt");
    require(events.waitPath(other / "after-throw.txt", mark), "callback exception stopped worker");
    watcher->stop();

    auto invalid = everything_lite::createPlatformWatcher();
    require(!invalid->start(), "empty root configuration must fail");
    require(!invalid->lastError().empty(), "empty root failure has no diagnostic");
    invalid->setRoots({(root / "old-root.txt").string()});
    require(!invalid->start(), "regular-file root must fail");
    require(!invalid->lastError().empty(), "regular-file failure has no diagnostic");
    fs::create_directory_symlink(other, tree.path / "root-link");
    invalid->setRoots({(tree.path / "root-link").string()});
    require(!invalid->start(), "symlink root must not be followed");
    require(!invalid->lastError().empty(), "symlink failure has no diagnostic");
    fs::create_directories(other / "nested");
    invalid->setRoots({(tree.path / "root-link" / "nested").string()});
    require(!invalid->start(), "symlink ancestor must not be followed");
}

std::size_t descriptorCount() {
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator("/proc/self/fd")) { (void)entry; ++count; }
    return count;
}

void lifecycleAndFailureTests() {
    TempTree tree;
    Events events;
    auto watcher = everything_lite::createPlatformWatcher();
    watcher->setRoots({tree.path.string()});
    const auto selfStop = tree.path / "self-stop.txt";
    watcher->setCallback([&](const FileEvent& event) {
        if (event.path == selfStop.string()) watcher->stop();
        events.add(event);
    });
    require(watcher->start(), "self-stop watcher start failed");
    auto mark = events.mark();
    write(selfStop);
    require(events.waitPath(selfStop, mark), "callback stop deadlocked");
    watcher->stop();
    watcher->setCallback([&](const FileEvent& event) { events.add(event); });
    require(watcher->start(), "restart after callback stop failed");
    mark = events.mark();
    write(tree.path / "self-stop-restarted.txt");
    require(events.waitPath(tree.path / "self-stop-restarted.txt", mark), "restart after callback stop lost monitoring");
    watcher->stop();

    const auto before = descriptorCount();
    for (int i = 0; i < 20; ++i) {
        require(watcher->start(), "lifecycle stress start failed");
        std::thread first([&] { watcher->stop(); });
        std::thread second([&] { watcher->stop(); });
        std::thread third([&] { watcher->start(); });
        first.join();
        second.join();
        third.join();
        watcher->stop();
    }
    require(descriptorCount() == before, "watcher lifecycle leaked file descriptors");

    rlimit saved{};
    require(::getrlimit(RLIMIT_NOFILE, &saved) == 0, "cannot inspect descriptor resource limit");
    rlimit exhausted = saved;
    exhausted.rlim_cur = 0;
    require(::setrlimit(RLIMIT_NOFILE, &exhausted) == 0, "cannot set bounded descriptor failure fixture");
    const bool started = watcher->start();
    const auto diagnostic = watcher->lastError();
    const bool restored = ::setrlimit(RLIMIT_NOFILE, &saved) == 0;
    require(restored, "cannot restore descriptor resource limit");
    require(!started && !diagnostic.empty(), "descriptor exhaustion silently claimed coverage");

    const auto restricted = tree.path / "restricted";
    fs::create_directory(restricted);
    require(::chmod(tree.path.c_str(), 0755) == 0, "cannot prepare permission fixture parent");
    require(::chmod(restricted.c_str(), 0000) == 0, "cannot prepare permission fixture root");
    const auto child = ::fork();
    require(child >= 0, "cannot fork permission fixture");
    if (child == 0) {
        if (::geteuid() == 0 && ::setuid(65534) != 0) ::_exit(2);
        auto denied = everything_lite::createPlatformWatcher();
        denied->setRoots({restricted.string()});
        const bool rejected = !denied->start() && !denied->lastError().empty();
        denied->stop();
        ::_exit(rejected ? 0 : 3);
    }
    int childStatus = 0;
    require(::waitpid(child, &childStatus, 0) == child, "cannot wait for permission fixture");
    require(::chmod(restricted.c_str(), 0700) == 0, "cannot restore permission fixture");
    require(WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 0, "permission denial silently claimed coverage");

    require(::chmod(restricted.c_str(), 0755) == 0, "cannot prepare runtime permission fixture");
    const auto inaccessible = restricted / "nested";
    fs::create_directory(inaccessible);
    require(::chmod(inaccessible.c_str(), 0000) == 0, "cannot prepare inaccessible subtree");
    if (::geteuid() == 0) {
        require(::chown(restricted.c_str(), 65534, 65534) == 0, "cannot prepare permission fixture owner");
        require(::chown(inaccessible.c_str(), 65534, 65534) == 0, "cannot prepare subtree fixture owner");
    }
    const auto runtimeChild = ::fork();
    require(runtimeChild >= 0, "cannot fork runtime permission fixture");
    if (runtimeChild == 0) {
        if (::geteuid() == 0 && ::setuid(65534) != 0) ::_exit(2);
        Events failures;
        auto degraded = everything_lite::createPlatformWatcher();
        degraded->setRoots({restricted.string()});
        degraded->setCallback([&](const FileEvent& event) { failures.add(event); });
        const bool observable = degraded->start() && failures.waitError(0) && !degraded->lastError().empty();
        if (!observable) { degraded->stop(); ::_exit(4); }
        auto failureMark = failures.mark();
        fs::create_directory(restricted / "visible");
        if (!failures.waitRescan(failureMark) || degraded->lastError().empty()) {
            degraded->stop();
            ::_exit(5); // Adding a readable subtree must not clear another subtree's failure.
        }
        failureMark = failures.mark();
        if (::chmod(inaccessible.c_str(), 0700) != 0) { degraded->stop(); ::_exit(6); }
        std::atomic_bool keepWriting{true};
        std::thread load([&] {
            while (keepWriting.load()) {
                write(restricted / "continuous.txt");
                std::this_thread::sleep_for(1ms);
            }
        });
        const bool recovered = failures.waitHealthyRescan(failureMark);
        keepWriting.store(false);
        load.join();
        if (!recovered || !degraded->lastError().empty()) { degraded->stop(); ::_exit(7); }
        failureMark = failures.mark();
        write(inaccessible / "recovered.txt");
        const bool installed = failures.waitPath(inaccessible / "recovered.txt", failureMark);
        degraded->stop();
        ::_exit(installed ? 0 : 8);
    }
    require(::waitpid(runtimeChild, &childStatus, 0) == runtimeChild, "cannot wait for runtime permission fixture");
    require(::chmod(inaccessible.c_str(), 0700) == 0, "cannot restore inaccessible subtree");
    require(WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 0, "recursive coverage error/recovery fixture failed: code=" + std::to_string(WEXITSTATUS(childStatus)));

    watcher->setRoots({(tree.path / "missing" / "root").string()});
    mark = events.mark();
    require(watcher->start(), "initially missing directory root should monitor its ancestor");
    require(events.waitRescan(mark), "missing-root setup had no initial compensation scan");
    mark = events.mark();
    fs::create_directories(tree.path / "missing" / "root" / "child");
    require(events.waitRescan(mark), "missing root did not recover after ancestor creation");
    mark = events.mark();
    write(tree.path / "missing" / "root" / "child" / "created.txt");
    require(events.waitPath(tree.path / "missing" / "root" / "child" / "created.txt", mark), "initially missing root was not monitored");
    watcher->stop();
}

void queuedRenameTest(bool movingOut) {
    TempTree tree;
    TempTree external;
    fs::create_directories(tree.path / "before" / "deep");
    Events events;
    std::mutex gateMutex;
    std::condition_variable gateChanged;
    bool blocked = false, released = false;
    const auto trigger = tree.path / "gate.txt";
    auto watcher = everything_lite::createPlatformWatcher();
    watcher->setRoots({tree.path.string()});
    watcher->setCallback([&](const FileEvent& event) {
        events.add(event);
        if (event.path == trigger.string() && !event.needs_full_rescan) {
            std::unique_lock<std::mutex> lock(gateMutex);
            if (!released) {
                blocked = true;
                gateChanged.notify_all();
                gateChanged.wait(lock, [&] { return released; });
            }
        }
    });
    require(watcher->start(), "queued-rename watcher failed");
    require(events.waitRescan(0), "queued-rename initial scan missing");
    write(trigger);
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        if (!gateChanged.wait_for(lock, 5s, [&] { return blocked; })) {
            released = true;
            gateChanged.notify_all();
            throw std::runtime_error("queued-rename callback gate not reached");
        }
    }
    const auto mark = events.mark();
    const auto destination = (movingOut ? external.path : tree.path) / "after";
    try {
        fs::rename(tree.path / "before", destination);
        fs::create_directory(destination / "deep" / "newdir");
        write(destination / "deep" / "racing.txt");
    } catch (...) {
        std::lock_guard<std::mutex> lock(gateMutex);
        released = true;
        gateChanged.notify_all();
        throw;
    }
    {
        std::lock_guard<std::mutex> lock(gateMutex);
        released = true;
        gateChanged.notify_all();
    }
    require(events.waitPath(movingOut ? tree.path / "before" : destination, mark), "queued rename transition missing");
    require(events.absent(tree.path / "before" / "deep" / "racing.txt", mark), "queued rename/write emitted stale path");
    require(events.absent(tree.path / "before" / "deep" / "newdir", mark), "queued move emitted stale directory path");
    watcher->stop();
}

void ancestorMoveTest() {
    TempTree tree;
    const auto root = tree.path / "a" / "b" / "root";
    fs::create_directories(root);
    Events events;
    auto watcher = everything_lite::createPlatformWatcher();
    watcher->setRoots({root.string()});
    watcher->setCallback([&](const FileEvent& event) { events.add(event); });
    require(watcher->start(), "ancestor-move watcher start failed");
    require(events.waitRescan(0), "ancestor-move initial scan missing");
    auto mark = events.mark();
    fs::rename(tree.path / "a", tree.path / "moved");
    write(tree.path / "moved" / "b" / "root" / "outside.txt");
    require(events.absent(root / "outside.txt", mark), "moving a root ancestor emitted stale file path");
    require(events.waitRescan(mark), "moving a root ancestor did not request compensation");
    mark = events.mark();
    fs::create_directories(root);
    require(events.waitRescan(mark), "root location did not recover after ancestor reconstruction");
    mark = events.mark();
    write(root / "restored.txt");
    require(events.waitPath(root / "restored.txt", mark), "ancestor reconstruction lost root monitoring");
    watcher->stop();
}

void overflowTest() {
    std::ifstream limitFile("/proc/sys/fs/inotify/max_queued_events");
    std::size_t queueLimit = 0;
    limitFile >> queueLimit;
    require(queueLimit > 0, "cannot read inotify queue limit");
    if (queueLimit > 65536) {
        std::cout << "Overflow stress skipped: queue limit exceeds bounded test budget\n";
        return;
    }
    TempTree tree;
    Events events;
    std::mutex gateMutex;
    std::condition_variable gateChanged;
    bool blocked = false;
    bool released = false;
    const auto trigger = tree.path / "block.txt";
    auto watcher = everything_lite::createPlatformWatcher();
    watcher->setRoots({tree.path.string()});
    watcher->setCallback([&](const FileEvent& event) {
        events.add(event);
        if (event.path == trigger.string() && !event.needs_full_rescan) {
            std::unique_lock<std::mutex> lock(gateMutex);
            if (!released) {
                blocked = true;
                gateChanged.notify_all();
                gateChanged.wait(lock, [&] { return released; });
            }
        }
    });
    require(watcher->start(), "overflow watcher start failed");
    require(events.waitRescan(0), "overflow watcher initial scan missing");
    write(trigger);
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        if (!gateChanged.wait_for(lock, 5s, [&] { return blocked; })) {
            released = true;
            gateChanged.notify_all();
            throw std::runtime_error("overflow callback gate was not reached");
        }
    }
    const auto mark = events.mark();
    try {
        // Unique names avoid the kernel coalescing identical adjacent events.
        for (std::size_t i = 0; i < queueLimit + 64; ++i)
            write(tree.path / ("overflow-" + std::to_string(i) + ".txt"));
    } catch (...) {
        std::lock_guard<std::mutex> lock(gateMutex);
        released = true;
        gateChanged.notify_all();
        throw;
    }
    {
        std::lock_guard<std::mutex> lock(gateMutex);
        released = true;
        gateChanged.notify_all();
    }
    require(events.waitRescan(mark), "actual queue overflow did not request compensation scan");
    const auto stableMark = events.mark();
    const auto stable = tree.path / "after-overflow.txt";
    write(stable);
    require(events.waitPath(stable, stableMark), "queue overflow did not reestablish monitoring; diagnostic=" + watcher->lastError());
    watcher->stop();
}
} // namespace

int main() {
    try {
        behaviorTests();
        std::cout << "Recursive, move, root recovery, and callback behavior passed" << std::endl;
        lifecycleAndFailureTests();
        std::cout << "Concurrent lifecycle and failure reporting passed" << std::endl;
        queuedRenameTest(false);
        queuedRenameTest(true);
        ancestorMoveTest();
        overflowTest();
        std::cout << "Linux watcher behavior tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Linux watcher behavior test failed: " << error.what() << '\n';
        return 1;
    }
}
