#pragma once
#include <cstdint>
#include <string>
namespace everything_lite {
struct DirectorySyncPermissions {
    bool accessibility = false;
    std::string finder_status = "unavailable";
    std::int32_t finder_error = 0;
};
DirectorySyncPermissions queryDirectorySyncPermissions(bool request);
int runDirectorySyncPermissionHelper(bool request);
}
