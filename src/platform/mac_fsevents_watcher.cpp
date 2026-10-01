#ifdef __APPLE__

#include "platform/mac_fsevents_watcher.h"
#include "core/path_utils.h"

#include <algorithm>
#include <iostream>

namespace everything_lite {

MacFSEventsWatcher::~MacFSEventsWatcher() {
    stop();
}

void MacFSEventsWatcher::setRoots(std::vector<std::string> roots) {
    const bool restart = running_.load();
    if (restart) stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        roots_.clear();
        for (auto& root : roots) roots_.push_back(normalizePath(root));
    }
    if (restart) start();
}

void MacFSEventsWatcher::setCallback(Callback callback_fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback_fn);
}

bool MacFSEventsWatcher::start() {
    if (running_.exchange(true)) return true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (roots_.empty()) {
            running_ = false;
            return false;
        }
    }
    thread_ = std::thread([this] { run(); });
    return true;
}

void MacFSEventsWatcher::stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (run_loop_) CFRunLoopStop(run_loop_);
    }
    if (thread_.joinable()) thread_.join();
}

void MacFSEventsWatcher::callback(ConstFSEventStreamRef,
                                  void* client_info,
                                  std::size_t num_events,
                                  void* event_paths,
                                  const FSEventStreamEventFlags event_flags[],
                                  const FSEventStreamEventId[]) {
    auto* self = static_cast<MacFSEventsWatcher*>(client_info);
    auto** paths = static_cast<char**>(event_paths);

    Callback cb;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        cb = self->callback_;
    }
    if (!cb) return;

    constexpr FSEventStreamEventFlags rescan_flags =
        kFSEventStreamEventFlagMustScanSubDirs |
        kFSEventStreamEventFlagUserDropped |
        kFSEventStreamEventFlagKernelDropped |
        kFSEventStreamEventFlagEventIdsWrapped |
        kFSEventStreamEventFlagRootChanged;

    for (std::size_t i = 0; i < num_events; ++i) {
        FileEvent event;
        event.path = paths[i] ? normalizePath(paths[i]) : std::string{};
        event.needs_full_rescan = (event_flags[i] & rescan_flags) != 0;
        cb(event);
    }
}

void MacFSEventsWatcher::run() {
    std::vector<std::string> roots;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        roots = roots_;
    }

    CFMutableArrayRef paths_to_watch = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    for (const auto& root : roots) {
        CFStringRef path = CFStringCreateWithCString(kCFAllocatorDefault, root.c_str(), kCFStringEncodingUTF8);
        if (path) {
            CFArrayAppendValue(paths_to_watch, path);
            CFRelease(path);
        }
    }

    FSEventStreamContext context{};
    context.info = this;
    const auto flags = static_cast<FSEventStreamCreateFlags>(
        kFSEventStreamCreateFlagFileEvents |
        kFSEventStreamCreateFlagWatchRoot |
        kFSEventStreamCreateFlagNoDefer);

    FSEventStreamRef stream = FSEventStreamCreate(
        kCFAllocatorDefault,
        &MacFSEventsWatcher::callback,
        &context,
        paths_to_watch,
        kFSEventStreamEventIdSinceNow,
        0.35,
        flags);
    CFRelease(paths_to_watch);

    if (!stream) {
        running_ = false;
        return;
    }

    CFRunLoopRef loop = CFRunLoopGetCurrent();
    CFRetain(loop);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        run_loop_ = loop;
    }

    FSEventStreamScheduleWithRunLoop(stream, loop, kCFRunLoopDefaultMode);
    if (running_.load() && FSEventStreamStart(stream)) {
        CFRunLoopRun();
        FSEventStreamStop(stream);
    }
    FSEventStreamInvalidate(stream);
    FSEventStreamRelease(stream);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        run_loop_ = nullptr;
    }
    CFRelease(loop);
    running_ = false;
}

} // namespace everything_lite

#endif
