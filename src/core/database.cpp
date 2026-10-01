#include "core/database.h"
#include "core/path_utils.h"

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace everything_lite {
namespace {

class SqliteConnection {
public:
    explicit SqliteConnection(const std::string& path) {
        const auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }
        if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
            std::string message = db_ ? sqlite3_errmsg(db_) : "unable to open sqlite database";
            if (db_) sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error(message);
        }
        exec("PRAGMA journal_mode=WAL;");
        exec("PRAGMA synchronous=NORMAL;");
        exec("PRAGMA temp_store=MEMORY;");
        exec("PRAGMA busy_timeout=5000;");
        exec("PRAGMA case_sensitive_like=ON;");
    }

    ~SqliteConnection() {
        if (db_) sqlite3_close(db_);
    }

    sqlite3* get() const { return db_; }

    void exec(const char* sql) const {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            std::string message = error ? error : "sqlite error";
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

private:
    sqlite3* db_ = nullptr;
};

struct StatementDeleter {
    void operator()(sqlite3_stmt* stmt) const {
        if (stmt) sqlite3_finalize(stmt);
    }
};
using Statement = std::unique_ptr<sqlite3_stmt, StatementDeleter>;

Statement prepare(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
    return Statement(raw);
}

void bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    if (sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        throw std::runtime_error("sqlite bind text failed");
    }
}

std::string columnText(sqlite3_stmt* stmt, int column) {
    const auto* text = sqlite3_column_text(stmt, column);
    return text ? reinterpret_cast<const char*>(text) : std::string{};
}

FileRecord readFileRecord(sqlite3_stmt* stmt) {
    FileRecord r;
    r.path = columnText(stmt, 0);
    r.parent_path = columnText(stmt, 1);
    r.name = columnText(stmt, 2);
    r.extension = columnText(stmt, 3);
    r.root = columnText(stmt, 4);
    r.size = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 5));
    r.modified_time = sqlite3_column_int64(stmt, 6);
    r.is_directory = sqlite3_column_int(stmt, 7) != 0;
    r.scan_generation = sqlite3_column_int64(stmt, 8);
    return r;
}

std::vector<SearchResult> runSearch(sqlite3* db,
                                    const SearchQuery& query,
                                    std::size_t limit,
                                    bool prefix_only,
                                    const std::unordered_set<std::string>& exclude_paths = {}) {
    std::ostringstream sql;
    sql << "SELECT path,parent_path,name,ext,root,size,modified_time,is_dir,scan_generation FROM files";

    std::vector<std::string> where;
    for (std::size_t i = 0; i < query.terms.size(); ++i) {
        where.emplace_back("(search_name LIKE ? ESCAPE '\\' OR search_path LIKE ? ESCAPE '\\')");
    }
    if (query.extension) where.emplace_back("ext = ? COLLATE NOCASE");
    if (query.path_term) where.emplace_back("search_path LIKE ? ESCAPE '\\'");
    if (query.min_size) where.emplace_back("size >= ?");
    if (query.max_size) where.emplace_back("size <= ?");
    if (query.modified_after) where.emplace_back("modified_time >= ?");
    if (query.files_only) where.emplace_back("is_dir = 0");
    if (query.directories_only) where.emplace_back("is_dir = 1");

    if (!where.empty()) {
        sql << " WHERE ";
        for (std::size_t i = 0; i < where.size(); ++i) {
            if (i) sql << " AND ";
            sql << where[i];
        }
    }
    sql << " ORDER BY is_dir DESC, name COLLATE NOCASE ASC LIMIT ?";

    auto stmt = prepare(db, sql.str());
    int bind_index = 1;
    for (const auto& token : query.terms) {
        const auto escaped = escapeLike(token);
        const auto pattern = (prefix_only && query.terms.size() == 1) ? (escaped + "%") : ("%" + escaped + "%");
        bindText(stmt.get(), bind_index++, pattern);
        bindText(stmt.get(), bind_index++, pattern);
    }
    if (query.extension) bindText(stmt.get(), bind_index++, *query.extension);
    if (query.path_term) bindText(stmt.get(), bind_index++, "%" + escapeLike(*query.path_term) + "%");
    if (query.min_size) sqlite3_bind_int64(stmt.get(), bind_index++, static_cast<sqlite3_int64>(*query.min_size));
    if (query.max_size) sqlite3_bind_int64(stmt.get(), bind_index++, static_cast<sqlite3_int64>(*query.max_size));
    if (query.modified_after) sqlite3_bind_int64(stmt.get(), bind_index++, *query.modified_after);
    sqlite3_bind_int64(stmt.get(), bind_index, static_cast<sqlite3_int64>(limit + exclude_paths.size()));

    std::vector<SearchResult> results;
    results.reserve(limit);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        auto record = readFileRecord(stmt.get());
        if (!exclude_paths.empty() && exclude_paths.find(record.path) != exclude_paths.end()) continue;
        SearchResult result;
        result.file = std::move(record);
        result.rank = prefix_only ? 1 : 2;
        results.push_back(std::move(result));
        if (results.size() >= limit) break;
    }
    return results;
}

} // namespace

Database::Database(std::string db_path) : db_path_(std::move(db_path)) {}

void Database::initialize() const {
    SqliteConnection conn(db_path_);
    conn.exec(R"SQL(
CREATE TABLE IF NOT EXISTS files (
    id INTEGER PRIMARY KEY,
    path TEXT NOT NULL UNIQUE,
    parent_path TEXT NOT NULL,
    name TEXT NOT NULL,
    ext TEXT NOT NULL DEFAULT '',
    root TEXT NOT NULL,
    size INTEGER NOT NULL DEFAULT 0,
    modified_time INTEGER NOT NULL DEFAULT 0,
    is_dir INTEGER NOT NULL DEFAULT 0,
    search_name TEXT NOT NULL,
    search_path TEXT NOT NULL,
    scan_generation INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_files_search_name ON files(search_name);
CREATE INDEX IF NOT EXISTS idx_files_ext ON files(ext COLLATE NOCASE);
CREATE INDEX IF NOT EXISTS idx_files_size ON files(size);
CREATE INDEX IF NOT EXISTS idx_files_root_generation ON files(root, scan_generation);
CREATE INDEX IF NOT EXISTS idx_files_modified ON files(modified_time DESC);
CREATE TABLE IF NOT EXISTS roots (
    root TEXT PRIMARY KEY,
    generation INTEGER NOT NULL DEFAULT 0,
    last_scan_time INTEGER NOT NULL DEFAULT 0,
    file_count INTEGER NOT NULL DEFAULT 0
);
)SQL");
}

void Database::beginRootScan(const std::string& root, std::int64_t generation) const {
    SqliteConnection conn(db_path_);
    auto stmt = prepare(conn.get(), R"SQL(
INSERT INTO roots(root,generation,last_scan_time,file_count)
VALUES(?,?,strftime('%s','now'),0)
ON CONFLICT(root) DO UPDATE SET generation=excluded.generation,last_scan_time=excluded.last_scan_time;
)SQL");
    bindText(stmt.get(), 1, normalizePath(root));
    sqlite3_bind_int64(stmt.get(), 2, generation);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
}

void Database::completeRootScan(const std::string& root, std::int64_t generation, std::uint64_t file_count) const {
    SqliteConnection conn(db_path_);
    auto stmt = prepare(conn.get(), "UPDATE roots SET generation=?,last_scan_time=strftime('%s','now'),file_count=? WHERE root=?;");
    sqlite3_bind_int64(stmt.get(), 1, generation);
    sqlite3_bind_int64(stmt.get(), 2, static_cast<sqlite3_int64>(file_count));
    bindText(stmt.get(), 3, normalizePath(root));
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
}

std::int64_t Database::currentGeneration(const std::string& root) const {
    SqliteConnection conn(db_path_);
    auto stmt = prepare(conn.get(), "SELECT generation FROM roots WHERE root=?;");
    bindText(stmt.get(), 1, normalizePath(root));
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) return sqlite3_column_int64(stmt.get(), 0);
    return 0;
}

void Database::upsertBatch(const std::vector<FileRecord>& records,
                           const std::string& root,
                           std::int64_t generation) const {
    if (records.empty()) return;
    SqliteConnection conn(db_path_);
    conn.exec("BEGIN IMMEDIATE TRANSACTION;");
    try {
        auto stmt = prepare(conn.get(), R"SQL(
INSERT INTO files(path,parent_path,name,ext,root,size,modified_time,is_dir,search_name,search_path,scan_generation)
VALUES(?,?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(path) DO UPDATE SET
    parent_path=excluded.parent_path,
    name=excluded.name,
    ext=excluded.ext,
    root=excluded.root,
    size=excluded.size,
    modified_time=excluded.modified_time,
    is_dir=excluded.is_dir,
    search_name=excluded.search_name,
    search_path=excluded.search_path,
    scan_generation=excluded.scan_generation;
)SQL");

        const auto normalized_root = normalizePath(root);
        for (const auto& r : records) {
            sqlite3_reset(stmt.get());
            sqlite3_clear_bindings(stmt.get());
            bindText(stmt.get(), 1, r.path);
            bindText(stmt.get(), 2, r.parent_path);
            bindText(stmt.get(), 3, r.name);
            bindText(stmt.get(), 4, r.extension);
            bindText(stmt.get(), 5, normalized_root);
            sqlite3_bind_int64(stmt.get(), 6, static_cast<sqlite3_int64>(r.size));
            sqlite3_bind_int64(stmt.get(), 7, r.modified_time);
            sqlite3_bind_int(stmt.get(), 8, r.is_directory ? 1 : 0);
            bindText(stmt.get(), 9, asciiFold(r.name));
            bindText(stmt.get(), 10, asciiFold(r.path));
            sqlite3_bind_int64(stmt.get(), 11, generation);
            if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
                throw std::runtime_error(sqlite3_errmsg(conn.get()));
            }
        }
        conn.exec("COMMIT;");
    } catch (...) {
        conn.exec("ROLLBACK;");
        throw;
    }
}

std::uint64_t Database::deleteStaleForRoot(const std::string& root, std::int64_t generation) const {
    SqliteConnection conn(db_path_);
    auto stmt = prepare(conn.get(), "DELETE FROM files WHERE root=? AND scan_generation<>?;");
    bindText(stmt.get(), 1, normalizePath(root));
    sqlite3_bind_int64(stmt.get(), 2, generation);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
    return static_cast<std::uint64_t>(sqlite3_changes(conn.get()));
}

std::uint64_t Database::deletePathAndDescendants(const std::string& path) const {
    SqliteConnection conn(db_path_);
    const auto normalized = normalizePath(path);
    const auto subtree = escapeLike(normalized) + "/%";
    auto stmt = prepare(conn.get(), "DELETE FROM files WHERE path=? OR path LIKE ? ESCAPE '\\';");
    bindText(stmt.get(), 1, normalized);
    bindText(stmt.get(), 2, subtree);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
    return static_cast<std::uint64_t>(sqlite3_changes(conn.get()));
}

std::uint64_t Database::deleteRoot(const std::string& root) const {
    SqliteConnection conn(db_path_);
    conn.exec("BEGIN IMMEDIATE TRANSACTION;");
    try {
        auto files_stmt = prepare(conn.get(), "DELETE FROM files WHERE root=?;");
        bindText(files_stmt.get(), 1, normalizePath(root));
        if (sqlite3_step(files_stmt.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
        const auto removed = static_cast<std::uint64_t>(sqlite3_changes(conn.get()));

        auto root_stmt = prepare(conn.get(), "DELETE FROM roots WHERE root=?;");
        bindText(root_stmt.get(), 1, normalizePath(root));
        if (sqlite3_step(root_stmt.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
        conn.exec("COMMIT;");
        return removed;
    } catch (...) {
        conn.exec("ROLLBACK;");
        throw;
    }
}

void Database::clear() const {
    SqliteConnection conn(db_path_);
    conn.exec("DELETE FROM files; DELETE FROM roots;");
}

std::vector<SearchResult> Database::search(const SearchQuery& query, std::size_t limit) const {
    SqliteConnection conn(db_path_);
    if (query.terms.size() == 1) {
        auto prefix = runSearch(conn.get(), query, limit, true);
        if (prefix.size() >= limit) return prefix;
        std::unordered_set<std::string> seen;
        seen.reserve(prefix.size() * 2 + 1);
        for (const auto& item : prefix) seen.insert(item.file.path);
        auto contains = runSearch(conn.get(), query, limit - prefix.size(), false, seen);
        prefix.insert(prefix.end(), std::make_move_iterator(contains.begin()), std::make_move_iterator(contains.end()));
        return prefix;
    }
    return runSearch(conn.get(), query, limit, false);
}

std::vector<SearchResult> Database::search(const std::string& query, std::size_t limit) const {
    return search(parseSearchQuery(query), limit);
}

std::uint64_t Database::totalFileCount() const {
    SqliteConnection conn(db_path_);
    auto stmt = prepare(conn.get(), "SELECT COUNT(*) FROM files;");
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        return static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 0));
    }
    return 0;
}

std::vector<std::string> Database::roots() const {
    SqliteConnection conn(db_path_);
    auto stmt = prepare(conn.get(), "SELECT root FROM roots ORDER BY root;");
    std::vector<std::string> out;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        out.push_back(columnText(stmt.get(), 0));
    }
    return out;
}

} // namespace everything_lite
