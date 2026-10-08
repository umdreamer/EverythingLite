#pragma once
#include "core/source_scan.h"
namespace everything_lite {
// Immutable job capture: no catalog or environment lookup during serialization.
struct ScanTask {
    const SourceScanRequest scan;
    // Persistent source-{id}.scan.lock in a caller-owned 0700 directory,
    // outside the source root and independent of per-job cleanup. Never unlink.
    const std::string lock_path;
    ScanTask(SourceScanRequest request, std::string persistent_lock_path);
};
void writeScanTask(const std::string& manifest_path, const ScanTask& task);
ScanTask readScanTask(const std::string& manifest_path);
class SourceScanLock {
public:
    explicit SourceScanLock(const ScanTask& task);
    ~SourceScanLock();
    SourceScanLock(const SourceScanLock&) = delete;
    SourceScanLock& operator=(const SourceScanLock&) = delete;
    // Transfer the FD to process lifetime before scanning/output. The OS closes
    // it at actual exit, including while stdio flush/atexit handlers block.
    void releaseToProcess() noexcept;
private:
    int fd_ = -1;
};
} // namespace everything_lite
