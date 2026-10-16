#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <functional>
namespace everything_lite {
struct IndexStorage {
    std::uint64_t main_bytes=0, wal_bytes=0, shm_bytes=0;
    std::uint64_t page_size=0, page_count=0, freelist_bytes=0;
    std::uint64_t backup_bytes=0, backup_count=0, total_bytes=0;
};
// A persistent, process-wide advisory lock for maintenance/backup cleanup.
// The lock file is never unlinked: unlinking a held inode would admit a second owner.
class IndexMaintenanceLock {
public:
    explicit IndexMaintenanceLock(const std::string& database_path);
    ~IndexMaintenanceLock();
    IndexMaintenanceLock(const IndexMaintenanceLock&)=delete;
    IndexMaintenanceLock& operator=(const IndexMaintenanceLock&)=delete;
private:
    std::intptr_t handle_=-1;
};
enum class IndexStoragePhase { Backup, Migrate, Vacuum, Verify };
using IndexStorageProgress=std::function<void(IndexStoragePhase)>;
struct IndexOptimization {
    IndexStorage before,after;
    std::uint64_t backup_bytes=0;
    std::string backup_path,error;
    bool already_compact=false,migration_committed=false,vacuum_completed=false;
};
IndexOptimization migrateIndexStorageToDirectory(const std::string& path,const std::string& backup_path,
    bool vacuum=true,std::atomic_bool* cancel=nullptr,const IndexStorageProgress& progress={});
struct IndexCompaction {
    IndexStorage before, after;
    std::uint64_t backup_bytes=0;
};
// No initialization, schema migration, row scans, or source-root access.
IndexStorage readIndexStorage(const std::string& path);
// Keeps the verified backup. Failure never restores over an active database.
// after includes the explicit backup even when outside the report naming rule.
// Source/backup names must not end in -journal/-wal/-shm (ASCII case-insensitive).
IndexCompaction compactIndexStorage(const std::string& path, const std::string& backup_path,
                                    std::atomic_bool* cancel=nullptr);
} // namespace everything_lite
