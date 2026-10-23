#include "core/directory_storage.h"
#include "core/path_utils.h"
#include <sqlite3.h>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <map>

namespace everything_lite {
namespace {
using Statement=std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)>;
void exec(sqlite3* db,const std::string& sql) {
    char* error=nullptr;const int rc=sqlite3_exec(db,sql.c_str(),nullptr,nullptr,&error);
    const std::string message=error?error:"directory storage SQL failed";sqlite3_free(error);
    if(rc!=SQLITE_OK)throw std::runtime_error(message);
}
Statement prepare(sqlite3* db,const std::string& sql) {
    sqlite3_stmt* raw=nullptr;const int rc=sqlite3_prepare_v2(db,sql.c_str(),-1,&raw,nullptr);
    Statement out(raw,sqlite3_finalize);if(rc!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db));return out;
}
void text(sqlite3_stmt* stmt,int i,const std::string& value) {
    if(sqlite3_bind_text(stmt,i,value.data(),static_cast<int>(value.size()),SQLITE_TRANSIENT)!=SQLITE_OK)throw std::runtime_error("directory storage bind failed");
}
std::string column(sqlite3_stmt* stmt,int i) {
    const auto* value=sqlite3_column_text(stmt,i);return value?reinterpret_cast<const char*>(value):"";
}
bool object(sqlite3* db,const std::string& name,const std::string& type="") {
    auto q=prepare(db,"SELECT 1 FROM sqlite_master WHERE name=? AND (?='' OR type=?)");
    text(q.get(),1,name);text(q.get(),2,type);text(q.get(),3,type);return sqlite3_step(q.get())==SQLITE_ROW;
}
std::pair<std::string,std::string> split(const std::string& relative) {
    if(relative==".")return {"",""};
    const auto pos=relative.find_last_of('/');
    return pos==std::string::npos?std::make_pair(std::string{},relative):std::make_pair(relative.substr(0,pos),relative.substr(pos+1));
}
constexpr const char* columns="id,source_id,relative_path,path,parent_path,name,ext,root,size,modified_time,is_dir,search_name,search_path,scan_generation,cloud_copy";
}
void registerDirectoryWriter(sqlite3* db) {
    const int rc=sqlite3_create_function_v2(db,"everything_lite_directory_writer_v1",0,SQLITE_UTF8,nullptr,
        [](sqlite3_context* context,int,sqlite3_value**){sqlite3_result_int(context,1);},nullptr,nullptr,nullptr);
    if(rc!=SQLITE_OK)throw std::runtime_error("unable to register directory storage writer");
}
bool hasDirectoryStorage(sqlite3* db) {
    if(object(db,"file_entries") || object(db,"directories") || object(db,"files","view"))return true;
    auto version=prepare(db,"PRAGMA user_version");
    if(sqlite3_step(version.get())==SQLITE_ROW && sqlite3_column_int(version.get(),0)==414)return true;
    if(!object(db,"app_metadata","table"))return false;
    auto flag=prepare(db,"SELECT 1 FROM app_metadata WHERE key='storage_layout' AND value='directory-v1'");
    return sqlite3_step(flag.get())==SQLITE_ROW;
}
bool directoryStorageReady(sqlite3* db) {
    try {
        auto version=prepare(db,"PRAGMA user_version");
        if(sqlite3_step(version.get())!=SQLITE_ROW || sqlite3_column_int(version.get(),0)!=414)return false;
        if(!object(db,"files","view") || !object(db,"file_entries","table") || !object(db,"directories","table"))return false;
        auto metadata=prepare(db,"SELECT count(*) FROM app_metadata WHERE (key='storage_layout' AND value='directory-v1') OR (key='directory_schema_ready' AND value='1')");
        if(sqlite3_step(metadata.get())!=SQLITE_ROW || sqlite3_column_int(metadata.get(),0)!=2)return false;
        prepare(db,std::string("SELECT ")+columns+" FROM files LIMIT 0");
        prepare(db,"SELECT id,source_id,directory_id,leaf,name,ext,size,modified_time,is_dir,search_name,scan_generation,cloud_copy FROM file_entries LIMIT 0");
        prepare(db,"SELECT id,source_id,relative_path FROM directories LIMIT 0");
        prepare(db,"SELECT source_id,generation,entry_id FROM scan_seen LIMIT 0");
        prepare(db,"SELECT root,generation,source_id,source_revision,policy_revision,identity,mount_fingerprint,state FROM compatibility_scans LIMIT 0");
        prepare(db,"SELECT id,label,type,mode,status,interval_seconds,identity,root,mount_fingerprint,last_check,last_success,revision FROM sources LIMIT 0");
        prepare(db,"SELECT root,generation,last_scan_time,file_count FROM roots LIMIT 0");
        prepare(db,"SELECT kind,position,path FROM exclusion_paths LIMIT 0");
        auto policy=prepare(db,"SELECT revision,folder_filter,file_filter FROM exclusion_policy WHERE id=1");
        if(sqlite3_step(policy.get())!=SQLITE_ROW)return false;
        for(const auto* name:{"idx_sources_root","idx_entries_search_name","idx_entries_ext","idx_entries_size","idx_entries_source_generation","idx_entries_modified"})if(!object(db,name,"index"))return false;
        for(const auto* table:{"sources","roots","scan_seen","compatibility_scans","exclusion_policy","exclusion_paths","app_metadata","directories","file_entries"})
            for(const auto* op:{"insert","update","delete"})if(!object(db,std::string("directory_guard_")+table+"_"+op,"trigger"))return false;
        auto fts=prepare(db,"SELECT sql FROM sqlite_master WHERE type='table' AND name='files_name_fts'");
        if(sqlite3_step(fts.get())!=SQLITE_ROW)return false;
        const auto fts_sql=column(fts.get(),0);
        if(fts_sql.find("detail=none")==std::string::npos || fts_sql.find("columnsize=0")==std::string::npos ||
           fts_sql.find("content='files'")==std::string::npos || fts_sql.find("tokenize='trigram'")==std::string::npos)return false;
        auto ready=prepare(db,"SELECT value FROM app_metadata WHERE key='name_fts_ready'");
        if(sqlite3_step(ready.get())!=SQLITE_ROW)return false;
        const auto flag=column(ready.get(),0);if(flag!="0" && flag!="1")return false;
        for(const auto* name:{"files_name_fts_ai","files_name_fts_ad","files_name_fts_au"})
            if(object(db,name,"trigger")!=(flag=="1"))return false;
        return true;
    }catch(const std::exception&){return false;}
}
void installDirectoryFtsTriggers(sqlite3* db) {
    exec(db,R"SQL(
CREATE TRIGGER IF NOT EXISTS files_name_fts_ai AFTER INSERT ON file_entries BEGIN
 INSERT INTO files_name_fts(rowid,search_name) VALUES(new.id,new.search_name); END;
CREATE TRIGGER IF NOT EXISTS files_name_fts_ad AFTER DELETE ON file_entries BEGIN
 INSERT INTO files_name_fts(files_name_fts,rowid,search_name) VALUES('delete',old.id,old.search_name); END;
CREATE TRIGGER IF NOT EXISTS files_name_fts_au AFTER UPDATE OF search_name ON file_entries WHEN old.search_name<>new.search_name BEGIN
 INSERT INTO files_name_fts(files_name_fts,rowid,search_name) VALUES('delete',old.id,old.search_name);
 INSERT INTO files_name_fts(rowid,search_name) VALUES(new.id,new.search_name); END;
)SQL");
}
std::int64_t directoryEntryId(sqlite3* db,std::int64_t source,const std::string& relative) {
    const auto parts=split(relative);
    auto q=prepare(db,"SELECT e.id FROM directories d JOIN file_entries e ON e.directory_id=d.id WHERE d.source_id=? AND d.relative_path=? AND e.leaf=?");
    sqlite3_bind_int64(q.get(),1,source);text(q.get(),2,parts.first);text(q.get(),3,parts.second);
    const int rc=sqlite3_step(q.get());if(rc==SQLITE_DONE)return 0;if(rc!=SQLITE_ROW)throw std::runtime_error(sqlite3_errmsg(db));return sqlite3_column_int64(q.get(),0);
}
struct DirectoryRecordWriter::Impl {
    sqlite3* db;
    Statement insert_dir,find_dir,upsert,find_entry;
    std::map<std::pair<std::int64_t,std::string>,std::int64_t> directories;
    explicit Impl(sqlite3* handle):db(handle),
      insert_dir(prepare(db,"INSERT INTO directories(source_id,relative_path) VALUES(?,?) ON CONFLICT(source_id,relative_path) DO NOTHING")),
      find_dir(prepare(db,"SELECT id FROM directories WHERE source_id=? AND relative_path=?")),
      upsert(prepare(db,R"SQL(INSERT INTO file_entries(source_id,directory_id,leaf,name,ext,size,modified_time,is_dir,search_name,scan_generation,cloud_copy)
VALUES(?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(directory_id,leaf) DO UPDATE SET name=excluded.name,ext=excluded.ext,size=excluded.size,
modified_time=excluded.modified_time,is_dir=excluded.is_dir,search_name=excluded.search_name,scan_generation=excluded.scan_generation,cloud_copy=excluded.cloud_copy
WHERE file_entries.name<>excluded.name OR file_entries.ext<>excluded.ext OR file_entries.size<>excluded.size
OR file_entries.modified_time<>excluded.modified_time OR file_entries.is_dir<>excluded.is_dir OR file_entries.cloud_copy<>excluded.cloud_copy)SQL")),
      find_entry(prepare(db,"SELECT e.id FROM directories d JOIN file_entries e ON e.directory_id=d.id WHERE d.source_id=? AND d.relative_path=? AND e.leaf=?")) {}
};
DirectoryRecordWriter::DirectoryRecordWriter(sqlite3* db):impl_(std::make_unique<Impl>(db)) {}
DirectoryRecordWriter::~DirectoryRecordWriter()=default;
std::int64_t DirectoryRecordWriter::directoryId(std::int64_t source,const std::string& path) {
    const auto key=std::make_pair(source,path);const auto found=impl_->directories.find(key);
    if(found!=impl_->directories.end())return found->second;
    auto& insert=impl_->insert_dir;sqlite3_reset(insert.get());sqlite3_clear_bindings(insert.get());
    sqlite3_bind_int64(insert.get(),1,source);text(insert.get(),2,path);
    if(sqlite3_step(insert.get())!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(impl_->db));
    auto& find=impl_->find_dir;sqlite3_reset(find.get());sqlite3_clear_bindings(find.get());
    sqlite3_bind_int64(find.get(),1,source);text(find.get(),2,path);
    if(sqlite3_step(find.get())!=SQLITE_ROW)throw std::runtime_error("directory storage missing directory");
    const auto id=sqlite3_column_int64(find.get(),0);
    sqlite3_reset(find.get());
    if(impl_->directories.size()>=8192)impl_->directories.clear();
    impl_->directories.emplace(key,id);return id;
}
std::int64_t DirectoryRecordWriter::entryId(std::int64_t source,const std::string& relative) {
    const auto parts=split(relative);auto& find=impl_->find_entry;sqlite3_reset(find.get());sqlite3_clear_bindings(find.get());
    sqlite3_bind_int64(find.get(),1,source);text(find.get(),2,parts.first);text(find.get(),3,parts.second);
    const int rc=sqlite3_step(find.get());if(rc==SQLITE_DONE)return 0;
    if(rc!=SQLITE_ROW)throw std::runtime_error(sqlite3_errmsg(impl_->db));
    const auto id=sqlite3_column_int64(find.get(),0);sqlite3_reset(find.get());return id;
}
bool DirectoryRecordWriter::upsert(std::int64_t source,const FileRecord& r,const std::string& relative,std::int64_t generation) {
    if(r.parent_path!=normalizePath(pathToUtf8(utf8ToPath(normalizePath(r.path)).parent_path())))throw std::invalid_argument("directory storage requires canonical parent_path");
    const auto parts=split(relative);const auto dir=directoryId(source,parts.first);
    auto& stmt=impl_->upsert;
    sqlite3_reset(stmt.get());sqlite3_clear_bindings(stmt.get());
    sqlite3_bind_int64(stmt.get(),1,source);sqlite3_bind_int64(stmt.get(),2,dir);text(stmt.get(),3,parts.second);text(stmt.get(),4,r.name);text(stmt.get(),5,r.extension);
    sqlite3_bind_int64(stmt.get(),6,static_cast<sqlite3_int64>(r.size));sqlite3_bind_int64(stmt.get(),7,r.modified_time);sqlite3_bind_int(stmt.get(),8,r.is_directory);
    text(stmt.get(),9,asciiFold(r.name));sqlite3_bind_int64(stmt.get(),10,generation);sqlite3_bind_int(stmt.get(),11,storedCloudFlags(r));
    if(sqlite3_step(stmt.get())!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(impl_->db));return sqlite3_changes(impl_->db)>0;
}
std::vector<DirectoryScopeQuery> directoryScopeQueries(const std::string& prefix,const std::string& suffix,const std::string& scope) {
    if(scope.empty() || scope==".")return {{prefix+"source_id=?"+suffix,{}}};
    const auto parts=split(scope);
    return {{prefix+"directory_id IN (SELECT id FROM directories WHERE source_id=? AND relative_path=?) AND leaf=?"+suffix,{parts.first,parts.second}},
            {prefix+"directory_id IN (SELECT id FROM directories WHERE source_id=? AND relative_path=?)"+suffix,{scope}},
            {prefix+"directory_id IN (SELECT id FROM directories WHERE source_id=? AND relative_path>=? AND relative_path<?)"+suffix,{scope+"/",scope+"0"}}};
}
void migrateDirectorySchema(sqlite3* db) {
    registerDirectoryWriter(db);
    auto version=prepare(db,"PRAGMA user_version");
    if(sqlite3_step(version.get())!=SQLITE_ROW || sqlite3_column_int(version.get(),0)!=413 || !object(db,"files","table"))
        throw std::runtime_error("storage: optimize requires initialized schema 413; first open and upgrade with a source-catalog capable version, then retry optimize");
    version.reset();
    exec(db,R"SQL(
DROP TRIGGER IF EXISTS files_name_fts_ai; DROP TRIGGER IF EXISTS files_name_fts_ad; DROP TRIGGER IF EXISTS files_name_fts_au;
DROP TABLE IF EXISTS files_name_fts;
ALTER TABLE files RENAME TO files_directory_legacy;
CREATE TABLE directories(id INTEGER PRIMARY KEY,source_id INTEGER NOT NULL REFERENCES sources(id),relative_path TEXT NOT NULL COLLATE BINARY,UNIQUE(source_id,relative_path));
CREATE TABLE file_entries(id INTEGER PRIMARY KEY,source_id INTEGER NOT NULL REFERENCES sources(id),directory_id INTEGER NOT NULL REFERENCES directories(id),leaf TEXT NOT NULL COLLATE BINARY,
 name TEXT NOT NULL,ext TEXT NOT NULL DEFAULT '',size INTEGER NOT NULL DEFAULT 0,modified_time INTEGER NOT NULL DEFAULT 0,is_dir INTEGER NOT NULL DEFAULT 0,
 search_name TEXT NOT NULL,scan_generation INTEGER NOT NULL DEFAULT 0,cloud_copy INTEGER NOT NULL DEFAULT 2,UNIQUE(directory_id,leaf));
CREATE INDEX idx_entries_search_name ON file_entries(search_name);
CREATE INDEX idx_entries_ext ON file_entries(ext COLLATE NOCASE);
CREATE INDEX idx_entries_size ON file_entries(size);
CREATE INDEX idx_entries_source_generation ON file_entries(source_id,scan_generation);
CREATE INDEX idx_entries_modified ON file_entries(modified_time DESC);
CREATE VIEW files AS SELECT e.id,e.source_id,
 CASE WHEN e.leaf='' THEN '.' WHEN d.relative_path='' THEN e.leaf ELSE d.relative_path||'/'||e.leaf END AS relative_path,
 CASE WHEN e.leaf='' THEN s.root ELSE rtrim(s.root,'/')||'/'||CASE WHEN d.relative_path='' THEN e.leaf ELSE d.relative_path||'/'||e.leaf END END AS path,
 CASE WHEN e.leaf='' THEN coalesce(nullif(rtrim(rtrim(s.root,replace(s.root,'/','')),'/'),''),'/')
 WHEN d.relative_path='' THEN s.root ELSE rtrim(s.root,'/')||'/'||d.relative_path END AS parent_path,
 e.name,e.ext,s.root AS root,e.size,e.modified_time,e.is_dir,e.search_name,
 lower(CASE WHEN e.leaf='' THEN s.root ELSE rtrim(s.root,'/')||'/'||CASE WHEN d.relative_path='' THEN e.leaf ELSE d.relative_path||'/'||e.leaf END END) AS search_path,
 e.scan_generation,e.cloud_copy FROM file_entries e JOIN directories d ON d.id=e.directory_id JOIN sources s ON s.id=e.source_id;
)SQL");
    {
        DirectoryRecordWriter writer(db);
        auto rows=prepare(db,"SELECT id,source_id,relative_path,name,ext,size,modified_time,is_dir,search_name,scan_generation,cloud_copy FROM files_directory_legacy ORDER BY id");
        auto insert=prepare(db,"INSERT INTO file_entries(id,source_id,directory_id,leaf,name,ext,size,modified_time,is_dir,search_name,scan_generation,cloud_copy) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
        int rc;
        while((rc=sqlite3_step(rows.get()))==SQLITE_ROW) {
            const auto relative=column(rows.get(),2);const auto parts=split(relative);
            if(relative.empty() || relative.front()=='/' || relative.find('\0')!=std::string::npos ||
               (relative!="." && (parts.second.empty() || pathToUtf8(utf8ToPath(relative).lexically_normal())!=relative)))throw std::runtime_error("storage: legacy relative path cannot be represented safely");
            for(const auto& part:utf8ToPath(relative))if(part=="..")throw std::runtime_error("storage: legacy relative path escapes source");
            const auto dir=writer.directoryId(sqlite3_column_int64(rows.get(),1),parts.first);
            sqlite3_reset(insert.get());sqlite3_clear_bindings(insert.get());
            sqlite3_bind_int64(insert.get(),1,sqlite3_column_int64(rows.get(),0));sqlite3_bind_int64(insert.get(),2,sqlite3_column_int64(rows.get(),1));sqlite3_bind_int64(insert.get(),3,dir);text(insert.get(),4,parts.second);
            for(int i=3;i<=10;++i) {
                if(sqlite3_column_type(rows.get(),i)==SQLITE_TEXT)text(insert.get(),i+2,column(rows.get(),i));else sqlite3_bind_int64(insert.get(),i+2,sqlite3_column_int64(rows.get(),i));
            }
            if(sqlite3_step(insert.get())!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db));
        }
        if(rc!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db));
    }
    for(const bool reverse:{false,true}) {
        const auto a=reverse?"files":"files_directory_legacy",b=reverse?"files_directory_legacy":"files";
        auto difference=prepare(db,std::string("SELECT 1 FROM (SELECT ")+columns+" FROM "+a+" EXCEPT SELECT "+columns+" FROM "+b+") LIMIT 1");
        const int rc=sqlite3_step(difference.get());
        if(rc!=SQLITE_DONE)throw std::runtime_error("storage: derived fields differ from legacy values; migration rolled back, keep recovery backup");
    }
    exec(db,R"SQL(
DROP TABLE files_directory_legacy;
DROP TABLE scan_seen;
CREATE TABLE scan_seen(source_id INTEGER NOT NULL,generation INTEGER NOT NULL,entry_id INTEGER NOT NULL,PRIMARY KEY(source_id,generation,entry_id));
UPDATE compatibility_scans SET state=2;
UPDATE sources SET revision=revision+1;
DELETE FROM app_metadata WHERE key IN ('schema_413_ready_v1','schema_413_fts_available');
INSERT OR REPLACE INTO app_metadata VALUES('storage_layout','directory-v1'),('directory_schema_ready','1'),('name_fts_ready','0');
CREATE VIRTUAL TABLE files_name_fts USING fts5(search_name,content='files',content_rowid='id',tokenize='trigram',detail=none,columnsize=0);
INSERT INTO files_name_fts(files_name_fts) VALUES('rebuild');
INSERT INTO files_name_fts(files_name_fts,rank) VALUES('integrity-check',1);
UPDATE app_metadata SET value='1' WHERE key='name_fts_ready';
PRAGMA user_version=414;
)SQL");
    installDirectoryFtsTriggers(db);
    for(const auto* table:{"sources","roots","scan_seen","compatibility_scans","exclusion_policy","exclusion_paths","app_metadata","directories","file_entries"})
        for(const auto* op:{"insert","update","delete"})exec(db,std::string("CREATE TRIGGER directory_guard_")+table+"_"+op+" BEFORE "+op+" ON "+table+" BEGIN SELECT everything_lite_directory_writer_v1(); END;");
    if(!directoryStorageReady(db))throw std::runtime_error("storage: compact schema verification failed");
}
} // namespace everything_lite
