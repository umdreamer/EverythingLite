#pragma once

#include "core/file_record.h"
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

    void beginRootScan(const std::string& root, std::int64_t generation) const;
    void completeRootScan(const std::string& root, std::int64_t generation, std::uint64_t file_count) const;
    std::int64_t currentGeneration(const std::string& root) const;

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
