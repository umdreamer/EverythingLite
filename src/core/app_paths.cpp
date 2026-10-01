#include "core/app_paths.h"

#include <cstdlib>
#include <filesystem>

namespace everything_lite {
namespace {

std::string envValue(const char* name) {
    if (const char* value = std::getenv(name)) {
        if (*value) return value;
    }
    return {};
}

std::filesystem::path homeDirectory() {
#if defined(_WIN32)
    auto home = envValue("USERPROFILE");
    if (!home.empty()) return std::filesystem::path(home);
    const auto drive = envValue("HOMEDRIVE");
    const auto path = envValue("HOMEPATH");
    if (!drive.empty() && !path.empty()) return std::filesystem::path(drive + path);
#else
    auto home = envValue("HOME");
    if (!home.empty()) return std::filesystem::path(home);
#endif
    return std::filesystem::current_path();
}

std::string pathString(const std::filesystem::path& path) {
#if defined(_WIN32)
    return path.u8string();
#else
    return path.string();
#endif
}

} // namespace

std::vector<std::string> databasePathCandidates() {
    if (const auto explicit_path = envValue("EVERYTHING_LITE_DB"); !explicit_path.empty()) {
        return {explicit_path};
    }

    const auto home = homeDirectory();
    std::vector<std::string> candidates;

#if defined(__APPLE__)
    const auto app_support = home / "Library" / "Application Support";
    // QStandardPaths::AppLocalDataLocation for the existing macOS GUI normally
    // resolves to the first path. The extra candidates preserve databases from
    // experimental builds that included the organization name.
    candidates.push_back(pathString(app_support / "Everything Lite" / "everything-lite.db"));
    candidates.push_back(pathString(app_support / "CICHI" / "Everything Lite" / "everything-lite.db"));
    candidates.push_back(pathString(app_support / "CICHI" / "EverythingLite" / "everything-lite.db"));
#elif defined(_WIN32)
    auto local_app_data = envValue("LOCALAPPDATA");
    if (!local_app_data.empty()) {
        candidates.push_back(pathString(std::filesystem::path(local_app_data) / "Everything Lite" / "everything-lite.db"));
    }
    candidates.push_back(pathString(home / "AppData" / "Local" / "Everything Lite" / "everything-lite.db"));
#else
    auto xdg = envValue("XDG_DATA_HOME");
    const auto data_home = xdg.empty() ? home / ".local" / "share" : std::filesystem::path(xdg);
    candidates.push_back(pathString(data_home / "Everything Lite" / "everything-lite.db"));
#endif

    return candidates;
}

std::string defaultDatabasePath() {
    const auto candidates = databasePathCandidates();
    if (candidates.empty()) {
        return pathString(std::filesystem::current_path() / ".everything-lite.db");
    }

    // Prefer an already-existing database so v0.4.1 never silently creates a
    // new empty database beside the user's established index.
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(std::filesystem::path(candidate), ec)) {
            return candidate;
        }
    }
    return candidates.front();
}

} // namespace everything_lite
