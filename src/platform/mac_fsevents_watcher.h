#pragma once

#ifdef __APPLE__

#include "platform/file_watcher.h"

#include <CoreServices/CoreServices.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace everything_lite {

class MacFSEventsWatcher final : public FileWatcher {
public:
    MacFSEventsWatcher() = default;
    ~MacFSEventsWatcher() override;

    void setRoots(std::vector<std::string> roots) override;
    void setCallback(Callback callback) override;
    bool start() override;
    void stop() override;
    std::string backendName() const override { return "macOS FSEvents"; }

private:
    static void callback(ConstFSEventStreamRef stream,
                         void* client_info,
                         std::size_t num_events,
                         void* event_paths,
                         const FSEventStreamEventFlags event_flags[],
                         const FSEventStreamEventId event_ids[]);

    void run();

    std::vector<std::string> roots_;
    Callback callback_;
    std::thread thread_;
    std::atomic_bool running_{false};
    mutable std::mutex mutex_;
    CFRunLoopRef run_loop_ = nullptr;
};

} // namespace everything_lite

#endif
