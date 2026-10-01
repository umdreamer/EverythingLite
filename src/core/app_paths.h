#pragma once

#include <string>
#include <vector>

namespace everything_lite {

// Resolve the default SQLite database used by both GUI and CLI.
// EVERYTHING_LITE_DB always wins. On macOS we preserve existing databases
// created by earlier GUI releases before falling back to the canonical path.
std::string defaultDatabasePath();
std::vector<std::string> databasePathCandidates();

} // namespace everything_lite
