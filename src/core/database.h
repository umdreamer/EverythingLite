#pragma once

#include "core/file_record.h"

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

    std::vector<SearchResult> search(const std::string& query, std::size_t limit = 500) const;

    std::uint64_t totalFileCount() const;
    std::vector<std::string> roots() const;

private:
    std::string db_path_;
};

} // namespace everything_lite
