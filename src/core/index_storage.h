#pragma once
#include <atomic>
#include <cstdint>
#include <string>
namespace everything_lite {
struct IndexStorage {
    std::uint64_t main_bytes=0, wal_bytes=0, shm_bytes=0;
    std::uint64_t page_size=0, page_count=0, freelist_bytes=0;
    std::uint64_t backup_bytes=0, backup_count=0, total_bytes=0;
};
struct IndexCompaction {
    IndexStorage before, after;
    std::uint64_t backup_bytes=0;
};
// No initialization, schema migration, row scans, or source-root access.
IndexStorage readIndexStorage(const std::string& path);
// Keeps the verified backup. Failure never restores over an active database.
// after includes the explicit backup even when outside the report naming rule.
IndexCompaction compactIndexStorage(const std::string& path, const std::string& backup_path,
                                    std::atomic_bool* cancel=nullptr);
} // namespace everything_lite
