#pragma once

#include "platform/file_watcher.h"
#include <memory>

namespace everything_lite {
std::unique_ptr<FileWatcher> createPlatformWatcher();
}
