#pragma once

#include "core/file_record.h"
#include "core/exclusion_policy.h"
#include "core/search_query.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

namespace everything_lite {

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
    // Full snapshots are merged only when the captured revision, identity,
    // binding and mount fingerprint still agree. Failure never deletes files.
    IndexStats mergeSourceSnapshot(const SourceRecord& expected,
                                  const std::vector<FileRecord>& records,
                                  const std::string& subtree_relative = {}) const;
    // Read-only staging SQLite: snapshot_metadata(complete=1), snapshot_files
    // columns follow FileRecord (path,parent_path,name,ext,root,size,
    // modified_time,is_dir,scan_generation,source_id,relative_path,cloud_copy).
    // Staging writers must close/stop writing before handing the file over.
    IndexStats mergeSourceSnapshotFile(const SourceRecord& expected,
                                      const std::string& snapshot_database,
                                      const std::string& subtree_relative = {}) const;
    std::string migrationBackupPath() const;
    ExclusionPolicy exclusionPolicy() const;
    // Validates first; atomically replaces rules, increments revision and purges
    // excluded cached rows for all sources, including offline sources.
    std::uint64_t saveExclusionPolicy(const ExclusionRules& rules) const;

    // Compatibility scans reserve a unique (root,generation) and capture
    // source/policy guards. Stale or completed generations cannot mutate rows.
    void beginRootScan(const std::string& root, std::int64_t generation) const;
    void completeRootScan(const std::string& root, std::int64_t generation, std::uint64_t file_count) const;
    std::int64_t currentGeneration(const std::string& root) const;

    // Without beginRootScan, one batch uses the current policy but has no
    // authority to delete stale rows or complete a full scan.
    void upsertBatch(const std::vector<FileRecord>& records,
                     const std::string& root,
                     std::int64_t generation) const;

    std::uint64_t deleteStaleForRoot(const std::string& root, std::int64_t generation) const;
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
