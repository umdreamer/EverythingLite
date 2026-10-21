#include "core/database.h"
#include "core/directory_storage.h"
#include "core/path_utils.h"
#include "core/scan_request.h"
#include "core/search_trace.h"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <chrono>
#include <memory>
#include <functional>
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
        registerDirectoryWriter(db_);
        // Configure lock waiting before any PRAGMA that may acquire a lock.
        sqlite3_busy_timeout(db_, 5000);
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
    r.source_id = sqlite3_column_int64(stmt, 9);
    r.relative_path = columnText(stmt, 10);
    r.cloud_copy = static_cast<CloudCopy>(sqlite3_column_int(stmt, 11));
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
    if(hasDirectoryStorage(conn.get())) { installDirectoryFtsTriggers(conn.get());return; }
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

ExclusionPolicy getPolicy(sqlite3* db) {
    auto state=prepare(db,"SELECT revision,folder_filter,file_filter FROM exclusion_policy WHERE id=1");
    if(sqlite3_step(state.get())!=SQLITE_ROW) throw std::runtime_error("exclusion policy missing");
    const auto revision=sqlite3_column_int64(state.get(),0);
    ExclusionRules rules; rules.folder_filter=columnText(state.get(),1); rules.file_filter=columnText(state.get(),2);
    auto paths=prepare(db,"SELECT kind,path FROM exclusion_paths ORDER BY kind,position");
    int rc;
    while((rc=sqlite3_step(paths.get()))==SQLITE_ROW) {
        (sqlite3_column_int(paths.get(),0)==0 ? rules.directories : rules.files).push_back(columnText(paths.get(),1));
    }
    if(rc!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
    return ExclusionPolicy(std::move(rules),revision);
}
std::uint64_t purgeExcluded(const SqliteConnection& conn,const ExclusionPolicy& policy) {
    // Build membership without modifying the SELECT input; do not probe paths.
    conn.exec("CREATE TEMP TABLE IF NOT EXISTS excluded_keys(id INTEGER PRIMARY KEY); DELETE FROM excluded_keys;");
    auto rows=prepare(conn.get(),"SELECT id,path,is_dir FROM files");
    auto excluded=prepare(conn.get(),"INSERT INTO excluded_keys VALUES(?)");
    int rc;
    while((rc=sqlite3_step(rows.get()))==SQLITE_ROW) {
        if(!policy.excludes(columnText(rows.get(),1),sqlite3_column_int(rows.get(),2)!=0)) continue;
        sqlite3_reset(excluded.get()); sqlite3_bind_int64(excluded.get(),1,sqlite3_column_int64(rows.get(),0));
        if(sqlite3_step(excluded.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
    }
    if(rc!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
    conn.exec(hasDirectoryStorage(conn.get()) ? "DELETE FROM file_entries WHERE id IN (SELECT id FROM excluded_keys);" : "DELETE FROM files WHERE id IN (SELECT id FROM excluded_keys);");
    const auto removed=static_cast<std::uint64_t>(sqlite3_changes(conn.get()));
    conn.exec("UPDATE roots SET file_count=(SELECT COUNT(*) FROM files WHERE files.root=roots.root);");
    return removed;
}

constexpr const char* sourceColumns = "id,label,type,mode,status,interval_seconds,identity,root,mount_fingerprint,last_check,last_success,revision";
SourceRecord readSource(sqlite3_stmt* stmt) {
    SourceRecord s;
    s.id=sqlite3_column_int64(stmt,0); s.label=columnText(stmt,1);
    s.type=static_cast<SourceType>(sqlite3_column_int(stmt,2));
    s.mode=static_cast<IndexMode>(sqlite3_column_int(stmt,3));
    s.status=static_cast<SourceStatus>(sqlite3_column_int(stmt,4));
    s.interval_seconds=sqlite3_column_int64(stmt,5); s.identity=columnText(stmt,6);
    s.root=columnText(stmt,7); s.mount_fingerprint=columnText(stmt,8);
    s.last_check=sqlite3_column_int64(stmt,9); s.last_success=sqlite3_column_int64(stmt,10);
    s.revision=sqlite3_column_int64(stmt,11); return s;
}
SourceRecord getSource(sqlite3* db, std::int64_t id) {
    auto stmt=prepare(db,std::string("SELECT ")+sourceColumns+" FROM sources WHERE id=?");
    sqlite3_bind_int64(stmt.get(),1,id);
    if(sqlite3_step(stmt.get())!=SQLITE_ROW) throw std::runtime_error("source not found");
    auto source=readSource(stmt.get());
    source.policy_revision=getPolicy(db).revision;
    return source;
}
bool normalizedAbsolute(const std::string& path) {
    return !path.empty() && path.front()=='/' && path.find('\0')==std::string::npos && normalizePath(path)==path;
}
std::vector<std::string> nestedSourceRoots(sqlite3* db,const SourceRecord& source) {
    auto stmt=prepare(db,"SELECT root FROM sources WHERE id<>?");sqlite3_bind_int64(stmt.get(),1,source.id);
    std::vector<std::string> roots;int rc;
    while((rc=sqlite3_step(stmt.get()))==SQLITE_ROW) {
        const auto root=normalizePath(columnText(stmt.get(),0));
        if(root!=source.root && pathIsWithin(root,source.root))roots.push_back(root);
    }
    if(rc!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db));
    std::sort(roots.begin(),roots.end());roots.erase(std::unique(roots.begin(),roots.end()),roots.end());return roots;
}
std::vector<SourceRecord> sourceDescendants(sqlite3* db,const SourceRecord& source) {
    auto stmt=prepare(db,std::string("SELECT ")+sourceColumns+" FROM sources WHERE id<>? ORDER BY id");
    sqlite3_bind_int64(stmt.get(),1,source.id);
    std::vector<SourceRecord> descendants; int rc;
    while((rc=sqlite3_step(stmt.get()))==SQLITE_ROW) {
        auto child=readSource(stmt.get()); child.root=normalizePath(child.root);
        if(child.root!=source.root && pathIsWithin(child.root,source.root)) descendants.push_back(std::move(child));
    }
    if(rc!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db));
    return descendants;
}
bool ownsSuccessfulSnapshot(const SourceRecord& source) {
    return source.last_success>0 && source.status==SourceStatus::Accessible;
}
bool sameDescendantOwnership(const std::vector<SourceRecord>& captured,const std::vector<SourceRecord>& current) {
    if(captured.size()!=current.size())return false;
    for(std::size_t i=0;i<captured.size();++i)
        if(captured[i].id!=current[i].id || captured[i].root!=current[i].root ||
           ownsSuccessfulSnapshot(captured[i])!=ownsSuccessfulSnapshot(current[i]))return false;
    return true;
}
std::string snapshotText(sqlite3_stmt* stmt,int column) {
    if(sqlite3_column_type(stmt,column)!=SQLITE_TEXT)throw std::runtime_error("snapshot text field type mismatch");
    const auto* bytes=sqlite3_column_text(stmt,column);const auto size=sqlite3_column_bytes(stmt,column);
    if(!bytes)throw std::runtime_error("unable to read snapshot text");
    std::string text(reinterpret_cast<const char*>(bytes),static_cast<std::size_t>(size));
    if(text.find('\0')!=std::string::npos)throw std::runtime_error("snapshot text contains NUL");return text;
}
std::int64_t snapshotInteger(sqlite3_stmt* stmt,int column) {
    if(sqlite3_column_type(stmt,column)!=SQLITE_INTEGER)throw std::runtime_error("snapshot integer field type mismatch");
    return sqlite3_column_int64(stmt,column);
}
std::vector<std::string> snapshotRoots(sqlite3* db,const char* query,const std::string& owning_root) {
    auto stmt=prepare(db,query);std::vector<std::string> roots;int rc;
    while((rc=sqlite3_step(stmt.get()))==SQLITE_ROW) {
        auto root=snapshotText(stmt.get(),0);
        if(!normalizedAbsolute(root) || root==owning_root || !pathIsWithin(root,owning_root))throw std::runtime_error("snapshot boundary outside owning root");
        if(!roots.empty() && roots.back()>=root)throw std::runtime_error("duplicate or unordered snapshot boundary");roots.push_back(std::move(root));
    }
    if(rc!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db));return roots;
}
std::int64_t sourceForRoot(sqlite3* db, const std::string& root, bool create) {
    auto stmt=prepare(db,"SELECT id FROM sources WHERE root=? ORDER BY id");
    bindText(stmt.get(),1,normalizePath(root));
    const int rc=sqlite3_step(stmt.get());
    if(rc==SQLITE_ROW) {
        const auto id=sqlite3_column_int64(stmt.get(),0);
        if(sqlite3_step(stmt.get())==SQLITE_ROW) throw std::runtime_error("ambiguous source binding; use source id");
        return id;
    }
    if(rc!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
    if(!create) return 0;
    auto insert=prepare(db,"INSERT INTO sources(label,root) VALUES(?,?)");
    bindText(insert.get(),1,pathToUtf8(utf8ToPath(root).filename()));
    bindText(insert.get(),2,normalizePath(root));
    if(sqlite3_step(insert.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
    return sqlite3_last_insert_rowid(db);
}
std::int64_t guardedScanSource(sqlite3* db,const std::string& root,std::int64_t generation,bool required) {
    auto guard=prepare(db,"SELECT source_id,source_revision,policy_revision,identity,mount_fingerprint,state FROM compatibility_scans WHERE root=? AND generation=?");
    bindText(guard.get(),1,normalizePath(root)); sqlite3_bind_int64(guard.get(),2,generation);
    const auto rc=sqlite3_step(guard.get());
    if(rc==SQLITE_DONE) {
        if(required) throw std::runtime_error("compatibility scan requires beginRootScan");
        return 0; // standalone batch: current policy, no deletion authority
    }
    if(rc!=SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db));
    if(sqlite3_column_int(guard.get(),5)!=0) throw std::runtime_error("compatibility scan is no longer active");
    const auto current=getSource(db,sqlite3_column_int64(guard.get(),0));
    if(current.revision!=sqlite3_column_int64(guard.get(),1) || current.policy_revision!=sqlite3_column_int64(guard.get(),2) ||
       current.identity!=columnText(guard.get(),3) || current.mount_fingerprint!=columnText(guard.get(),4) || current.root!=normalizePath(root))
        throw std::runtime_error("source or exclusion policy changed during compatibility scan");
    if(sourceForRoot(db,root,false)!=current.id) throw std::runtime_error("compatibility scan binding changed");
    return current.id;
}
void advanceSourceRevision(sqlite3* db,std::int64_t id) {
    auto update=prepare(db,"UPDATE sources SET revision=revision+1 WHERE id=?");
    sqlite3_bind_int64(update.get(),1,id);
    if(sqlite3_step(update.get())!=SQLITE_DONE || sqlite3_changes(db)!=1)
        throw std::runtime_error("unable to advance source revision");
}
void advanceScanRevision(sqlite3* db,std::int64_t id,const std::string& root,std::int64_t generation) {
    advanceSourceRevision(db,id);
    // Only the already validated generation owns this mutation. Refreshing
    // every active guard would let an unrelated writer revive a stale scan.
    auto update=prepare(db,"UPDATE compatibility_scans SET source_revision=(SELECT revision FROM sources WHERE id=?) WHERE root=? AND generation=? AND source_id=? AND state=0");
    sqlite3_bind_int64(update.get(),1,id); bindText(update.get(),2,normalizePath(root));
    sqlite3_bind_int64(update.get(),3,generation); sqlite3_bind_int64(update.get(),4,id);
    if(sqlite3_step(update.get())!=SQLITE_DONE || sqlite3_changes(db)!=1)
        throw std::runtime_error("unable to advance compatibility scan revision");
}
std::string relativeTo(const std::string& path, const std::string& root) {
    const auto normalized=normalizePath(path);
    if(!pathIsWithin(normalized,root)) throw std::invalid_argument("record outside source binding");
    if(normalized==root) return ".";
    const auto prefix=root.back()=='/' ? root.size() : root.size()+1;
    return normalized.substr(prefix);
}
std::string accessPath(const std::string& root,const std::string& relative) {
    if(relative==".") return root;
    return normalizePath(pathToUtf8(utf8ToPath(root)/utf8ToPath(relative)));
}
bool upsertRecords(sqlite3* db, const SourceRecord& source,
                   const std::vector<FileRecord>& records, std::int64_t generation,
                   const ExclusionPolicy& policy) {
    if(hasDirectoryStorage(db)) {
        bool changed=false;DirectoryRecordWriter writer(db);
        for(const auto& r:records) {
            if(r.source_id!=0 && r.source_id!=source.id)throw std::invalid_argument("wrong source record");
            const auto relative=relativeTo(r.path,source.root);
            if(!r.relative_path.empty() && r.relative_path!=relative)throw std::invalid_argument("relative path mismatch");
            if(policy.excludes(r.path,r.is_directory))continue;
            changed=writer.upsert(source.id,r,relative,generation)||changed;
        }
        return changed;
    }
    auto stmt=prepare(db,R"SQL(
INSERT INTO files(source_id,relative_path,path,parent_path,name,ext,root,size,modified_time,is_dir,search_name,search_path,scan_generation,cloud_copy)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(source_id,relative_path) DO UPDATE SET
path=excluded.path,parent_path=excluded.parent_path,name=excluded.name,ext=excluded.ext,root=excluded.root,
size=excluded.size,modified_time=excluded.modified_time,is_dir=excluded.is_dir,search_name=excluded.search_name,
search_path=excluded.search_path,scan_generation=excluded.scan_generation,cloud_copy=excluded.cloud_copy
WHERE files.path<>excluded.path OR files.parent_path<>excluded.parent_path OR files.name<>excluded.name
OR files.ext<>excluded.ext OR files.root<>excluded.root OR files.size<>excluded.size
OR files.modified_time<>excluded.modified_time OR files.is_dir<>excluded.is_dir
OR files.cloud_copy<>excluded.cloud_copy;
)SQL");
    bool changed=false;
    for(const auto& r:records) {
        if(r.source_id!=0 && r.source_id!=source.id) throw std::invalid_argument("wrong source record");
        const auto relative=relativeTo(r.path,source.root);
        if(!r.relative_path.empty() && r.relative_path!=relative) throw std::invalid_argument("relative path mismatch");
        if(policy.excludes(r.path,r.is_directory)) continue;
        sqlite3_reset(stmt.get()); sqlite3_clear_bindings(stmt.get());
        sqlite3_bind_int64(stmt.get(),1,source.id); bindText(stmt.get(),2,relative);
        bindText(stmt.get(),3,normalizePath(r.path)); bindText(stmt.get(),4,r.parent_path);
        bindText(stmt.get(),5,r.name); bindText(stmt.get(),6,r.extension); bindText(stmt.get(),7,source.root);
        sqlite3_bind_int64(stmt.get(),8,static_cast<sqlite3_int64>(r.size));
        sqlite3_bind_int64(stmt.get(),9,r.modified_time); sqlite3_bind_int(stmt.get(),10,r.is_directory?1:0);
        bindText(stmt.get(),11,asciiFold(r.name)); bindText(stmt.get(),12,asciiFold(r.path));
        sqlite3_bind_int64(stmt.get(),13,generation); sqlite3_bind_int(stmt.get(),14,static_cast<int>(r.cloud_copy));
        if(sqlite3_step(stmt.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
        changed=sqlite3_changes(db)>0 || changed;
    }
    return changed;
}
std::int64_t countRows(sqlite3* db,const char* table) {
    auto stmt=prepare(db,std::string("SELECT COUNT(*) FROM ")+table);
    if(sqlite3_step(stmt.get())!=SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db));
    return sqlite3_column_int64(stmt.get(),0);
}
bool columnExists(sqlite3* db,const char* table,const char* column) {
    auto stmt=prepare(db,std::string("PRAGMA table_info(")+table+")");
    while(sqlite3_step(stmt.get())==SQLITE_ROW) if(columnText(stmt.get(),1)==column) return true;
    return false;
}
bool ftsTriggersPresent(sqlite3* db) {
    auto stmt=prepare(db,"SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND name IN ('files_name_fts_ai','files_name_fts_ad','files_name_fts_au')");
    return sqlite3_step(stmt.get())==SQLITE_ROW && sqlite3_column_int(stmt.get(),0)==3;
}
bool schemaReady(sqlite3* db) {
    if(!tableExists(db,"app_metadata") || !columnExists(db,"app_metadata","key") || !columnExists(db,"app_metadata","value")) return false;
    if(!metadataFlag(db,"schema_413_ready_v1")) return false;
    auto version=prepare(db,"PRAGMA user_version");
    if(sqlite3_step(version.get())!=SQLITE_ROW || sqlite3_column_int(version.get(),0)!=413) return false;
    auto objects=prepare(db,R"SQL(SELECT COUNT(*) FROM sqlite_master WHERE
(type='table' AND name IN ('files','sources','roots','scan_seen','compatibility_scans','exclusion_policy','exclusion_paths')) OR
(type='index' AND name IN ('idx_sources_root','idx_files_search_name','idx_files_ext','idx_files_size','idx_files_root_generation','idx_files_modified','idx_files_path')))SQL");
    if(sqlite3_step(objects.get())!=SQLITE_ROW || sqlite3_column_int(objects.get(),0)!=14) return false;
    // Preparing LIMIT 0 checks the columns used by current callers, without
    // reading files or counting index contents. A stamp alone is insufficient.
    try {
        prepare(db,"SELECT id,source_id,relative_path,path,parent_path,name,ext,root,size,modified_time,is_dir,search_name,search_path,scan_generation,cloud_copy FROM files LIMIT 0");
        prepare(db,"SELECT id,label,type,mode,status,interval_seconds,identity,root,mount_fingerprint,last_check,last_success,revision FROM sources LIMIT 0");
        prepare(db,"SELECT source_id,generation,relative_path FROM scan_seen LIMIT 0");
        prepare(db,"SELECT root,generation,source_id,source_revision,policy_revision,identity,mount_fingerprint,state FROM compatibility_scans LIMIT 0");
        prepare(db,"SELECT root,generation,last_scan_time,file_count FROM roots LIMIT 0");
        prepare(db,"SELECT kind,position,path FROM exclusion_paths LIMIT 0");
        auto policy=prepare(db,"SELECT revision,folder_filter,file_filter FROM exclusion_policy WHERE id=1");
        if(sqlite3_step(policy.get())!=SQLITE_ROW) return false;
    } catch(const std::runtime_error&) { return false; }
    const bool available=tableExists(db,"files_name_fts");
    if(available!=metadataFlag(db,"schema_413_fts_available")) return false;
    if(!available) return !metadataFlag(db,"name_fts_ready");
    return !metadataFlag(db,"name_fts_ready") || ftsTriggersPresent(db);
}
std::string backupDatabase(sqlite3* writer,const std::string& path) {
    // The caller holds BEGIN IMMEDIATE on writer. A separate read-only handle
    // sees the same committed snapshot while every other writer is excluded;
    // backing up the connection with an active write transaction can be BUSY.
    sqlite3* source_raw=nullptr;
    if(sqlite3_open_v2(path.c_str(),&source_raw,SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK) {
        if(source_raw) sqlite3_close(source_raw);
        throw std::runtime_error("unable to open migration backup source");
    }
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> source(source_raw,sqlite3_close);
    sqlite3_busy_timeout(source_raw,5000);
    if(sqlite3_exec(source_raw,"BEGIN",nullptr,nullptr,nullptr)!=SQLITE_OK)
        throw std::runtime_error("unable to establish migration backup snapshot");
    const auto ticks=std::chrono::system_clock::now().time_since_epoch().count();
    auto backup_path=path+".pre-source-catalog-"+std::to_string(ticks)+".backup.sqlite";
    if(std::filesystem::exists(utf8ToPath(backup_path))) throw std::runtime_error("migration backup already exists");
    sqlite3* raw=nullptr;
    if(sqlite3_open_v2(backup_path.c_str(),&raw,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,nullptr)!=SQLITE_OK) {
        if(raw) sqlite3_close(raw);
        throw std::runtime_error("unable to create migration backup");
    }
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> target(raw,sqlite3_close);
    sqlite3_backup* backup=sqlite3_backup_init(raw,"main",source_raw,"main");
    if(!backup) throw std::runtime_error("unable to initialize migration backup");
    const auto rc=sqlite3_backup_step(backup,-1);
    const auto finish=sqlite3_backup_finish(backup);
    if(rc!=SQLITE_DONE || finish!=SQLITE_OK) throw std::runtime_error("migration backup failed");
    // Publish a self-contained backup file even when the live index uses WAL.
    if(sqlite3_exec(raw,"PRAGMA journal_mode=DELETE",nullptr,nullptr,nullptr)!=SQLITE_OK)
        throw std::runtime_error("unable to finalize migration backup journal");
    if(countRows(raw,"files")!=countRows(writer,"files") ||
       countRows(raw,"sqlite_master")!=countRows(writer,"sqlite_master") ||
       (tableExists(writer,"roots") && countRows(raw,"roots")!=countRows(writer,"roots")))
        throw std::runtime_error("migration backup schema or row count mismatch");
    auto integrity=prepare(raw,"PRAGMA integrity_check");
    if(sqlite3_step(integrity.get())!=SQLITE_ROW || columnText(integrity.get(),0)!="ok")
        throw std::runtime_error("migration backup integrity check failed");
    return backup_path;
}

bool relativeWithin(const std::string& path,const std::string& scope) {
    return scope.empty() || scope=="." || path==scope ||
        (path.size()>scope.size() && path.compare(0,scope.size(),scope)==0 && path[scope.size()]=='/');
}
void validateScope(const std::string& scope) {
    if(scope.empty() || scope==".") return;
    const auto parsed=utf8ToPath(scope);
    if(parsed.is_absolute() || pathToUtf8(parsed.lexically_normal())!=scope) throw std::invalid_argument("invalid subtree scope");
    for(const auto& part:parsed) if(part=="..") throw std::invalid_argument("subtree escapes source");
}
IndexStats mergeSnapshot(const SqliteConnection& conn,const SourceRecord& expected,
                         const std::function<bool(std::vector<FileRecord>&)>& next,
                         const std::string& scope,
                         const std::function<void(sqlite3*)>& admission={},
                         const ExclusionPolicy* captured_policy=nullptr,
                         const std::vector<std::string>* protected_scopes=nullptr) {
    validateScope(scope);
    // Snapshot membership grows with the source. Keep its temporary B-tree on
    // disk, while the record transfer remains bounded to batches of 1000.
    conn.exec("PRAGMA temp_store=FILE;");
    conn.exec("BEGIN IMMEDIATE;");
    try {
        const auto s=getSource(conn.get(),expected.id);
        if(s.revision!=expected.revision || s.identity!=expected.identity || s.root!=expected.root || s.mount_fingerprint!=expected.mount_fingerprint)
            throw std::runtime_error("source binding changed during scan");
        const auto current_policy=getPolicy(conn.get());
        const auto& policy=captured_policy?*captured_policy:current_policy;
        if(current_policy.revision!=expected.policy_revision || policy.revision!=expected.policy_revision) throw std::runtime_error("exclusion policy changed during scan");
        if(admission)admission(conn.get());
        const bool compact=hasDirectoryStorage(conn.get());
        conn.exec(compact?"CREATE TEMP TABLE snapshot_keys(entry_id INTEGER PRIMARY KEY);":"CREATE TEMP TABLE snapshot_keys(relative_path TEXT PRIMARY KEY);");
        auto keys=prepare(conn.get(),"INSERT INTO snapshot_keys VALUES(?)");
        std::unique_ptr<DirectoryRecordWriter> compact_writer;
        if(compact)compact_writer=std::make_unique<DirectoryRecordWriter>(conn.get());
        std::vector<FileRecord> records; std::uint64_t count=0;
        while(next(records)) {
            std::vector<FileRecord> accepted; accepted.reserve(records.size());
            for(const auto& record:records) {
                const auto relative=relativeTo(record.path,s.root);
                if(record.source_id!=0 && record.source_id!=s.id) throw std::invalid_argument("wrong source record");
                if(!record.relative_path.empty() && record.relative_path!=relative) throw std::invalid_argument("relative path mismatch");
                if(!relativeWithin(relative,scope)) throw std::invalid_argument("record outside snapshot scope");
                if(policy.excludes(record.path,record.is_directory)) continue;
                accepted.push_back(record);
                sqlite3_reset(keys.get()); sqlite3_clear_bindings(keys.get());
                if(compact) {
                    compact_writer->upsert(s.id,record,relative,0);
                    sqlite3_bind_int64(keys.get(),1,compact_writer->entryId(s.id,relative));
                } else bindText(keys.get(),1,relative);
                if(sqlite3_step(keys.get())!=SQLITE_DONE) throw std::runtime_error("duplicate snapshot path");
            }
            if(!compact)upsertRecords(conn.get(),s,accepted,0,policy); count+=accepted.size(); records.clear();
        }
        std::uint64_t retained_count=0;
        // Pruned but unconfirmed/offline children cannot prove deletion of
        // the parent's last historical copy. Preserve only existing own keys;
        // never copy records to another source or read a child filesystem.
        if(protected_scopes && !protected_scopes->empty()) {
            // Separate equality/range probes keep both predicates on the
            // (source_id,relative_path) index, including large offline caches.
            if(compact) {
                for(const auto& protected_scope:*protected_scopes) {
                    if(!relativeWithin(protected_scope,scope))continue;
                    for(const auto& query:directoryScopeQueries("INSERT OR IGNORE INTO snapshot_keys SELECT id FROM file_entries WHERE ","",protected_scope)) {
                        auto retain=prepare(conn.get(),query.sql);sqlite3_bind_int64(retain.get(),1,s.id);
                        for(std::size_t i=0;i<query.bounds.size();++i)bindText(retain.get(),static_cast<int>(i)+2,query.bounds[i]);
                        if(sqlite3_step(retain.get())!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(conn.get()));
                        retained_count+=static_cast<std::uint64_t>(sqlite3_changes(conn.get()));
                    }
                }
            } else {
            auto exact=prepare(conn.get(),"INSERT OR IGNORE INTO snapshot_keys SELECT relative_path FROM files WHERE source_id=? AND relative_path=?");
            auto subtree=prepare(conn.get(),"INSERT OR IGNORE INTO snapshot_keys SELECT relative_path FROM files WHERE source_id=? AND relative_path>=? AND relative_path<?");
            const auto retain=[&](sqlite3_stmt* statement,const std::string& lower,const std::string& upper) {
                sqlite3_reset(statement); sqlite3_clear_bindings(statement);
                sqlite3_bind_int64(statement,1,s.id); bindText(statement,2,lower);
                if(!upper.empty())bindText(statement,3,upper);
                if(sqlite3_step(statement)!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(conn.get()));
                retained_count+=static_cast<std::uint64_t>(sqlite3_changes(conn.get()));
            };
            for(const auto& protected_scope:*protected_scopes) {
                if(!relativeWithin(protected_scope,scope))continue;
                retain(exact.get(),protected_scope,{});
                retain(subtree.get(),protected_scope+"/",protected_scope+"0");
            }
            }
        }
        const bool full=scope.empty() || scope==".";
        IndexStats stats; stats.indexed=count; stats.completed=true;
        const auto removeMissing=[&](const std::string& sql,const std::vector<std::string>& bounds) {
            auto remove=prepare(conn.get(),sql);
            sqlite3_bind_int64(remove.get(),1,s.id);
            for(std::size_t i=0;i<bounds.size();++i) bindText(remove.get(),static_cast<int>(i)+2,bounds[i]);
            if(sqlite3_step(remove.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
            stats.removed+=static_cast<std::uint64_t>(sqlite3_changes(conn.get()));
            if(searchTraceEnabled()) searchTrace("Database", "merge_delete vm_steps="+std::to_string(sqlite3_stmt_status(remove.get(),SQLITE_STMTSTATUS_VM_STEP,0)));
        };
        const std::string missing=" AND NOT EXISTS(SELECT 1 FROM snapshot_keys WHERE snapshot_keys.relative_path=files.relative_path)";
        if(compact) {
            const std::string compact_missing=" AND NOT EXISTS(SELECT 1 FROM snapshot_keys WHERE snapshot_keys.entry_id=file_entries.id)";
            for(const auto& query:directoryScopeQueries("DELETE FROM file_entries WHERE ",compact_missing,scope))removeMissing(query.sql,query.bounds);
        } else if(full) {
            removeMissing("DELETE FROM files WHERE source_id=?"+missing,{});
        } else {
            // Under BINARY collation, '/' (0x2f) immediately precedes '0'
            // (0x30). These bounds include only scope descendants regardless
            // of UTF-8 bytes, quotes or SQL wildcard characters in the scope.
            // Separate equality/range probes reuse UNIQUE(source_id,relative_path)
            // and avoid visiting unrelated entries in the same source.
            removeMissing("DELETE FROM files WHERE source_id=? AND relative_path=?"+missing,{scope});
            removeMissing("DELETE FROM files WHERE source_id=? AND relative_path>=? AND relative_path<?"+missing,{scope+"/",scope+"0"});
        }
        auto update=prepare(conn.get(),full ?
            "UPDATE sources SET status=0,last_check=strftime('%s','now'),last_success=strftime('%s','now'),revision=revision+1 WHERE id=?" :
            "UPDATE sources SET last_check=strftime('%s','now'),revision=revision+1 WHERE id=?");
        sqlite3_bind_int64(update.get(),1,s.id);
        if(sqlite3_step(update.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
        if(full) {
            auto root=prepare(conn.get(),"INSERT INTO roots(root,generation,last_scan_time,file_count) VALUES(?,0,strftime('%s','now'),?) ON CONFLICT(root) DO UPDATE SET last_scan_time=excluded.last_scan_time,file_count=excluded.file_count");
            bindText(root.get(),1,s.root); sqlite3_bind_int64(root.get(),2,count+retained_count);
            if(sqlite3_step(root.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
        }
        if(tableExists(conn.get(),"files_name_fts") && !metadataFlag(conn.get(),"name_fts_ready")) {
            conn.exec("INSERT INTO files_name_fts(files_name_fts) VALUES('rebuild');");
            if(compact)conn.exec("INSERT INTO files_name_fts(files_name_fts,rank) VALUES('integrity-check',1);");
            installFtsTriggers(conn); setMetadataFlag(conn.get(),"name_fts_ready",true);
        }
        conn.exec("COMMIT;"); return stats;
    } catch (...) { conn.exec("ROLLBACK;"); throw; }
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

std::vector<std::string> compactGrams(const std::string& term) {
    std::vector<std::size_t> offsets;
    for(std::size_t i=0;i<term.size();) {
        offsets.push_back(i);const unsigned char c=term[i];
        const std::size_t bytes=c<0x80?1:(c>=0xc2 && c<=0xdf?2:(c>=0xe0 && c<=0xef?3:(c>=0xf0 && c<=0xf4?4:0)));
        if(!bytes || i+bytes>term.size() || c==0)return {};
        for(std::size_t j=1;j<bytes;++j)if((static_cast<unsigned char>(term[i+j])&0xc0)!=0x80)return {};
        if(bytes>=3) {
            const auto second=static_cast<unsigned char>(term[i+1]);
            if((c==0xe0 && second<0xa0)||(c==0xed && second>=0xa0)||(c==0xf0 && second<0x90)||(c==0xf4 && second>=0x90))return {};
        }
        i+=bytes;
    }
    offsets.push_back(term.size());if(offsets.size()<4)return {};
    const std::size_t last=offsets.size()-4;
    std::vector<std::string> grams;
    for(const auto i:{std::size_t{0},last/2,last}) {
        const auto gram=term.substr(offsets[i],offsets[i+3]-offsets[i]);
        if(std::find(grams.begin(),grams.end(),gram)==grams.end())grams.push_back(gram);
    }
    return grams;
}

bool canUseNameFts(const SearchQuery& query, bool fts_ready,bool compact) {
    if (!fts_ready || query.match_path || query.terms.empty()) return false;
    for (const auto& term : query.terms) {
        if (compact?compactGrams(term).empty():utf8CodepointCount(term)<3) return false;
    }
    return true;
}

std::string makeFtsExpression(const SearchQuery& query,bool compact) {
    std::ostringstream out;
    for (std::size_t i = 0; i < query.terms.size(); ++i) {
        if (i) out << " AND ";
        if(compact) {
            const auto grams=compactGrams(query.terms[i]);
            for(std::size_t j=0;j<grams.size();++j) {if(j)out<<" AND ";out<<quoteFtsPhrase(grams[j]);}
        } else out << quoteFtsPhrase(query.terms[i]);
    }
    return out.str();
}

int cancelProgressCallback(void* opaque) {
    const auto* cancel = static_cast<const std::atomic_bool*>(opaque);
    return cancel && cancel->load(std::memory_order_relaxed) ? 1 : 0;
}

std::vector<SearchResult> runSearch(sqlite3* db,
                                    const std::string& db_path,
                                    const SearchQuery& query,
                                    std::size_t limit,
                                    std::size_t offset,
                                    bool fts_ready,
                                    const std::atomic_bool* cancel,
                                    bool force_direct = false) {
    const bool compact=hasDirectoryStorage(db);
    const bool use_fts = !force_direct && canUseNameFts(query, fts_ready,compact);
    const auto trace_started = std::chrono::steady_clock::now();

    if (searchTraceEnabled()) {
        std::ostringstream t;
        t << "db=\"" << db_path << "\""
          << " sqlite_version=" << sqlite3_libversion()
          << " sqlite_sourceid=\"" << sqlite3_sourceid() << "\""
          << " fts_ready=" << (fts_ready ? 1 : 0)
          << " force_direct=" << (force_direct ? 1 : 0)
          << " use_fts=" << (use_fts ? 1 : 0)
          << " terms=" << query.terms.size()
          << " match_path=" << (query.match_path ? 1 : 0)
          << " files_only=" << (query.files_only ? 1 : 0)
          << " directories_only=" << (query.directories_only ? 1 : 0)
          << " limit=" << limit << " offset=" << offset;
        searchTrace("Database", t.str());
        for (std::size_t i = 0; i < query.terms.size(); ++i) {
            searchTrace("Database", "term[" + std::to_string(i) + "]=\"" + query.terms[i] + "\" hex=[" + bytesToHex(query.terms[i]) + "]");
        }
        if (query.path_term) searchTrace("Database", "path_term=\"" + *query.path_term + "\"");
        if (query.extension) searchTrace("Database", "extension=\"" + *query.extension + "\"");
    }

    std::ostringstream sql;
    if (use_fts) {
        sql << "SELECT files.path,files.parent_path,files.name,files.ext,files.root,files.size,files.modified_time,files.is_dir,files.scan_generation,files.source_id,files.relative_path,files.cloud_copy,sources.label,sources.type,sources.status,sources.last_check,sources.last_success "
               "FROM files_name_fts JOIN files ON files.id=files_name_fts.rowid JOIN sources ON sources.id=files.source_id";
    } else {
        sql << "SELECT files.path,files.parent_path,files.name,files.ext,files.root,files.size,files.modified_time,files.is_dir,files.scan_generation,files.source_id,files.relative_path,files.cloud_copy,sources.label,sources.type,sources.status,sources.last_check,sources.last_success FROM files JOIN sources ON sources.id=files.source_id";
    }

    std::vector<std::string> where;
    if (use_fts) {
        where.emplace_back("files_name_fts MATCH ?");
    }
    if(!use_fts || compact) {
        for (std::size_t i = 0; i < query.terms.size(); ++i) {
            // Everything-compatible default: plain terms match the basename only.
            // Full-path matching is opt-in via Match Path.
            // Correctness path uses instr() instead of LIKE. Both search_name
            // and query terms are already ASCII-folded, while non-ASCII UTF-8
            // bytes are preserved exactly. This avoids wildcard/collation/LIKE
            // behaviour becoming a source of false negatives.
            where.emplace_back(query.match_path
                ? "instr(files.search_path, ?) > 0"
                : "instr(files.search_name, ?) > 0");
        }
    }
    if (query.source_id) where.emplace_back("files.source_id = ?");
    if (query.source_status) where.emplace_back("sources.status = ?");
    if (query.extension) where.emplace_back("files.ext = ? COLLATE NOCASE");
    if (query.path_term) {
        where.emplace_back((force_direct || compact)
            ? "instr(files.search_path, ?) > 0"
            : "files.search_path LIKE ? ESCAPE '\\'");
    }
    std::string folder_lower,folder_upper;
    if(query.folder_scope) {
        if(query.folder_scope->empty() || !utf8ToPath(*query.folder_scope).is_absolute())
            throw std::invalid_argument("folder scope must be an absolute directory path");
        const auto folder=normalizePath(*query.folder_scope);
        folder_lower=folder=="/" ? "/" : folder+"/";
        folder_upper=folder=="/" ? "0" : folder+"0";
        where.emplace_back("files.path >= ? AND files.path < ?");
    }
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

    // search_name is ASCII-folded name; its BINARY order equals name NOCASE.
    // Reuse the existing name index for direct/short queries and sort only
    // equal-name parent groups. FTS retains its selective query plan.
    sql << " ORDER BY files.search_name ASC, files.parent_path COLLATE NOCASE ASC, files.id ASC LIMIT ? OFFSET ?";

    searchTrace("Database", "SQL=" + sql.str());
    if (use_fts) searchTrace("Database", "bind[1] FTS=\"" + makeFtsExpression(query,compact) + "\"");

    if (cancel) {
        sqlite3_progress_handler(db, 1000, cancelProgressCallback, const_cast<std::atomic_bool*>(cancel));
    }

    auto stmt = prepare(db, sql.str());
    int bind_index = 1;
    if (use_fts) {
        bindText(stmt.get(), bind_index++, makeFtsExpression(query,compact));
    }
    if(!use_fts || compact) {
        for (const auto& token : query.terms) {
            searchTrace("Database", "bind[" + std::to_string(bind_index) + "] term=\"" + token + "\" hex=[" + bytesToHex(token) + "]");
            bindText(stmt.get(), bind_index++, token);
        }
    }
    if (query.source_id) sqlite3_bind_int64(stmt.get(), bind_index++, *query.source_id);
    if (query.source_status) sqlite3_bind_int(stmt.get(), bind_index++, static_cast<int>(*query.source_status));
    if (query.extension) bindText(stmt.get(), bind_index++, *query.extension);
    if (query.path_term) {
        if (force_direct || compact) bindText(stmt.get(), bind_index++, *query.path_term);
        else bindText(stmt.get(), bind_index++, "%" + escapeLike(*query.path_term) + "%");
    }
    if(query.folder_scope) {
        bindText(stmt.get(),bind_index++,folder_lower);
        bindText(stmt.get(),bind_index++,folder_upper);
    }
    if (query.min_size) sqlite3_bind_int64(stmt.get(), bind_index++, static_cast<sqlite3_int64>(*query.min_size));
    if (query.max_size) sqlite3_bind_int64(stmt.get(), bind_index++, static_cast<sqlite3_int64>(*query.max_size));
    if (query.modified_after) sqlite3_bind_int64(stmt.get(), bind_index++, *query.modified_after);
    searchTrace("Database", "bind[" + std::to_string(bind_index) + "] limit=" + std::to_string(limit));
    sqlite3_bind_int64(stmt.get(), bind_index++, static_cast<sqlite3_int64>(limit));
    searchTrace("Database", "bind[" + std::to_string(bind_index) + "] offset=" + std::to_string(offset));
    sqlite3_bind_int64(stmt.get(), bind_index, static_cast<sqlite3_int64>(offset));

    std::vector<SearchResult> results;
    results.reserve(limit);
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        SearchResult result;
        result.file = readFileRecord(stmt.get());
        result.rank = 0;
        result.source_label = columnText(stmt.get(), 12);
        result.source_type = static_cast<SourceType>(sqlite3_column_int(stmt.get(), 13));
        result.source_status = static_cast<SourceStatus>(sqlite3_column_int(stmt.get(), 14));
        result.source_last_check = sqlite3_column_int64(stmt.get(), 15);
        result.source_last_success = sqlite3_column_int64(stmt.get(), 16);
        results.push_back(std::move(result));
    }
    if (cancel) sqlite3_progress_handler(db, 0, nullptr, nullptr);
    if (rc == SQLITE_INTERRUPT || (cancel && cancel->load(std::memory_order_relaxed))) {
        throw std::runtime_error("search cancelled");
    }
    if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
    if (searchTraceEnabled()) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - trace_started).count();
        searchTrace("Database", "sqlite_step_done result_count=" + std::to_string(results.size()) + " elapsed_ms=" + std::to_string(elapsed) + " vm_steps=" + std::to_string(sqlite3_stmt_status(stmt.get(),SQLITE_STMTSTATUS_VM_STEP,0)));
        const std::size_t preview = std::min<std::size_t>(results.size(), 5);
        for (std::size_t i = 0; i < preview; ++i) {
            searchTrace("Database", "result[" + std::to_string(i) + "]=\"" + results[i].file.path + "\"");
        }
    }
    return results;
}

} // namespace

bool sourceCatalogSchemaReady(sqlite3* db) {
    try {return schemaReady(db);}catch(const std::exception&) {return false;}
}

Database::Database(std::string db_path) : db_path_(std::move(db_path)) {}

void Database::initialize() const {
    SqliteConnection conn(db_path_);
    // Use one committed read snapshot for the stamp and shape checks. Repeated
    // batch/statistics initializers must not compete for the WAL writer lock.
    conn.exec("BEGIN;");
    try {
        const bool compact=hasDirectoryStorage(conn.get());
        const bool ready=compact?directoryStorageReady(conn.get()):schemaReady(conn.get());
        if(compact && !ready)throw std::runtime_error("unsupported or damaged directory storage; restore the verified recovery backup");
        conn.exec("COMMIT;");
        if(ready) return;
    } catch (...) { conn.exec("ROLLBACK;"); throw; }
    conn.exec("BEGIN IMMEDIATE;");
    try {
        if(hasDirectoryStorage(conn.get())) {
            if(!directoryStorageReady(conn.get()))throw std::runtime_error("unsupported or damaged directory storage; restore the verified recovery backup");
            conn.exec("COMMIT;");return;
        }
        if(schemaReady(conn.get())) { conn.exec("COMMIT;"); return; }
        // Recheck only after taking the writer lock: another initializer may
        // already have migrated the schema while this connection was waiting.
        const bool legacy=tableExists(conn.get(),"files") && !columnExists(conn.get(),"files","source_id");
        const auto old_count=legacy ? countRows(conn.get(),"files") : 0;
        const auto backup=legacy ? backupDatabase(conn.get(),db_path_) : std::string{};
        conn.exec(R"SQL(
CREATE TABLE IF NOT EXISTS sources (
 id INTEGER PRIMARY KEY AUTOINCREMENT,label TEXT NOT NULL DEFAULT '',type INTEGER NOT NULL DEFAULT 5,
 mode INTEGER NOT NULL DEFAULT 1,status INTEGER NOT NULL DEFAULT 2,interval_seconds INTEGER NOT NULL DEFAULT 900 CHECK(interval_seconds>=0),
 identity TEXT NOT NULL DEFAULT '',root TEXT NOT NULL,mount_fingerprint TEXT NOT NULL DEFAULT '',
 last_check INTEGER NOT NULL DEFAULT 0,last_success INTEGER NOT NULL DEFAULT 0,revision INTEGER NOT NULL DEFAULT 1
);
CREATE INDEX IF NOT EXISTS idx_sources_root ON sources(root);
CREATE TABLE IF NOT EXISTS scan_seen(source_id INTEGER NOT NULL,generation INTEGER NOT NULL,relative_path TEXT NOT NULL,PRIMARY KEY(source_id,generation,relative_path));
CREATE TABLE IF NOT EXISTS compatibility_scans (
 root TEXT NOT NULL,generation INTEGER NOT NULL,source_id INTEGER NOT NULL,source_revision INTEGER NOT NULL,
 policy_revision INTEGER NOT NULL,identity TEXT NOT NULL,mount_fingerprint TEXT NOT NULL,
 state INTEGER NOT NULL DEFAULT 0 CHECK(state IN (0,1,2)),PRIMARY KEY(root,generation)
);
CREATE TABLE IF NOT EXISTS roots(root TEXT PRIMARY KEY,generation INTEGER NOT NULL DEFAULT 0,last_scan_time INTEGER NOT NULL DEFAULT 0,file_count INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS exclusion_policy (
 id INTEGER PRIMARY KEY CHECK(id=1), revision INTEGER NOT NULL DEFAULT 0,
 folder_filter TEXT NOT NULL DEFAULT '', file_filter TEXT NOT NULL DEFAULT ''
);
INSERT OR IGNORE INTO exclusion_policy(id) VALUES(1);
CREATE TABLE IF NOT EXISTS exclusion_paths (
 kind INTEGER NOT NULL CHECK(kind IN (0,1)),position INTEGER NOT NULL,path TEXT NOT NULL,
 PRIMARY KEY(kind,position)
);
CREATE TABLE IF NOT EXISTS app_metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);
INSERT OR IGNORE INTO app_metadata(key,value) VALUES('name_fts_ready','0');
)SQL");
        if(legacy) {
            conn.exec(R"SQL(
DROP TRIGGER IF EXISTS files_name_fts_ai;
DROP TRIGGER IF EXISTS files_name_fts_ad;
DROP TRIGGER IF EXISTS files_name_fts_au;
DROP TABLE IF EXISTS files_name_fts;
ALTER TABLE files RENAME TO files_legacy;
INSERT INTO sources(label,root,last_check,last_success)
SELECT root,root,COALESCE((SELECT last_scan_time FROM roots WHERE roots.root=bindings.root),0),
COALESCE((SELECT last_scan_time FROM roots WHERE roots.root=bindings.root),0)
FROM (SELECT root FROM files_legacy UNION SELECT root FROM roots) AS bindings;
)SQL");
        }
        conn.exec(R"SQL(
CREATE TABLE IF NOT EXISTS files (
 id INTEGER PRIMARY KEY,source_id INTEGER NOT NULL REFERENCES sources(id),relative_path TEXT NOT NULL,
 path TEXT NOT NULL,parent_path TEXT NOT NULL,name TEXT NOT NULL,ext TEXT NOT NULL DEFAULT '',root TEXT NOT NULL,
 size INTEGER NOT NULL DEFAULT 0,modified_time INTEGER NOT NULL DEFAULT 0,is_dir INTEGER NOT NULL DEFAULT 0,
 search_name TEXT NOT NULL,search_path TEXT NOT NULL,scan_generation INTEGER NOT NULL DEFAULT 0,
 cloud_copy INTEGER NOT NULL DEFAULT 2,UNIQUE(source_id,relative_path)
);
)SQL");
        if(legacy) {
            conn.exec(R"SQL(
INSERT INTO files(id,source_id,relative_path,path,parent_path,name,ext,root,size,modified_time,is_dir,search_name,search_path,scan_generation)
SELECT f.id,s.id,CASE WHEN f.path=f.root THEN '.' WHEN f.root='/' THEN substr(f.path,2)
WHEN substr(f.path,1,length(f.root)+1)=f.root||'/' THEN substr(f.path,length(f.root)+2) ELSE f.path END,
f.path,f.parent_path,f.name,f.ext,f.root,f.size,f.modified_time,f.is_dir,f.search_name,f.search_path,f.scan_generation
FROM files_legacy f JOIN sources s ON s.root=f.root;
)SQL");
            if(countRows(conn.get(),"files")!=old_count) throw std::runtime_error("migration file count mismatch");
            conn.exec("DROP TABLE files_legacy;");
            auto stmt=prepare(conn.get(),"INSERT OR REPLACE INTO app_metadata(key,value) VALUES('source_migration_backup',?)");
            bindText(stmt.get(),1,backup);
            if(sqlite3_step(stmt.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
            setMetadataFlag(conn.get(),"name_fts_ready",false);
        }
        conn.exec(R"SQL(
CREATE INDEX IF NOT EXISTS idx_files_search_name ON files(search_name);
CREATE INDEX IF NOT EXISTS idx_files_ext ON files(ext COLLATE NOCASE);
CREATE INDEX IF NOT EXISTS idx_files_size ON files(size);
CREATE INDEX IF NOT EXISTS idx_files_root_generation ON files(source_id,scan_generation);
CREATE INDEX IF NOT EXISTS idx_files_modified ON files(modified_time DESC);
CREATE INDEX IF NOT EXISTS idx_files_path ON files(path);
)SQL");
        const bool had_fts=tableExists(conn.get(),"files_name_fts");
        const bool fts_ok=conn.tryExec(R"SQL(CREATE VIRTUAL TABLE IF NOT EXISTS files_name_fts USING fts5(search_name,content='files',content_rowid='id',tokenize='trigram');)SQL");
        if(!had_fts) setMetadataFlag(conn.get(),"name_fts_ready",false);
        if(fts_ok && legacy) {
            conn.exec("INSERT INTO files_name_fts(files_name_fts) VALUES('rebuild');");
            conn.exec("INSERT INTO files_name_fts(files_name_fts,rank) VALUES('integrity-check',1);");
            setMetadataFlag(conn.get(),"name_fts_ready",true);
        }
        if(fts_ok && metadataFlag(conn.get(),"name_fts_ready")) {
            if(!legacy && !ftsTriggersPresent(conn.get())) {
                // An absent maintenance trigger may have missed earlier writes.
                // Keep canonical searches until a complete rebuild restores FTS.
                setMetadataFlag(conn.get(),"name_fts_ready",false);
            } else installFtsTriggers(conn);
        }
        if(!fts_ok) setMetadataFlag(conn.get(),"name_fts_ready",false);
        if(!metadataFlag(conn.get(),"name_fts_ready")) {
            // Do not apply delete/update commands to an incomplete external-
            // content index. The next complete snapshot rebuilds it atomically.
            conn.exec("DROP TRIGGER IF EXISTS files_name_fts_ai; DROP TRIGGER IF EXISTS files_name_fts_ad; DROP TRIGGER IF EXISTS files_name_fts_au;");
        }
        setMetadataFlag(conn.get(),"schema_413_fts_available",fts_ok);
        setMetadataFlag(conn.get(),"schema_413_ready_v1",true);
        conn.exec("PRAGMA user_version=413; COMMIT;");
    } catch (...) { conn.exec("ROLLBACK;"); throw; }
}

ExclusionPolicy Database::exclusionPolicy() const {
    SqliteConnection conn(db_path_); conn.exec("BEGIN;"); return getPolicy(conn.get());
}
std::uint64_t Database::saveExclusionPolicy(const ExclusionRules& rules) const {
    const ExclusionPolicy validated(rules); // invalid rules must not touch storage
    SqliteConnection conn(db_path_); conn.exec("PRAGMA temp_store=FILE; BEGIN IMMEDIATE;");
    try {
        auto update=prepare(conn.get(),"UPDATE exclusion_policy SET revision=revision+1,folder_filter=?,file_filter=? WHERE id=1");
        bindText(update.get(),1,validated.rules().folder_filter); bindText(update.get(),2,validated.rules().file_filter);
        if(sqlite3_step(update.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
        conn.exec("DELETE FROM exclusion_paths;");
        auto paths=prepare(conn.get(),"INSERT INTO exclusion_paths(kind,position,path) VALUES(?,?,?)");
        int kind=0;
        for(const auto* list: {&validated.rules().directories,&validated.rules().files}) {
            std::int64_t position=0;
            for(const auto& path:*list) {
                sqlite3_reset(paths.get()); sqlite3_clear_bindings(paths.get());
                sqlite3_bind_int(paths.get(),1,kind); sqlite3_bind_int64(paths.get(),2,position++); bindText(paths.get(),3,path);
                if(sqlite3_step(paths.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
            }
            ++kind;
        }
        const auto removed=purgeExcluded(conn,validated);
        conn.exec("COMMIT;"); return removed;
    } catch(...) { conn.exec("ROLLBACK;"); throw; }
}

SourceVerificationResult Database::publishSourceVerification(const SourceVerificationEvidence& evidence, VerificationMode mode) const {
    const auto& expected=evidence.expected;validateSource(expected);
    if(expected.id<=0 || !normalizedAbsolute(expected.root) ||
       (mode!=VerificationMode::Automatic && mode!=VerificationMode::Explicit))
        throw std::invalid_argument("invalid source verification request");
    SqliteConnection conn(db_path_);conn.exec("BEGIN IMMEDIATE;");
    try {
        const auto current=getSource(conn.get(),expected.id);
        // Policy is independent of source identity. All persistent catalog fields
        // must still match; cache writes through the API advance this revision.
        if(current.revision!=expected.revision || current.root!=expected.root || current.identity!=expected.identity ||
           current.mount_fingerprint!=expected.mount_fingerprint || current.label!=expected.label || current.type!=expected.type ||
           current.mode!=expected.mode || current.status!=expected.status || current.interval_seconds!=expected.interval_seconds ||
           current.last_check!=expected.last_check || current.last_success!=expected.last_success)
            throw std::runtime_error("source changed during verification");
        SourceVerificationResult result;const auto& observed=evidence.initial_before;
        bool accepted=stableSourceVerification(evidence);
        if(!current.identity.empty() && !observed.identity.empty() && current.identity!=observed.identity) {
            accepted=false;result.issue=ProbeIssue::IdentityChanged;result.error="source identity changed; forget and add a new source";
        } else if(accepted && mode==VerificationMode::Automatic) {
            if(current.identity.empty()) {
                // This check belongs INSIDE the write transaction. A legacy
                // cache appearing after observation requires explicit consent.
                auto cache=prepare(conn.get(),"SELECT 1 FROM files WHERE source_id=? LIMIT 1");
                sqlite3_bind_int64(cache.get(),1,current.id);const auto rc=sqlite3_step(cache.get());
                if(rc!=SQLITE_ROW && rc!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(conn.get()));
                if(rc==SQLITE_ROW) {accepted=false;result.error="cached source requires explicit verification";}
            } else if(!sameSourceDirectoryInstance(current.mount_fingerprint,observed.mount_fingerprint)) {
                accepted=false;result.issue=ProbeIssue::ScopeChanged;result.error="directory instance requires explicit verification";
            }
        }
        if(accepted) {
            result.published=true;result.status=SourceStatus::Accessible;result.issue=ProbeIssue::None;
        } else {
            const bool classified=!result.error.empty();
            if(!classified)result.error="source verification did not establish stable native evidence";
            // Actual offline/failed evidence takes precedence over a mismatch
            // inferred from an unavailable expected network slot or failed end.
            for(const auto* p:{&evidence.initial_before,&evidence.candidate_before,&evidence.candidate_after,&evidence.initial_after}) {
                if(p->status==SourceStatus::Offline || p->status==SourceStatus::Failed) {
                    result.status=p->status;result.issue=p->issue;result.error="source unavailable during verification";break;
                }
                if(!classified && p->issue!=ProbeIssue::None && p->issue!=ProbeIssue::NeedsVerification)result.issue=p->issue;
            }
        }
        auto update=prepare(conn.get(),"UPDATE sources SET identity=?,mount_fingerprint=?,type=?,status=?,last_check=?,revision=revision+1 WHERE id=?");
        bindText(update.get(),1,accepted?observed.identity:current.identity);
        bindText(update.get(),2,accepted?observed.mount_fingerprint:current.mount_fingerprint);
        const auto actual_type=(observed.type==SourceType::Local || observed.type==SourceType::External || observed.type==SourceType::NAS)?observed.type:SourceType::Unknown;
        sqlite3_bind_int(update.get(),3,static_cast<int>(accepted && current.type==SourceType::Unknown?actual_type:current.type));
        sqlite3_bind_int(update.get(),4,static_cast<int>(result.status));
        using namespace std::chrono;sqlite3_bind_int64(update.get(),5,duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
        sqlite3_bind_int64(update.get(),6,current.id);
        if(sqlite3_step(update.get())!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(conn.get()));
        conn.exec("COMMIT;");return result;
    } catch(...) {conn.exec("ROLLBACK;");throw;}
}

std::string Database::migrationBackupPath() const {
    SqliteConnection conn(db_path_);
    auto stmt=prepare(conn.get(),"SELECT value FROM app_metadata WHERE key='source_migration_backup'");
    return sqlite3_step(stmt.get())==SQLITE_ROW ? columnText(stmt.get(),0) : std::string{};
}
std::vector<SourceRecord> Database::sources() const {
    SqliteConnection conn(db_path_); conn.exec("BEGIN;");
    const auto policy=getPolicy(conn.get());
    auto stmt=prepare(conn.get(),std::string("SELECT ")+sourceColumns+" FROM sources ORDER BY id");
    std::vector<SourceRecord> out; int rc;
    while((rc=sqlite3_step(stmt.get()))==SQLITE_ROW) {
        auto source=readSource(stmt.get()); source.policy_revision=policy.revision; out.push_back(std::move(source));
    }
    if(rc!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
    return out;
}
SourceRecord Database::source(std::int64_t id) const {
    SqliteConnection conn(db_path_); conn.exec("BEGIN;"); return getSource(conn.get(),id);
}
std::int64_t Database::ensureSourceForRoot(const std::string& root) const {
    SqliteConnection conn(db_path_); conn.exec("BEGIN IMMEDIATE;");
    try { const auto id=sourceForRoot(conn.get(),root,true); conn.exec("COMMIT;"); return id; }
    catch (...) { conn.exec("ROLLBACK;"); throw; }
}
std::int64_t Database::saveSource(const SourceRecord& value) const {
    validateSource(value);
    auto s=value; s.root=normalizePath(value.root);
    SqliteConnection conn(db_path_); conn.exec("BEGIN IMMEDIATE;");
    try {
        SourceRecord old;
        if(s.id) {
            old=getSource(conn.get(),s.id);
            if(s.revision!=old.revision) throw std::runtime_error("source revision changed");
            if(s.identity!=old.identity && !old.identity.empty() && countRows(conn.get(),"files")>0) {
                auto stmt=prepare(conn.get(),"SELECT 1 FROM files WHERE source_id=? LIMIT 1");
                sqlite3_bind_int64(stmt.get(),1,s.id);
                if(sqlite3_step(stmt.get())==SQLITE_ROW) throw std::runtime_error("indexed source identity cannot be replaced");
            }
        }
        auto stmt=prepare(conn.get(),s.id ?
            "UPDATE sources SET label=?,type=?,mode=?,status=?,interval_seconds=?,identity=?,root=?,mount_fingerprint=?,last_check=?,last_success=?,revision=revision+1 WHERE id=?" :
            "INSERT INTO sources(label,type,mode,status,interval_seconds,identity,root,mount_fingerprint,last_check,last_success) VALUES(?,?,?,?,?,?,?,?,?,?)");
        bindText(stmt.get(),1,s.label); sqlite3_bind_int(stmt.get(),2,static_cast<int>(s.type));
        sqlite3_bind_int(stmt.get(),3,static_cast<int>(s.mode)); sqlite3_bind_int(stmt.get(),4,static_cast<int>(s.status));
        sqlite3_bind_int64(stmt.get(),5,s.interval_seconds); bindText(stmt.get(),6,s.identity); bindText(stmt.get(),7,s.root);
        bindText(stmt.get(),8,s.mount_fingerprint); sqlite3_bind_int64(stmt.get(),9,s.last_check); sqlite3_bind_int64(stmt.get(),10,s.last_success);
        if(s.id) sqlite3_bind_int64(stmt.get(),11,s.id);
        if(sqlite3_step(stmt.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
        if(!s.id) s.id=sqlite3_last_insert_rowid(conn.get());
        if(!old.root.empty() && old.root!=s.root && !hasDirectoryStorage(conn.get())) {
            auto files=prepare(conn.get(),"SELECT id,relative_path FROM files WHERE source_id=?");
            sqlite3_bind_int64(files.get(),1,s.id);
            auto update=prepare(conn.get(),"UPDATE files SET path=?,parent_path=?,root=?,search_path=? WHERE id=?");
            while(sqlite3_step(files.get())==SQLITE_ROW) {
                const auto path=accessPath(s.root,columnText(files.get(),1));
                sqlite3_reset(update.get()); sqlite3_clear_bindings(update.get());
                bindText(update.get(),1,path); bindText(update.get(),2,normalizePath(pathToUtf8(utf8ToPath(path).parent_path())));
                bindText(update.get(),3,s.root); bindText(update.get(),4,asciiFold(path));
                sqlite3_bind_int64(update.get(),5,sqlite3_column_int64(files.get(),0));
                if(sqlite3_step(update.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
            }
        }
        if(!old.root.empty() && old.root!=s.root) purgeExcluded(conn,getPolicy(conn.get()));
        conn.exec("COMMIT;"); return s.id;
    } catch (...) { conn.exec("ROLLBACK;"); throw; }
}
std::uint64_t Database::forgetSource(std::int64_t id) const {
    SqliteConnection conn(db_path_); conn.exec("BEGIN IMMEDIATE;");
    try {
        const auto s=getSource(conn.get(),id);
        auto seen=prepare(conn.get(),"DELETE FROM scan_seen WHERE source_id=?"); sqlite3_bind_int64(seen.get(),1,id);
        if(sqlite3_step(seen.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
        auto files=prepare(conn.get(),hasDirectoryStorage(conn.get())?"DELETE FROM file_entries WHERE source_id=?":"DELETE FROM files WHERE source_id=?"); sqlite3_bind_int64(files.get(),1,id);
        if(sqlite3_step(files.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
        const auto removed=static_cast<std::uint64_t>(sqlite3_changes(conn.get()));
        if(hasDirectoryStorage(conn.get())) {
            auto dirs=prepare(conn.get(),"DELETE FROM directories WHERE source_id=?");sqlite3_bind_int64(dirs.get(),1,id);
            if(sqlite3_step(dirs.get())!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(conn.get()));
        }
        auto source_stmt=prepare(conn.get(),"DELETE FROM sources WHERE id=?"); sqlite3_bind_int64(source_stmt.get(),1,id);
        if(sqlite3_step(source_stmt.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
        auto roots=prepare(conn.get(),"DELETE FROM roots WHERE root=? AND NOT EXISTS(SELECT 1 FROM sources WHERE root=?)");
        bindText(roots.get(),1,s.root); bindText(roots.get(),2,s.root);
        if(sqlite3_step(roots.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
        conn.exec("COMMIT;"); return removed;
    } catch (...) { conn.exec("ROLLBACK;"); throw; }
}
SourceOwnershipSnapshot Database::captureSourceDescendants(const SourceRecord& expected) const {
    SqliteConnection conn(db_path_); conn.exec("BEGIN;");
    const auto current=getSource(conn.get(),expected.id);
    if(current.revision!=expected.revision || current.root!=expected.root || current.identity!=expected.identity || current.mount_fingerprint!=expected.mount_fingerprint)
        throw std::runtime_error("source binding changed before scan");
    const auto policy=getPolicy(conn.get());
    if(policy.revision!=expected.policy_revision)throw std::runtime_error("exclusion policy changed before scan");
    return {policy,sourceDescendants(conn.get(),expected)};
}
IndexStats Database::mergeSourceSnapshot(const SourceRecord& expected,
                                        const std::vector<FileRecord>& records,
                                        const std::string& subtree_relative,
                                        const std::vector<SourceRecord>* expected_descendants) const {
    std::vector<std::string> protected_scopes;
    if(expected_descendants)for(const auto& child:*expected_descendants)
        if(!ownsSuccessfulSnapshot(child))protected_scopes.push_back(relativeTo(child.root,expected.root));
    SqliteConnection conn(db_path_); std::size_t offset=0;
    return mergeSnapshot(conn,expected,[&](std::vector<FileRecord>& batch) {
        if(offset==records.size()) return false;
        const auto end=std::min(records.size(),offset+1000);
        batch.assign(records.begin()+offset,records.begin()+end); offset=end;
        if(expected_descendants)for(const auto& record:batch)
            for(const auto& child:*expected_descendants)
                if(pathIsWithin(record.path,child.root))throw std::invalid_argument("snapshot record belongs to nested source");
        return true;
    },subtree_relative,[&](sqlite3* main) {
        if(!expected_descendants)return;
        // This check must follow BEGIN IMMEDIATE and precede every files write.
        // Child file/label updates do not change ownership and stay admissible.
        if(!sameDescendantOwnership(*expected_descendants,sourceDescendants(main,expected)))
            throw std::runtime_error("nested source ownership changed during scan");
        const auto scope=subtree_relative.empty() || subtree_relative=="." ? expected.root :
            normalizePath(pathToUtf8(utf8ToPath(expected.root)/utf8ToPath(subtree_relative)));
        for(const auto& child:*expected_descendants)
            if(pathIsWithin(scope,child.root))throw std::invalid_argument("scan scope belongs to nested source");
    },nullptr,expected_descendants?&protected_scopes:nullptr);
}
IndexStats Database::mergeSourceSnapshotFile(const SourceRecord& expected,
                                            const std::string& snapshot_database,
                                            const std::string& subtree_relative) const {
    if(normalizePath(snapshot_database)==normalizePath(db_path_)) throw std::invalid_argument("snapshot must be a separate database");
    sqlite3* raw=nullptr;
    if(sqlite3_open_v2(snapshot_database.c_str(),&raw,SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK) {
        if(raw) sqlite3_close(raw);
        throw std::runtime_error("unable to open snapshot database");
    }
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> staging(raw,sqlite3_close);
    // Hold a consistent read snapshot even if an uncooperative producer writes.
    if(sqlite3_exec(raw,"BEGIN",nullptr,nullptr,nullptr)!=SQLITE_OK) throw std::runtime_error("unable to read snapshot");
    auto complete=prepare(raw,"SELECT complete FROM snapshot_metadata");
    if(sqlite3_step(complete.get())!=SQLITE_ROW || sqlite3_column_int(complete.get(),0)!=1 || sqlite3_step(complete.get())!=SQLITE_DONE)
        throw std::runtime_error("snapshot is incomplete");
    auto stmt=prepare(raw,"SELECT path,parent_path,name,ext,root,size,modified_time,is_dir,scan_generation,source_id,relative_path,cloud_copy FROM snapshot_files");
    SqliteConnection conn(db_path_); bool done=false;
    return mergeSnapshot(conn,expected,[&](std::vector<FileRecord>& batch) {
        if(done) return false;
        for(std::size_t i=0;i<1000;++i) {
            const auto rc=sqlite3_step(stmt.get());
            if(rc==SQLITE_DONE) { done=true; break; }
            if(rc!=SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(raw));
            batch.push_back(readFileRecord(stmt.get()));
        }
        return !batch.empty();
    },subtree_relative);
}

SourceScanRequest Database::captureSourceScanRequest(std::int64_t id,
    const std::string& scan_path,const std::string& snapshot_path,const std::string& nonce) const {
    if(id<=0)throw std::invalid_argument("invalid source id");
    SqliteConnection conn(db_path_);conn.exec("BEGIN;");
    auto stmt=prepare(conn.get(),std::string("SELECT ")+sourceColumns+" FROM sources WHERE id=?");sqlite3_bind_int64(stmt.get(),1,id);
    if(sqlite3_step(stmt.get())!=SQLITE_ROW)throw std::runtime_error("source not found");
    auto source=readSource(stmt.get());const auto policy=getPolicy(conn.get());source.policy_revision=policy.revision;
    SourceScanRequest request{source,policy,nestedSourceRoots(conn.get(),source),scan_path.empty()?source.root:scan_path,snapshot_path,nonce};
    validateSourceScanRequest(request);
    for(const auto& root:request.nested_source_roots)if(pathIsWithin(request.scan_path,root))throw std::invalid_argument("scan scope belongs to nested source");
    if(normalizePath(snapshot_path)==normalizePath(db_path_))throw std::invalid_argument("snapshot must be a separate database");
    return request;
}
IndexStats Database::mergeSourceScanSnapshot(const SourceScanRequest& captured) const {
    validateSourceScanRequest(captured);
    const auto& expected=captured.source;
    const auto canonical=capturedSourceCanonicalRoot(expected.mount_fingerprint);
    const auto relative=relativeTo(captured.scan_path,expected.root);const auto scope=relative=="."?std::string{}:relative;
    // Fixed captured topology is a strict, normalized, sorted set. The helper
    // prunes these roots regardless of their current accessibility status.
    const auto& nested=captured.nested_source_roots;
    for(std::size_t i=0;i<nested.size();++i) {
        if(nested[i]==expected.root || !pathIsWithin(nested[i],expected.root) || (i && nested[i-1]>=nested[i]))throw std::invalid_argument("invalid captured nested source set");
        if(pathIsWithin(captured.scan_path,nested[i]))throw std::invalid_argument("scan scope belongs to nested source");
    }
    if(normalizePath(captured.snapshot_path)==normalizePath(db_path_))throw std::invalid_argument("snapshot must be a separate database");
    sqlite3* raw=nullptr;
    if(sqlite3_open_v2(captured.snapshot_path.c_str(),&raw,SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK) {
        if(raw)sqlite3_close(raw);throw std::runtime_error("unable to open snapshot database");
    }
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> staging(raw,sqlite3_close);
    // Proof and all bounded record reads share this one pinned staging view.
    // Production callers still wait for the helper's actual exit and closed
    // writer; WAL concurrency consistency is not a hostile same-UID guarantee.
    if(sqlite3_exec(raw,"BEGIN",nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("unable to read snapshot");
    auto complete=prepare(raw,"SELECT complete FROM snapshot_metadata");
    if(sqlite3_step(complete.get())!=SQLITE_ROW || snapshotInteger(complete.get(),0)!=1 || sqlite3_step(complete.get())!=SQLITE_DONE)throw std::runtime_error("snapshot is incomplete");
    auto proof=prepare(raw,"SELECT nonce,source_id,source_revision,policy_revision,binding,identity,mount_fingerprint,actual_scope,canonical_root,boundary_fingerprint,subtree_relative FROM snapshot_proof");
    if(sqlite3_step(proof.get())!=SQLITE_ROW)throw std::runtime_error("snapshot proof missing");
    if(snapshotText(proof.get(),0)!=captured.nonce || snapshotInteger(proof.get(),1)!=expected.id || snapshotInteger(proof.get(),2)!=expected.revision || snapshotInteger(proof.get(),3)!=captured.policy.revision ||
       snapshotText(proof.get(),4)!=expected.root || snapshotText(proof.get(),5)!=expected.identity || snapshotText(proof.get(),6)!=expected.mount_fingerprint ||
       snapshotText(proof.get(),7)!=captured.scan_path || snapshotText(proof.get(),8)!=canonical || snapshotText(proof.get(),10)!=scope)throw std::runtime_error("snapshot proof does not match captured request");
    const auto child_roots=capturedChildMountRoots(snapshotText(proof.get(),9),canonical);
    if(sqlite3_step(proof.get())!=SQLITE_DONE)throw std::runtime_error("duplicate snapshot proof");
    if(snapshotRoots(raw,"SELECT root FROM snapshot_nested_roots ORDER BY root COLLATE BINARY",expected.root)!=nested)throw std::runtime_error("snapshot nested sources changed");
    if(snapshotRoots(raw,"SELECT root FROM snapshot_child_mounts ORDER BY root COLLATE BINARY",canonical)!=child_roots)throw std::runtime_error("snapshot child mount proof mismatch");
    auto rows=prepare(raw,"SELECT path,parent_path,name,ext,root,size,modified_time,is_dir,scan_generation,source_id,relative_path,cloud_copy FROM snapshot_files");
    SqliteConnection conn(db_path_);bool done=false;
    return mergeSnapshot(conn,expected,[&](std::vector<FileRecord>& batch) {
        if(done)return false;
        for(std::size_t i=0;i<1000;++i) {
            const int rc=sqlite3_step(rows.get());if(rc==SQLITE_DONE){done=true;break;}if(rc!=SQLITE_ROW)throw std::runtime_error(sqlite3_errmsg(raw));
            FileRecord record;record.path=snapshotText(rows.get(),0);record.parent_path=snapshotText(rows.get(),1);record.name=snapshotText(rows.get(),2);record.extension=snapshotText(rows.get(),3);record.root=snapshotText(rows.get(),4);
            const auto size=snapshotInteger(rows.get(),5);record.modified_time=snapshotInteger(rows.get(),6);const auto directory=snapshotInteger(rows.get(),7);record.scan_generation=snapshotInteger(rows.get(),8);record.source_id=snapshotInteger(rows.get(),9);record.relative_path=snapshotText(rows.get(),10);const auto cloud=snapshotInteger(rows.get(),11);
            if(size<0 || directory<0 || directory>1 || cloud<0 || cloud>2 || record.scan_generation!=0 || record.source_id!=expected.id)throw std::runtime_error("invalid snapshot record integer");
            if(!normalizedAbsolute(record.path) || !normalizedAbsolute(record.parent_path) || record.root!=expected.root || !pathIsWithin(record.path,captured.scan_path) || record.relative_path!=relativeTo(record.path,expected.root))throw std::runtime_error("snapshot record outside captured scope");
            const auto parsed=utf8ToPath(record.path);auto name=pathToUtf8(parsed.filename());if(name.empty())name=record.path;auto extension=pathToUtf8(parsed.extension());if(!extension.empty() && extension.front()=='.')extension.erase(0,1);
            if(record.parent_path!=normalizePath(pathToUtf8(parsed.parent_path())) || record.name!=name || record.extension!=extension)throw std::runtime_error("invalid snapshot record path fields");
            for(const auto& root:nested)if(pathIsWithin(record.path,root))throw std::runtime_error("snapshot record belongs to nested source");
            for(const auto& root:child_roots){const auto suffix=relativeTo(root,canonical);const auto binding=expected.root=="/"?"/"+suffix:expected.root+"/"+suffix;if(pathIsWithin(record.path,binding))throw std::runtime_error("snapshot record crosses child mount");}
            record.size=static_cast<std::uint64_t>(size);record.is_directory=directory==1;record.cloud_copy=static_cast<CloudCopy>(cloud);batch.push_back(std::move(record));
        }
        return !batch.empty();
    },scope,[&](sqlite3* main) {
        // Other sources can change this topology without advancing the owning
        // source revision. Recheck under the SAME main write transaction,
        // before any canonical files mutation; never open a second writer.
        if(nestedSourceRoots(main,expected)!=nested)throw std::runtime_error("nested source topology changed during scan");
    },&captured.policy);
}

void Database::beginRootScan(const std::string& root, std::int64_t generation) const {
    SqliteConnection conn(db_path_); conn.exec("BEGIN IMMEDIATE;");
    try {
        const auto id=sourceForRoot(conn.get(),root,true);
        advanceSourceRevision(conn.get(),id);
        const auto source=getSource(conn.get(),id);
        auto supersede=prepare(conn.get(),"UPDATE compatibility_scans SET state=2 WHERE source_id=? AND state=0");
        sqlite3_bind_int64(supersede.get(),1,id);
        if(sqlite3_step(supersede.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
        auto guard=prepare(conn.get(),"INSERT INTO compatibility_scans(root,generation,source_id,source_revision,policy_revision,identity,mount_fingerprint) VALUES(?,?,?,?,?,?,?)");
        bindText(guard.get(),1,source.root); sqlite3_bind_int64(guard.get(),2,generation); sqlite3_bind_int64(guard.get(),3,id);
        sqlite3_bind_int64(guard.get(),4,source.revision); sqlite3_bind_int64(guard.get(),5,source.policy_revision);
        bindText(guard.get(),6,source.identity); bindText(guard.get(),7,source.mount_fingerprint);
        if(sqlite3_step(guard.get())!=SQLITE_DONE) throw std::runtime_error("compatibility scan generation already used");
        auto remove=prepare(conn.get(),"DELETE FROM scan_seen WHERE source_id=?");
        sqlite3_bind_int64(remove.get(),1,id);
        if(sqlite3_step(remove.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
        auto stmt=prepare(conn.get(),"INSERT INTO roots(root,generation) VALUES(?,?) ON CONFLICT(root) DO UPDATE SET generation=excluded.generation");
        bindText(stmt.get(),1,normalizePath(root)); sqlite3_bind_int64(stmt.get(),2,generation);
        if(sqlite3_step(stmt.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
        conn.exec("COMMIT;");
    } catch (...) { conn.exec("ROLLBACK;"); throw; }
}
void Database::completeRootScan(const std::string& root, std::int64_t generation, std::uint64_t count) const {
    SqliteConnection conn(db_path_); conn.exec("BEGIN IMMEDIATE;");
    try {
        const auto id=guardedScanSource(conn.get(),root,generation,true);
        advanceSourceRevision(conn.get(),id);
        auto stmt=prepare(conn.get(),"UPDATE roots SET generation=?,last_scan_time=strftime('%s','now'),file_count=? WHERE root=?");
        sqlite3_bind_int64(stmt.get(),1,generation); sqlite3_bind_int64(stmt.get(),2,count); bindText(stmt.get(),3,normalizePath(root));
        if(sqlite3_step(stmt.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
        auto complete=prepare(conn.get(),"UPDATE compatibility_scans SET state=1 WHERE root=? AND generation=?");
        bindText(complete.get(),1,normalizePath(root)); sqlite3_bind_int64(complete.get(),2,generation);
        if(sqlite3_step(complete.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
        conn.exec("COMMIT;");
    } catch (...) { conn.exec("ROLLBACK;"); throw; }
}
std::int64_t Database::currentGeneration(const std::string& root) const {
    SqliteConnection conn(db_path_); sourceForRoot(conn.get(),root,false);
    auto stmt=prepare(conn.get(),"SELECT generation FROM roots WHERE root=?"); bindText(stmt.get(),1,normalizePath(root));
    return sqlite3_step(stmt.get())==SQLITE_ROW ? sqlite3_column_int64(stmt.get(),0) : 0;
}
void Database::upsertBatch(const std::vector<FileRecord>& records,const std::string& root,std::int64_t generation) const {
    SqliteConnection conn(db_path_); conn.exec("BEGIN IMMEDIATE;");
    try {
        const auto guarded=guardedScanSource(conn.get(),root,generation,false);
        const auto id=guarded ? guarded : sourceForRoot(conn.get(),root,true);
        const auto source=getSource(conn.get(),id);
        const auto policy=getPolicy(conn.get());
        const bool changed=upsertRecords(conn.get(),source,records,generation,policy);
        if(changed) {
            if(guarded) advanceScanRevision(conn.get(),id,root,generation);
            else advanceSourceRevision(conn.get(),id);
        }
        const bool compact=hasDirectoryStorage(conn.get());
        std::unique_ptr<DirectoryRecordWriter> compact_writer;
        if(compact)compact_writer=std::make_unique<DirectoryRecordWriter>(conn.get());
        auto seen=prepare(conn.get(),compact?"INSERT OR IGNORE INTO scan_seen(source_id,generation,entry_id) VALUES(?,?,?)":"INSERT OR IGNORE INTO scan_seen(source_id,generation,relative_path) VALUES(?,?,?)");
        for(const auto& record:records) {
            if(policy.excludes(record.path,record.is_directory)) continue;
            sqlite3_reset(seen.get()); sqlite3_clear_bindings(seen.get());
            sqlite3_bind_int64(seen.get(),1,id); sqlite3_bind_int64(seen.get(),2,generation);
            if(compact)sqlite3_bind_int64(seen.get(),3,compact_writer->entryId(id,relativeTo(record.path,source.root)));
            else bindText(seen.get(),3,relativeTo(record.path,source.root));
            if(sqlite3_step(seen.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
        }
        conn.exec("COMMIT;");
    } catch (...) { conn.exec("ROLLBACK;"); throw; }
}
std::uint64_t Database::deleteStaleForRoot(const std::string& root,std::int64_t generation) const {
    SqliteConnection conn(db_path_); conn.exec("BEGIN IMMEDIATE;");
    try {
        const auto id=guardedScanSource(conn.get(),root,generation,true);
        auto stmt=prepare(conn.get(),hasDirectoryStorage(conn.get())?
            "DELETE FROM file_entries WHERE source_id=? AND NOT EXISTS(SELECT 1 FROM scan_seen WHERE scan_seen.source_id=file_entries.source_id AND scan_seen.generation=? AND scan_seen.entry_id=file_entries.id)":
            "DELETE FROM files WHERE source_id=? AND NOT EXISTS(SELECT 1 FROM scan_seen WHERE scan_seen.source_id=files.source_id AND scan_seen.generation=? AND scan_seen.relative_path=files.relative_path)");
        sqlite3_bind_int64(stmt.get(),1,id); sqlite3_bind_int64(stmt.get(),2,generation);
        if(sqlite3_step(stmt.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
        const auto removed=static_cast<std::uint64_t>(sqlite3_changes(conn.get()));
        if(removed) advanceScanRevision(conn.get(),id,root,generation);
        conn.exec("COMMIT;"); return removed;
    } catch (...) { conn.exec("ROLLBACK;"); throw; }
}
std::uint64_t Database::deletePathAndDescendants(const std::string& path) const {
    SqliteConnection conn(db_path_); const auto normalized=normalizePath(path);
    conn.exec("BEGIN IMMEDIATE;");
    try {
        auto sources_stmt=prepare(conn.get(),"SELECT id,root FROM sources");
        std::int64_t id=0; std::string source_root;
        while(sqlite3_step(sources_stmt.get())==SQLITE_ROW) {
            const auto root=columnText(sources_stmt.get(),1);
            if(pathIsWithin(normalized,root)) {
                if(id) throw std::runtime_error("ambiguous path; use source id");
                id=sqlite3_column_int64(sources_stmt.get(),0); source_root=root;
            }
        }
        if(!id) { conn.exec("COMMIT;"); return 0; }
        const auto relative=relativeTo(normalized,source_root);
        std::uint64_t removed=0;
        const auto remove=[&](const std::string& sql,const std::vector<std::string>& bounds) {
            auto stmt=prepare(conn.get(),sql); sqlite3_bind_int64(stmt.get(),1,id);
            for(std::size_t i=0;i<bounds.size();++i) bindText(stmt.get(),static_cast<int>(i)+2,bounds[i]);
            if(sqlite3_step(stmt.get())!=SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(conn.get()));
            removed+=static_cast<std::uint64_t>(sqlite3_changes(conn.get()));
        };
        if(hasDirectoryStorage(conn.get())) {
            for(const auto& query:directoryScopeQueries("DELETE FROM file_entries WHERE ","",relative))remove(query.sql,query.bounds);
        } else if(relative==".") remove("DELETE FROM files WHERE source_id=?",{});
        else {
            // Use the same BINARY relative-path bounds as snapshot cleanup.
            // Source root '/' is represented by '.', without a double-slash
            // LIKE pattern that would leave its children behind.
            remove("DELETE FROM files WHERE source_id=? AND relative_path=?",{relative});
            remove("DELETE FROM files WHERE source_id=? AND relative_path>=? AND relative_path<?",{relative+"/",relative+"0"});
        }
        if(removed) advanceSourceRevision(conn.get(),id);
        conn.exec("COMMIT;"); return removed;
    } catch (...) { conn.exec("ROLLBACK;"); throw; }
}
std::uint64_t Database::deleteRoot(const std::string& root) const {
    SqliteConnection conn(db_path_); const auto id=sourceForRoot(conn.get(),root,false);
    return id ? forgetSource(id) : 0;
}
void Database::clear() const {
    SqliteConnection conn(db_path_);
    conn.exec(hasDirectoryStorage(conn.get())?
        "BEGIN IMMEDIATE; DELETE FROM file_entries; DELETE FROM directories; DELETE FROM scan_seen; DELETE FROM sources; DELETE FROM roots; UPDATE compatibility_scans SET state=2; COMMIT;":
        "BEGIN IMMEDIATE; DELETE FROM files; DELETE FROM scan_seen; DELETE FROM sources; DELETE FROM roots; UPDATE compatibility_scans SET state=2; COMMIT;");
}

std::vector<SearchResult> Database::search(const SearchQuery& query,
                                           std::size_t limit,
                                           std::size_t offset,
                                           const std::atomic_bool* cancel) const {
    SqliteConnection conn(db_path_);
    const bool fts_ready = tableExists(conn.get(), "files_name_fts") && metadataFlag(conn.get(), "name_fts_ready");
    return runSearch(conn.get(), db_path_, query, limit, offset, fts_ready, cancel, false);
}

std::vector<SearchResult> Database::search(const std::string& query,
                                           std::size_t limit,
                                           std::size_t offset,
                                           const std::atomic_bool* cancel) const {
    return search(parseSearchQuery(query), limit, offset, cancel);
}

std::vector<SearchResult> Database::searchReliable(const SearchQuery& query,
                                                   std::size_t limit,
                                                   std::size_t offset) const {
    SqliteConnection conn(db_path_);
    // Deliberately bypass the optional FTS index. The canonical `files` table
    // is the source of truth. v0.4.5 uses SQLite instr() for exact substring
    // matching instead of LIKE so correctness does not depend on LIKE
    // wildcard/collation behaviour. No progress handler is installed here.
    return runSearch(conn.get(), db_path_, query, limit, offset, false, nullptr, true);
}

std::vector<SearchResult> Database::searchReliable(const std::string& query,
                                                   std::size_t limit,
                                                   std::size_t offset) const {
    return searchReliable(parseSearchQuery(query), limit, offset);
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
    auto stmt = prepare(conn.get(), "SELECT DISTINCT root FROM sources ORDER BY root;");
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

std::uint64_t Database::nameSearchIndexCount() const {
    SqliteConnection conn(db_path_);
    if (!tableExists(conn.get(), "files_name_fts")) return 0;
    auto stmt = prepare(conn.get(), "SELECT COUNT(*) FROM files_name_fts;");
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        return static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 0));
    }
    return 0;
}

void Database::verifyNameSearchIndex() const {
    SqliteConnection conn(db_path_);
    if (!tableExists(conn.get(), "files_name_fts")) {
        throw std::runtime_error("FTS5 trigram index is unavailable");
    }
    // FTS5 external-content integrity-check with rank=1 compares the actual
    // full-text index against the files content table, not merely row counts.
    conn.exec("INSERT INTO files_name_fts(files_name_fts, rank) VALUES('integrity-check', 1);");
}

void Database::rebuildNameSearchIndex() const {
    SqliteConnection conn(db_path_);
    if (!tableExists(conn.get(), "files_name_fts")) return;

    if(hasDirectoryStorage(conn.get())) {
        conn.exec("BEGIN IMMEDIATE;");
        try {
            conn.exec("INSERT INTO files_name_fts(files_name_fts) VALUES('rebuild');");
            conn.exec("INSERT INTO files_name_fts(files_name_fts,rank) VALUES('integrity-check',1);");
            installFtsTriggers(conn);setMetadataFlag(conn.get(),"name_fts_ready",true);
            conn.exec("COMMIT;");return;
        }catch(...) {conn.exec("ROLLBACK;");throw;}
    }
    // External-content FTS tables require one initial rebuild for databases
    // created by v0.2 and earlier. This is a one-time O(N) migration.
    conn.exec("INSERT INTO files_name_fts(files_name_fts) VALUES('rebuild');");
    installFtsTriggers(conn);
    setMetadataFlag(conn.get(), "name_fts_ready", true);
}

} // namespace everything_lite
