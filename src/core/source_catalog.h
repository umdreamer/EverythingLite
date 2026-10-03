#pragma once
#include <cstdint>
#include <string>
namespace everything_lite {
enum class SourceType { Local, NAS, Cloud, USB, External, Unknown };
enum class IndexMode { Realtime, Periodic };
enum class SourceStatus { Accessible, Offline, Unverified, Failed };
enum class CloudCopy { LocalCopy, RemoteOnly, Unknown };
// Identity is stable source evidence. A fingerprint describes a particular mount;
// neither is automatically fabricated from a path or a device number.
struct SourceRecord {
    std::int64_t id = 0;
    std::string label;
    SourceType type = SourceType::Unknown;
    IndexMode mode = IndexMode::Periodic;
    SourceStatus status = SourceStatus::Unverified;
    std::int64_t interval_seconds = 900; // zero means manual refresh
    std::string identity;
    std::string root;
    std::string mount_fingerprint;
    std::int64_t last_check = 0;
    std::int64_t last_success = 0;
    std::int64_t revision = 0;
    // Nonpersistent scan guard captured with source()/sources().
    std::int64_t policy_revision = 0;
};
void validateSource(const SourceRecord& source);
} // namespace everything_lite
