#include "core/database.h"
#include "core/path_utils.h"

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>

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

    bool tryExec(const char* sql) const noexcept {
        char* error = nullptr;
        const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &error);
        if (error) sqlite3_free(error);
        return rc == SQLITE_OK;
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

bool tableExists(sqlite3* db, const char* name) {
    auto stmt = prepare(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1;");
    bindText(stmt.get(), 1, name);
    return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

bool metadataFlag(sqlite3* db, const char* key) {
    auto stmt = prepare(db, "SELECT value FROM app_metadata WHERE key=? LIMIT 1;");
    bindText(stmt.get(), 1, key);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) return false;
    return columnText(stmt.get(), 0) == "1";
}

void setMetadataFlag(sqlite3* db, const char* key, bool enabled) {
    auto stmt = prepare(db, R"SQL(
INSERT INTO app_metadata(key,value) VALUES(?,?)
ON CONFLICT(key) DO UPDATE SET value=excluded.value;
)SQL");
    bindText(stmt.get(), 1, key);
    bindText(stmt.get(), 2, enabled ? "1" : "0");
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
}

void installFtsTriggers(const SqliteConnection& conn) {
    conn.exec(R"SQL(
CREATE TRIGGER IF NOT EXISTS files_name_fts_ai AFTER INSERT ON files BEGIN
    INSERT INTO files_name_fts(rowid,search_name) VALUES(new.id,new.search_name);
END;
CREATE TRIGGER IF NOT EXISTS files_name_fts_ad AFTER DELETE ON files BEGIN
    INSERT INTO files_name_fts(files_name_fts,rowid,search_name)
    VALUES('delete',old.id,old.search_name);
END;
CREATE TRIGGER IF NOT EXISTS files_name_fts_au AFTER UPDATE OF search_name ON files WHEN old.search_name <> new.search_name BEGIN
    INSERT INTO files_name_fts(files_name_fts,rowid,search_name)
    VALUES('delete',old.id,old.search_name);
    INSERT INTO files_name_fts(rowid,search_name) VALUES(new.id,new.search_name);
END;
)SQL");
}

std::size_t utf8CodepointCount(const std::string& text) {
    std::size_t count = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0U) != 0x80U) ++count;
    }
    return count;
}

std::string quoteFtsPhrase(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 4);
    out.push_back('"');
    for (char c : text) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

bool canUseNameFts(const SearchQuery& query, bool fts_ready) {
    if (!fts_ready || query.match_path || query.terms.empty()) return false;
    for (const auto& term : query.terms) {
        if (utf8CodepointCount(term) < 3) return false;
    }
    return true;
}

std::string makeFtsExpression(const SearchQuery& query) {
    std::ostringstream out;
    for (std::size_t i = 0; i < query.terms.size(); ++i) {
        if (i) out << " AND ";
        out << quoteFtsPhrase(query.terms[i]);
    }
    return out.str();
}

std::vector<SearchResult> runSearch(sqlite3* db,
                                    const SearchQuery& query,
                                    std::size_t limit,
                                    std::size_t offset,
                                    bool fts_ready) {
    const bool use_fts = canUseNameFts(query, fts_ready);

    std::ostringstream sql;
    if (use_fts) {
        sql << "SELECT files.path,files.parent_path,files.name,files.ext,files.root,files.size,files.modified_time,files.is_dir,files.scan_generation "
               "FROM files_name_fts JOIN files ON files.id=files_name_fts.rowid";
    } else {
        sql << "SELECT files.path,files.parent_path,files.name,files.ext,files.root,files.size,files.modified_time,files.is_dir,files.scan_generation FROM files";
    }

    std::vector<std::string> where;
    if (use_fts) {
        where.emplace_back("files_name_fts MATCH ?");
    } else {
        for (std::size_t i = 0; i < query.terms.size(); ++i) {
            // Everything-compatible default: plain terms match the basename only.
            // Full-path matching is opt-in via Match Path.
            where.emplace_back(query.match_path
                ? "files.search_path LIKE ? ESCAPE '\\'"
                : "files.search_name LIKE ? ESCAPE '\\'");
        }
    }
    if (query.extension) where.emplace_back("files.ext = ? COLLATE NOCASE");
    if (query.path_term) where.emplace_back("files.search_path LIKE ? ESCAPE '\\'");
    if (query.min_size) where.emplace_back("files.size >= ?");
    if (query.max_size) where.emplace_back("files.size <= ?");
    if (query.modified_after) where.emplace_back("files.modified_time >= ?");
    if (query.files_only) where.emplace_back("files.is_dir = 0");
    if (query.directories_only) where.emplace_back("files.is_dir = 1");

    if (!where.empty()) {
        sql << " WHERE ";
        for (std::size_t i = 0; i < where.size(); ++i) {
            if (i) sql << " AND ";
            sql << where[i];
        }
    }

    // Match Everything's default presentation more closely: Name, then Path.
    sql << " ORDER BY files.name COLLATE NOCASE ASC, files.parent_path COLLATE NOCASE ASC LIMIT ? OFFSET ?";

    auto stmt = prepare(db, sql.str());
    int bind_index = 1;
    if (use_fts) {
        bindText(stmt.get(), bind_index++, makeFtsExpression(query));
    } else {
        for (const auto& token : query.terms) {
            const auto escaped = escapeLike(token);
            bindText(stmt.get(), bind_index++, "%" + escaped + "%");
        }
    }
    if (query.extension) bindText(stmt.get(), bind_index++, *query.extension);
    if (query.path_term) bindText(stmt.get(), bind_index++, "%" + escapeLike(*query.path_term) + "%");
    if (query.min_size) sqlite3_bind_int64(stmt.get(), bind_index++, static_cast<sqlite3_int64>(*query.min_size));
    if (query.max_size) sqlite3_bind_int64(stmt.get(), bind_index++, static_cast<sqlite3_int64>(*query.max_size));
    if (query.modified_after) sqlite3_bind_int64(stmt.get(), bind_index++, *query.modified_after);
    sqlite3_bind_int64(stmt.get(), bind_index++, static_cast<sqlite3_int64>(limit));
    sqlite3_bind_int64(stmt.get(), bind_index, static_cast<sqlite3_int64>(offset));

    std::vector<SearchResult> results;
    results.reserve(limit);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        SearchResult result;
        result.file = readFileRecord(stmt.get());
        result.rank = 0;
        results.push_back(std::move(result));
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
CREATE TABLE IF NOT EXISTS app_metadata (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
INSERT OR IGNORE INTO app_metadata(key,value) VALUES('name_fts_ready','0');
)SQL");

    // FTS5 may not be compiled into every platform SQLite. The app remains
    // functional without it and simply falls back to LIKE scans.
    const bool fts_ok = conn.tryExec(R"SQL(
CREATE VIRTUAL TABLE IF NOT EXISTS files_name_fts USING fts5(
    search_name,
    content='files',
    content_rowid='id',
    tokenize='trigram'
);
)SQL");
    if (fts_ok && metadataFlag(conn.get(), "name_fts_ready")) {
        installFtsTriggers(conn);
    }
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

std::vector<SearchResult> Database::search(const SearchQuery& query,
                                           std::size_t limit,
                                           std::size_t offset) const {
    SqliteConnection conn(db_path_);
    const bool fts_ready = tableExists(conn.get(), "files_name_fts") && metadataFlag(conn.get(), "name_fts_ready");
    return runSearch(conn.get(), query, limit, offset, fts_ready);
}

std::vector<SearchResult> Database::search(const std::string& query,
                                           std::size_t limit,
                                           std::size_t offset) const {
    return search(parseSearchQuery(query), limit, offset);
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

bool Database::nameSearchIndexAvailable() const {
    SqliteConnection conn(db_path_);
    return tableExists(conn.get(), "files_name_fts");
}

bool Database::nameSearchIndexReady() const {
    SqliteConnection conn(db_path_);
    return tableExists(conn.get(), "files_name_fts") && metadataFlag(conn.get(), "name_fts_ready");
}

void Database::rebuildNameSearchIndex() const {
    SqliteConnection conn(db_path_);
    if (!tableExists(conn.get(), "files_name_fts")) return;

    // External-content FTS tables require one initial rebuild for databases
    // created by v0.2 and earlier. This is a one-time O(N) migration.
    conn.exec("INSERT INTO files_name_fts(files_name_fts) VALUES('rebuild');");
    installFtsTriggers(conn);
    setMetadataFlag(conn.get(), "name_fts_ready", true);
}

} // namespace everything_lite
