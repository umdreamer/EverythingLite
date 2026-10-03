#include "core/index_manager.h"
#include "core/source_catalog.h"
#include "core/path_utils.h"
#include <thread>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include <sqlite3.h>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace everything_lite;
namespace fs = std::filesystem;
static void sql(const fs::path& db, const char* text) {
    sqlite3* handle=nullptr; assert(sqlite3_open(db.string().c_str(), &handle)==SQLITE_OK);
    char* error=nullptr; const auto rc=sqlite3_exec(handle,text,nullptr,nullptr,&error);
    if (error) { std::cerr<<error<<'\n'; sqlite3_free(error); }
    sqlite3_close(handle); assert(rc==SQLITE_OK);
}
static std::int64_t scalar(const fs::path& db, const char* text) {
    sqlite3* handle=nullptr; assert(sqlite3_open(db.string().c_str(), &handle)==SQLITE_OK);
    sqlite3_stmt* stmt=nullptr; assert(sqlite3_prepare_v2(handle,text,-1,&stmt,nullptr)==SQLITE_OK);
    assert(sqlite3_step(stmt)==SQLITE_ROW); const auto result=sqlite3_column_int64(stmt,0);
    sqlite3_finalize(stmt); sqlite3_close(handle); return result;
}
static bool rejected(const std::function<void()>& work) { try { work(); } catch (const std::exception&) { return true; } return false; }
int main() {
    assert(pathIsWithin("/synthetic/file.txt","/"));
    const auto nonce=std::chrono::steady_clock::now().time_since_epoch().count();
    auto base=fs::temp_directory_path()/("everything-lite-source-test-"+std::to_string(nonce));
    fs::create_directories(base/"root");
    std::ofstream(base/"root"/"retained.txt") << "retained";
    IndexManager manager((base/"index.db").string());
    auto& db=manager.database();
    manager.rebuildRoot((base/"root").string());
    const auto before=db.totalFileCount(); assert(before==2);
    fs::rename(base/"root",base/"offline");
    auto offline=manager.rebuildRoot((base/"root").string());
    assert(!offline.completed); assert(db.totalFileCount()==before);
    assert(db.sources().front().status==SourceStatus::Offline);
    manager.rebuildRoots({}); assert(db.totalFileCount()==before);
    fs::rename(base/"offline",base/"root");
    manager.rebuildRoot((base/"root").string());
    sql(base/"index.db", "CREATE TABLE audit(writes INTEGER); INSERT INTO audit VALUES(0); CREATE TRIGGER audit_update AFTER UPDATE ON files BEGIN UPDATE audit SET writes=writes+1; END; CREATE TRIGGER audit_insert AFTER INSERT ON files BEGIN UPDATE audit SET writes=writes+1; END; CREATE TRIGGER audit_delete AFTER DELETE ON files BEGIN UPDATE audit SET writes=writes+1; END;");
    manager.rebuildRoot((base/"root").string());
    assert(scalar(base/"index.db","SELECT writes FROM audit")==0);
    std::atomic_bool cancel{true};
    auto cancelled=manager.rebuildRoot((base/"root").string(),{},&cancel);
    assert(cancelled.cancelled && !cancelled.completed); assert(db.totalFileCount()==before);
    FileScanner scanner(1);
    auto refused=scanner.scanTree((base/"root").string(),[](std::vector<FileRecord>&&) { return false; });
    assert(refused.cancelled && !refused.completed);
    auto callback_error=scanner.scanTree((base/"root").string(),[](std::vector<FileRecord>&&)->bool { throw std::runtime_error("callback failure"); });
    assert(!callback_error.completed && !callback_error.cancelled && !callback_error.error.empty());
    // Partial batches never reach the canonical tables when cancellation occurs.
    std::ofstream(base/"root"/"partial.txt")<<"partial";
    cancel=false;
    auto partial=manager.rebuildRoot((base/"root").string(),[&](const std::string&,std::uint64_t){cancel=true;}, &cancel);
    assert(!partial.completed); assert(db.search("partial").empty());
    fs::remove(base/"root"/"partial.txt");
    fs::remove(base/"root"/"retained.txt");
    manager.rebuildRoot((base/"root").string()); assert(db.totalFileCount()==1);

    // Two independent logical sources may bind the same access path.
    auto first=db.sources().front(); first.label="first"; first.identity="synthetic:first"; first.mount_fingerprint="binding-a"; db.saveSource(first);
    SourceRecord other; other.label="second"; other.root=first.root; other.type=SourceType::NAS; other.identity="synthetic:second"; other.mount_fingerprint="binding-b";
    other.id=db.saveSource(other);
    assert(other.id!=first.id); assert(rejected([&]{manager.rebuildRoot(first.root);}));
    std::ofstream(base/"root"/"collision.txt")<<"second";
    manager.rebuildSource(other.id);
    assert(db.search("collision").size()==1);
    manager.rebuildSource(first.id);
    assert(db.search("collision").size()==2);
    SearchQuery selected=parseSearchQuery("collision"); selected.source_id=other.id;
    const auto page=db.search(selected,1); assert(page.size()==1 && page[0].file.source_id==other.id);
    assert(page[0].source_label=="second" && page[0].source_type==SourceType::NAS);
    auto second=db.source(other.id); second.status=SourceStatus::Offline; db.saveSource(second);
    selected.source_status=SourceStatus::Offline; assert(db.search(selected,1).size()==1);
    selected.source_status=SourceStatus::Accessible; assert(db.search(selected,1).empty());
    assert(db.searchReliable(selected,1).empty());
    assert(rejected([&]{db.deletePathAndDescendants((base/"root"/"collision.txt").string());}));
    assert(rejected([&]{db.deleteRoot(first.root);}));
    const auto guard=db.source(first.id); auto changed=guard; changed.mount_fingerprint="binding-new"; db.saveSource(changed);
    assert(rejected([&]{db.mergeSourceSnapshot(guard,{});}));
    // Rebinding updates access fields for one source only and keeps logical identity.
    auto rebind=db.source(first.id); rebind.root=(base/"rebound").string(); db.saveSource(rebind);
    selected.source_status.reset(); selected.source_id=first.id;
    assert(db.search(selected).front().file.path==(base/"rebound"/"collision.txt").string());
    selected.source_id=other.id; assert(db.search(selected).front().file.path==(base/"root"/"collision.txt").string());
    db.forgetSource(first.id); assert(db.search("collision").size()==1);
    assert(parseSearchQuery("source:"+std::to_string(other.id)+" status:offline collision").source_id==other.id);
    if(db.nameSearchIndexAvailable()){db.verifyNameSearchIndex();}

    // Legacy migration backs up first and assigns Unknown, preserving rows/FTS.
    const auto legacy=base/"legacy.db";
    sql(legacy,R"SQL(CREATE TABLE files(id INTEGER PRIMARY KEY,path TEXT NOT NULL UNIQUE,parent_path TEXT NOT NULL,name TEXT NOT NULL,ext TEXT NOT NULL DEFAULT '',root TEXT NOT NULL,size INTEGER NOT NULL DEFAULT 0,modified_time INTEGER NOT NULL DEFAULT 0,is_dir INTEGER NOT NULL DEFAULT 0,search_name TEXT NOT NULL,search_path TEXT NOT NULL,scan_generation INTEGER NOT NULL DEFAULT 0); CREATE TABLE roots(root TEXT PRIMARY KEY,generation INTEGER NOT NULL DEFAULT 0,last_scan_time INTEGER NOT NULL DEFAULT 0,file_count INTEGER NOT NULL DEFAULT 0); INSERT INTO files VALUES(17,'/synthetic/mounted/legacy.txt','/synthetic/mounted','legacy.txt','txt','/synthetic/mounted',7,1,0,'legacy.txt','/synthetic/mounted/legacy.txt',1); INSERT INTO roots VALUES('/synthetic/mounted',1,1,1);)SQL");
    if(db.nameSearchIndexAvailable()) {
        sql(legacy,"CREATE VIRTUAL TABLE files_name_fts USING fts5(search_name,content='files',content_rowid='id',tokenize='trigram'); INSERT INTO files_name_fts(files_name_fts) VALUES('rebuild'); CREATE TABLE app_metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL); INSERT INTO app_metadata VALUES('name_fts_ready','1');");
    }
    Database migrated(legacy.string()); migrated.initialize();
    assert(migrated.totalFileCount()==1 && migrated.sources().size()==1);
    assert(migrated.sources()[0].type==SourceType::Unknown);
    assert(migrated.search("legacy")[0].file.relative_path=="legacy.txt");
    assert(!migrated.migrationBackupPath().empty() && fs::exists(migrated.migrationBackupPath()));
    assert(scalar(migrated.migrationBackupPath(),"SELECT COUNT(*) FROM files")==1);
    if(migrated.nameSearchIndexAvailable()){ assert(migrated.nameSearchIndexReady()); migrated.verifyNameSearchIndex(); }
    migrated.initialize(); assert(migrated.totalFileCount()==1);
    // Incremental events scan only the affected subtree; no whole-root regression.
    const auto event_root=base/"events";
    fs::create_directories(event_root/"nested");
    std::ofstream(event_root/"nested"/"target.txt")<<"target";
    std::ofstream(event_root/"unrelated.txt")<<"unchanged";
    IndexManager events((base/"events.db").string()); events.rebuildRoot(event_root.string());
    auto single_event=events.applyPathChange(event_root.string(),(event_root/"unrelated.txt").string());
    assert(single_event.completed && single_event.scanned==1);
    fs::remove(event_root/"nested"/"target.txt");
    auto removed_event=events.applyPathChange(event_root.string(),(event_root/"nested"/"target.txt").string());
    assert(removed_event.completed && removed_event.scanned==1 && removed_event.removed==1);
    assert(events.database().search("unrelated").size()==1);
    // The streaming snapshot API rejects unpublished/partial snapshots.
    const auto staging=base/"snapshot.db";
    sql(staging,R"SQL(CREATE TABLE snapshot_metadata(complete INTEGER NOT NULL); INSERT INTO snapshot_metadata VALUES(0); CREATE TABLE snapshot_files(path TEXT,parent_path TEXT,name TEXT,ext TEXT,root TEXT,size INTEGER,modified_time INTEGER,is_dir INTEGER,scan_generation INTEGER,source_id INTEGER,relative_path TEXT,cloud_copy INTEGER);)SQL");
    auto current=events.database().sources().front();
    assert(rejected([&]{events.database().mergeSourceSnapshotFile(current,staging.string());}));
    assert(events.database().search("unrelated").size()==1);
    sql(staging,"UPDATE snapshot_metadata SET complete=1");
    events.database().mergeSourceSnapshotFile(current,staging.string(),"nested");
    assert(events.database().search("unrelated").size()==1);
    // Streaming a snapshot across a batch boundary preserves all rows/cloud flags.
    SourceRecord streamed; streamed.root="/synthetic/stream"; streamed.label="stream"; streamed.type=SourceType::Cloud;
    streamed.id=events.database().saveSource(streamed); streamed=events.database().source(streamed.id);
    sql(staging,"WITH RECURSIVE n(v) AS (VALUES(0) UNION ALL SELECT v+1 FROM n WHERE v<1000) INSERT INTO snapshot_files SELECT '/synthetic/stream/item-'||v||'.txt','/synthetic/stream','item-'||v||'.txt','txt','/synthetic/stream',1,1,0,0,0,'item-'||v||'.txt',1 FROM n");
    auto streamed_merge=events.database().mergeSourceSnapshotFile(streamed,staging.string());
    assert(streamed_merge.completed && streamed_merge.indexed==1001);
    SearchQuery stream_query; stream_query.source_id=streamed.id;
    const auto streamed_rows=events.database().search(stream_query,1002);
    assert(streamed_rows.size()==1001 && streamed_rows.front().file.cloud_copy==CloudCopy::RemoteOnly);
    assert(streamed_rows.front().source_type==SourceType::Cloud);
    // A late malformed record rolls back earlier updates in the same merge.
    auto invalid_rows=std::vector<FileRecord>{streamed_rows[0].file,streamed_rows[1].file};
    invalid_rows[0].size=999; invalid_rows[1].source_id=999999;
    auto stream_guard=events.database().source(streamed.id);
    assert(rejected([&]{events.database().mergeSourceSnapshot(stream_guard,invalid_rows);}));
    const auto after_rollback=events.database().search(stream_query,1002);
    assert(after_rollback.size()==1001 && after_rollback[0].file.size==1);
    events.database().forgetSource(streamed.id);
    // Legacy generation APIs record scan membership separately from file writes.
    const auto compatibility=base/"compatibility.db";
    Database compat(compatibility.string()); compat.initialize();
    FileRecord item; assert(FileScanner::readSingle((base/"root"/"collision.txt").string(),item));
    compat.beginRootScan((base/"root").string(),1);
    compat.upsertBatch({item},(base/"root").string(),1);
    compat.completeRootScan((base/"root").string(),1,1);
    compat.beginRootScan((base/"root").string(),2);
    compat.upsertBatch({item},(base/"root").string(),2);
    assert(compat.deleteStaleForRoot((base/"root").string(),2)==0);
    assert(compat.totalFileCount()==1);
    // Traversal permission failures preserve the complete previous snapshot.
    const auto protected_root=base/"protected";
    fs::create_directories(protected_root/"denied");
    std::ofstream(protected_root/"denied"/"kept.txt")<<"kept";
    IndexManager protected_manager((base/"protected.db").string());
    protected_manager.rebuildRoot(protected_root.string());
    const auto protected_count=protected_manager.database().totalFileCount();
#if !defined(_WIN32)
    if(geteuid()!=0) {
    fs::permissions(protected_root/"denied",fs::perms::none);
    const auto permission_failure=protected_manager.rebuildRoot(protected_root.string());
    fs::permissions(protected_root/"denied",fs::perms::owner_all);
    assert(!permission_failure.completed && !permission_failure.error.empty());
    assert(protected_manager.database().totalFileCount()==protected_count);
    fs::permissions(protected_root/"denied",fs::perms::none);
    const auto event_failure=protected_manager.applyPathChange(protected_root.string(),(protected_root/"denied").string());
    fs::permissions(protected_root/"denied",fs::perms::owner_all);
    assert(!event_failure.completed && protected_manager.database().totalFileCount()==protected_count);
    } else std::cout << "permission-denied checks skipped for privileged user; traversal failure checked independently\n";
#endif
    // Removing the scan root after its first callback deterministically fails
    // directory iteration, including in privileged Linux containers.
    FileScanner changing(1);
    const auto interrupted=changing.scanTree(protected_root.string(),[&](std::vector<FileRecord>&&) {
        fs::rename(protected_root,base/"moved-protected"); return true;
    });
    assert(!interrupted.completed && !interrupted.error.empty());
    assert(protected_manager.database().totalFileCount()==protected_count);
    fs::rename(base/"moved-protected",protected_root);
    // Atomic implicit-source creation must not create path ambiguity.
    Database concurrent((base/"concurrent.db").string()); concurrent.initialize();
    std::vector<std::int64_t> ids(4); std::vector<std::thread> workers; std::atomic_bool start{false};
    for(std::size_t i=0;i<ids.size();++i) workers.emplace_back([&,i] {
        while(!start.load()) std::this_thread::yield();
        ids[i]=concurrent.ensureSourceForRoot((base/"concurrent-root").string());
    });
    start=true; for(auto& worker:workers) worker.join();
    assert(concurrent.sources().size()==1);
    for(const auto id:ids) assert(id==ids.front());
    fs::remove_all(base);
    std::cout << "source catalog tests passed\n";
}
