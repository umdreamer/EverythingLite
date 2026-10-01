#ifdef __APPLE__

#include "platform/mac_fsevents_watcher.h"
#include "core/path_utils.h"

#include <algorithm>
#include <utility>

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

    std::vector<std::string> roots;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        roots = roots_;
    }
    if (roots.empty()) {
        running_ = false;
        return false;
    }

    CFMutableArrayRef paths_to_watch =
        CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    if (!paths_to_watch) {
        running_ = false;
        return false;
    }

    for (const auto& root : roots) {
        CFStringRef path = CFStringCreateWithCString(
            kCFAllocatorDefault, root.c_str(), kCFStringEncodingUTF8);
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
        return false;
    }

    dispatch_queue_t queue = dispatch_queue_create(
        "org.cichi.everything-lite.fsevents", DISPATCH_QUEUE_SERIAL);
    if (!queue) {
        FSEventStreamRelease(stream);
        running_ = false;
        return false;
    }

    // FSEventStreamScheduleWithRunLoop is deprecated since macOS 13.
    // Dispatch queues are Apple's current API for scheduling FSEvents streams.
    FSEventStreamSetDispatchQueue(stream, queue);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stream_ = stream;
        queue_ = queue;
    }

    if (!FSEventStreamStart(stream)) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stream_ = nullptr;
            queue_ = nullptr;
        }
        FSEventStreamInvalidate(stream);
        FSEventStreamRelease(stream);
#if !OS_OBJECT_USE_OBJC
        dispatch_release(queue);
#endif
        running_ = false;
        return false;
    }

    return true;
}

void MacFSEventsWatcher::stop() {
    if (!running_.exchange(false)) return;

    FSEventStreamRef stream = nullptr;
    dispatch_queue_t queue = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stream = stream_;
        queue = queue_;
        stream_ = nullptr;
        queue_ = nullptr;
    }

    if (stream) {
        FSEventStreamStop(stream);
        FSEventStreamInvalidate(stream);
        FSEventStreamRelease(stream);
    }
#if !OS_OBJECT_USE_OBJC
    if (queue) dispatch_release(queue);
#else
    (void)queue;
#endif
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
    if (!cb || !self->running_.load()) return;

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

} // namespace everything_lite

#endif
