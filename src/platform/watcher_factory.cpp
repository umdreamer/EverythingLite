#include "platform/watcher_factory.h"

#ifdef __APPLE__
#include "platform/mac_fsevents_watcher.h"
#elif defined(__linux__)
#include "platform/linux_inotify_watcher.h"
#else
#include "platform/null_watcher.h"
#endif

namespace everything_lite {

std::unique_ptr<FileWatcher> createPlatformWatcher() {
#ifdef __APPLE__
    return std::make_unique<MacFSEventsWatcher>();
#elif defined(__linux__)
    return std::make_unique<LinuxInotifyWatcher>();
#else
    return std::make_unique<NullWatcher>();
#endif
}

} // namespace everything_lite
