#include "platform/watcher_factory.h"

#ifdef __APPLE__
#include "platform/mac_fsevents_watcher.h"
#else
#include "platform/null_watcher.h"
#endif

namespace everything_lite {

std::unique_ptr<FileWatcher> createPlatformWatcher() {
#ifdef __APPLE__
    return std::make_unique<MacFSEventsWatcher>();
#else
    return std::make_unique<NullWatcher>();
#endif
}

} // namespace everything_lite
