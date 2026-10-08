#pragma once
#include "core/scanner.h"
#include "core/source_probe.h"
namespace everything_lite {
// Captured by value: no catalog/policy lookup or identity binding occurs here.
struct SourceScanRequest {
    SourceRecord source;
    ExclusionPolicy policy;
    std::vector<std::string> nested_source_roots;
    std::string scan_path;
    // New leaf in an existing caller-owned 0700 directory, outside source root.
    // Existing files/links and output inside the source are rejected.
    std::string snapshot_path;
    std::string nonce;
};
struct SourceScanResult {
    IndexStats stats; // indexed stays zero: this module never writes the main DB.
    ProbeResult before, after;
    std::string actual_scan_path, subtree_relative;
    // Nonempty only after complete=1 and all SQLite writers/descriptors close.
    // Caller owns cleanup; an interrupted request can leave complete=0 at its
    // explicit output path. Never remove an existing/replaced user file.
    std::string snapshot_path;
    std::uint64_t staged=0, batches=0;
    std::size_t max_batch=0;
};
using SourceScanProgress = std::function<void(std::uint64_t)>;
SourceScanResult scanSourceToSnapshot(SourceScanRequest request,
    const std::atomic_bool* cancelled=nullptr, const SourceScanProgress& progress={});
} // namespace everything_lite
