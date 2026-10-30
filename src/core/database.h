#pragma once

#include "core/file_record.h"
#include "core/exclusion_policy.h"
#include "core/search_query.h"
#include "core/source_scan.h"
#include "core/source_verification.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct sqlite3;

namespace everything_lite {

// Read-only source-catalog readiness used by normal initialization and recovery
// validation. Does not initialize, repair, or scan application records.
bool sourceCatalogSchemaReady(sqlite3* db);

struct SourceOwnershipSnapshot {
    ExclusionPolicy policy;
    std::vector<SourceRecord> descendants;
};

class Database {
public:
    explicit Database(std::string db_path);

    const std::string& path() const { return db_path_; }
    void initialize() const;
    std::vector<SourceRecord> sources() const;
    SourceRecord source(std::int64_t id) const;
    std::int64_t ensureSourceForRoot(const std::string& root) const;
    std::int64_t saveSource(const SourceRecord& source) const;
    std::uint64_t forgetSource(std::int64_t id) const;
    // Captures literal descendants and policy in one read snapshot, validating
    // the owning source guard. No source filesystem or identity probing occurs.
    SourceOwnershipSnapshot captureSourceDescendants(const SourceRecord& expected) const;
    // Full snapshots require the captured source/policy guard to agree.
    // When provided, ownership/protection admission runs inside the main write
    // transaction. Offline or never-successful children retain parent cache.
    IndexStats mergeSourceSnapshot(const SourceRecord& expected,
                                  const std::vector<FileRecord>& records,
                                  const std::string& subtree_relative = {},
                                  const std::vector<SourceRecord>* expected_descendants = nullptr,
                                  const std::vector<std::string>* unreadable_paths = nullptr) const;
    // Read-only staging SQLite: snapshot_metadata(complete=1), snapshot_files
    // columns follow FileRecord (path,parent_path,name,ext,root,size,
    // modified_time,is_dir,scan_generation,source_id,relative_path,cloud_copy).
    // Staging writers must close/stop writing before handing the file over.
    IndexStats mergeSourceSnapshotFile(const SourceRecord& expected,
                                      const std::string& snapshot_database,
                                      const std::string& subtree_relative = {}) const;
    // One read transaction captures source, policy and strict descendant roots
    // by value. Empty scan_path means source.root; no source filesystem access,
    // identity binding or job/output files are created.
    SourceScanRequest captureSourceScanRequest(std::int64_t id,
        const std::string& scan_path, const std::string& snapshot_path,
        const std::string& nonce) const;
    // Production handoff: mandatory nonce/proof and fixed staging read view.
    // Caller must wait for actual helper exit and closed staging writers.
    // Source, policy and topology guards run in the main write transaction;
    // every failure rolls back all canonical changes, including prior batches.
    IndexStats mergeSourceScanSnapshot(const SourceScanRequest& captured) const;
    SourceVerificationResult publishSourceVerification(const SourceVerificationEvidence& evidence, VerificationMode mode) const;
    std::string migrationBackupPath() const;
    ExclusionPolicy exclusionPolicy() const;
    // Validates first; atomically replaces rules, increments revision and purges
    // excluded cached rows for all sources, including offline sources.
    using ExclusionProgress = std::function<void(std::uint64_t checked,std::uint64_t total)>;
    std::uint64_t saveExclusionPolicy(const ExclusionRules& rules,const ExclusionProgress& progress = {}) const;

    // Compatibility scans reserve a unique (root,generation) and advance the
    // source revision at begin/completion. Every published file mutation
    // advances that revision and refreshes only its own validated scan guard.
    // Modern snapshots and other writers invalidate stale scan generations.
    void beginRootScan(const std::string& root, std::int64_t generation) const;
    void completeRootScan(const std::string& root, std::int64_t generation, std::uint64_t file_count) const;
    std::int64_t currentGeneration(const std::string& root) const;

    // Without beginRootScan, one batch uses the current policy but has no
    // authority to delete stale rows or complete a full scan. Changed standalone
    // batches invalidate previously captured snapshots and active scan guards.
    void upsertBatch(const std::vector<FileRecord>& records,
                     const std::string& root,
                     std::int64_t generation) const;

    std::uint64_t deleteStaleForRoot(const std::string& root, std::int64_t generation) const;
    // Cache-only deletion; callers must establish deletion authority themselves.
    // Exact BINARY subtree removal advances the owning source revision, so
    // older snapshots cannot restore deleted cached records.
    std::uint64_t deletePathAndDescendants(const std::string& path) const;
    std::uint64_t deleteRoot(const std::string& root) const;
    void clear() const;

    // offset/limit are intentionally exposed for viewport-style incremental loading.
    std::vector<SearchResult> search(const SearchQuery& query,
                                     std::size_t limit = 500,
                                     std::size_t offset = 0,
                                     const std::atomic_bool* cancel = nullptr) const;
    std::vector<SearchResult> search(const std::string& query,
                                     std::size_t limit = 500,
                                     std::size_t offset = 0,
                                     const std::atomic_bool* cancel = nullptr) const;

    // Correctness-first search path. This deliberately bypasses FTS5/trigram
    // and uses the canonical files table with LIKE predicates. It is slower
    // on very large indexes, but it is the reference implementation used as
    // a fallback whenever an accelerated search could miss a valid result.
    std::vector<SearchResult> searchReliable(const SearchQuery& query,
                                             std::size_t limit = 500,
                                             std::size_t offset = 0) const;
    std::vector<SearchResult> searchReliable(const std::string& query,
                                             std::size_t limit = 500,
                                             std::size_t offset = 0) const;

    std::uint64_t totalFileCount() const;
    std::vector<std::string> roots() const;

    // v0.3: optional SQLite FTS5 trigram acceleration for basename substring
    // searches. Existing v0.2 databases keep working and fall back to LIKE
    // until a full rebuild has prepared the trigram index once.
    bool nameSearchIndexAvailable() const;
    bool nameSearchIndexReady() const;
    std::uint64_t nameSearchIndexCount() const;
    void verifyNameSearchIndex() const;
    void rebuildNameSearchIndex() const;

private:
    std::string db_path_;
};

} // namespace everything_lite
