#pragma once
#include "core/file_record.h"
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
struct sqlite3;
namespace everything_lite {
// None of these helpers owns a transaction. Business admission stays in Database.
bool hasDirectoryStorage(sqlite3* db);
bool directoryStorageReady(sqlite3* db);
void registerDirectoryWriter(sqlite3* db);
void installDirectoryFtsTriggers(sqlite3* db);
void migrateDirectorySchema(sqlite3* db);
std::int64_t directoryEntryId(sqlite3* db,std::int64_t source,const std::string& relative);
class DirectoryRecordWriter {
public:
    explicit DirectoryRecordWriter(sqlite3* db);
    ~DirectoryRecordWriter();
    bool upsert(std::int64_t source,const FileRecord& record,const std::string& relative,std::int64_t generation);
    std::int64_t entryId(std::int64_t source,const std::string& relative);
    std::int64_t directoryId(std::int64_t source,const std::string& relative);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct DirectoryScopeQuery { std::string sql; std::vector<std::string> bounds; };
// Each query binds source at 1, then bounds. Equality/range probes avoid a source scan.
std::vector<DirectoryScopeQuery> directoryScopeQueries(const std::string& prefix,
    const std::string& suffix,const std::string& scope);
} // namespace everything_lite
