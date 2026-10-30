#include "core/database.h"
#include "core/directory_storage.h"
#include "core/index_storage.h"
#include "core/path_utils.h"
#include <sqlite3.h>
#include <cassert>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <functional>
#include <memory>
#include <fstream>
#if defined(__APPLE__)
#include <dlfcn.h>
#endif
#ifndef _WIN32
#include <sys/stat.h>
#endif
using namespace everything_lite;
namespace fs=std::filesystem;
using Handle=std::unique_ptr<sqlite3,decltype(&sqlite3_close)>;
#if defined(__APPLE__)
// Test-only interception delivers cancellation after native VACUUM has succeeded.
static std::atomic_bool* vacuum_return_cancel=nullptr;
extern "C" int sqlite3_exec(sqlite3* db,const char* text,sqlite3_callback callback,void* data,char** error) {
    using Exec=int(*)(sqlite3*,const char*,sqlite3_callback,void*,char**);
    static const auto native=reinterpret_cast<Exec>(dlsym(RTLD_NEXT,"sqlite3_exec"));assert(native);
    const int rc=native(db,text,callback,data,error);
    if(rc==SQLITE_OK && vacuum_return_cancel && text && std::string(text)=="VACUUM") {
        vacuum_return_cancel->store(true);vacuum_return_cancel=nullptr;
    }
    return rc;
}
#endif

static Handle open(const fs::path& path,bool writer=false) {
    sqlite3* raw=nullptr;assert(sqlite3_open(path.string().c_str(),&raw)==SQLITE_OK);
    if(writer)registerDirectoryWriter(raw);return Handle(raw,sqlite3_close);
}
static void sql(sqlite3* db,const std::string& text) {
    char* error=nullptr;const int rc=sqlite3_exec(db,text.c_str(),nullptr,nullptr,&error);
    if(error){std::cerr<<error<<'\n';sqlite3_free(error);}assert(rc==SQLITE_OK);
}
static std::int64_t scalar(sqlite3* db,const std::string& text) {
    sqlite3_stmt* raw=nullptr;assert(sqlite3_prepare_v2(db,text.c_str(),-1,&raw,nullptr)==SQLITE_OK);
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> q(raw,sqlite3_finalize);
    assert(sqlite3_step(raw)==SQLITE_ROW);return sqlite3_column_int64(raw,0);
}
static std::string rows(const fs::path& path,const std::string& table="files") {
    auto db=open(path);sqlite3_stmt* raw=nullptr;
    const auto query="SELECT * FROM "+table+" ORDER BY 1";
    assert(sqlite3_prepare_v2(db.get(),query.c_str(),-1,&raw,nullptr)==SQLITE_OK);
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> q(raw,sqlite3_finalize);
    std::ostringstream out;int rc;
    while((rc=sqlite3_step(raw))==SQLITE_ROW)for(int i=0;i<sqlite3_column_count(raw);++i) {
        const auto* value=sqlite3_column_text(raw,i);const int bytes=sqlite3_column_bytes(raw,i);
        out<<bytes<<':';if(value)out.write(reinterpret_cast<const char*>(value),bytes);out<<';';
    }
    assert(rc==SQLITE_DONE);return out.str();
}
static FileRecord file(const std::string& root,const std::string& relative,bool is_dir=false) {
    FileRecord r;r.root=root;r.path=relative=="."?root:normalizePath(root+"/"+relative);
    r.parent_path=normalizePath(fs::path(r.path).parent_path().string());r.relative_path=relative;
    r.name=fs::path(r.path).filename().string();r.extension=is_dir?"":"txt";r.is_directory=is_dir;r.size=7;r.modified_time=17;return r;
}
static std::vector<std::string> identities(const std::vector<SearchResult>& results) {
    std::vector<std::string> out;for(const auto& r:results)out.push_back(std::to_string(r.file.source_id)+":"+r.file.relative_path);return out;
}
static bool rejects(const std::function<void()>& work) {try{work();}catch(const std::exception&){return true;}return false;}
static void optimize(const fs::path& path) {
    const auto result=migrateIndexStorageToDirectory(path.string(),path.string()+".recovery.sqlite",false);
    if(!result.error.empty())std::cerr<<result.error<<'\n';
    assert(result.error.empty() && result.migration_committed && !result.vacuum_completed);
}
static void failures(const fs::path& base) {
    const auto path=base/"failures.sqlite";Database db(path.string());db.initialize();
    const std::string root="/synthetic/failure";const auto id=db.ensureSourceForRoot(root);
    db.mergeSourceSnapshot(db.source(id),{file(root,"retained.txt")});
    const auto original=rows(path);const auto sources=rows(path,"sources");
    std::atomic_bool cancel{true};
    auto result=migrateIndexStorageToDirectory(path.string(),(base/"cancel-early.sqlite").string(),true,&cancel);
    assert(!result.error.empty() && !result.migration_committed && !fs::exists(base/"cancel-early.sqlite"));
    cancel=false;
    result=migrateIndexStorageToDirectory(path.string(),(base/"cancel-backup.sqlite").string(),true,&cancel,[&](IndexStoragePhase phase){if(phase==IndexStoragePhase::Migrate)cancel=true;});
    assert(!result.error.empty() && !result.migration_committed && rows(path)==original && rows(path,"sources")==sources);
    if(!result.backup_bytes)std::cerr<<"cancel-backup: "<<result.error<<"\n";
    assert(result.backup_bytes>0 && rows(base/"cancel-backup.sqlite")==original);
    {
        IndexMaintenanceLock lock(path.string());
        assert(rejects([&]{IndexMaintenanceLock second(path.string());}));
        auto locked=migrateIndexStorageToDirectory(path.string(),(base/"locked.sqlite").string());
        assert(!locked.error.empty() && !fs::exists(base/"locked.sqlite"));
        assert(rejects([&]{compactIndexStorage(path.string(),(base/"locked-vacuum.sqlite").string());}));
    }
    {
        auto writer=open(path);sql(writer.get(),"BEGIN IMMEDIATE");
        auto busy=migrateIndexStorageToDirectory(path.string(),(base/"busy.sqlite").string());
        assert(!busy.error.empty() && !busy.migration_committed && !fs::exists(base/"busy.sqlite"));
        sql(writer.get(),"ROLLBACK");
    }
    {
        auto writer=open(path);sql(writer.get(),"UPDATE files SET parent_path='/synthetic/inconsistent'");
        const auto inconsistent=rows(path);
        auto mismatch=migrateIndexStorageToDirectory(path.string(),(base/"mismatch.sqlite").string());
        assert(!mismatch.error.empty() && !mismatch.migration_committed && rows(path)==inconsistent);
        assert(rows(base/"mismatch.sqlite")==inconsistent);
        sql(writer.get(),"UPDATE files SET parent_path='/synthetic/failure'");
    }
    cancel=false;
    result=migrateIndexStorageToDirectory(path.string(),(base/"cancel-vacuum.sqlite").string(),true,&cancel,[&](IndexStoragePhase phase){if(phase==IndexStoragePhase::Vacuum)cancel=true;});
    assert(!result.error.empty() && result.migration_committed && !result.vacuum_completed && rows(path)==original);
    db.initialize();db.verifyNameSearchIndex();
    auto raw=open(path);assert(directoryStorageReady(raw.get()));
#ifndef _WIN32
    struct stat info{};assert(::stat((base/"cancel-vacuum.sqlite").c_str(),&info)==0 && (info.st_mode&0777)==0600);
#endif
    // An old process already holding a connection cannot update any persistent catalog.
    for(const auto& query:{"UPDATE sources SET label='old'","DELETE FROM roots","DELETE FROM app_metadata","DELETE FROM compatibility_scans","DELETE FROM exclusion_policy","DELETE FROM exclusion_paths","DELETE FROM scan_seen","DELETE FROM directories","DELETE FROM file_entries"})
        assert(sqlite3_exec(raw.get(),query,nullptr,nullptr,nullptr)!=SQLITE_OK);
    sql(raw.get(),"BEGIN");assert(sqlite3_exec(raw.get(),"CREATE INDEX idx_files_path ON files(path)",nullptr,nullptr,nullptr)!=SQLITE_OK);sql(raw.get(),"ROLLBACK");
    raw.reset();
    // Compact corruption is rejected without applying the legacy repair initializer.
    auto writer=open(path,true);sql(writer.get(),"DROP INDEX idx_entries_search_name");writer.reset();
    assert(rejects([&]{db.initialize();}));
    auto check=open(path);assert(scalar(check.get(),"PRAGMA user_version")==414);
}
#if defined(__APPLE__)
static void cancellationAfterVacuum(const fs::path& base) {
    const auto path=base/"late-cancel.sqlite";Database db(path.string());db.initialize();
    const auto id=db.ensureSourceForRoot("/synthetic/late-cancel");
    db.mergeSourceSnapshot(db.source(id),{file("/synthetic/late-cancel","retained.txt")});
    const auto before=rows(path);std::atomic_bool cancel{false};vacuum_return_cancel=&cancel;
    const auto result=migrateIndexStorageToDirectory(path.string(),(base/"late-recovery.sqlite").string(),true,&cancel);
    vacuum_return_cancel=nullptr;
    assert(cancel.load() && !result.error.empty() && result.migration_committed && result.vacuum_completed);
    assert(rows(path)==before && rows(base/"late-recovery.sqlite")==before);
    db.initialize();db.verifyNameSearchIndex();
}
#endif
static void writeApis(const fs::path& base) {
    const auto path=base/"api.sqlite";Database db(path.string());db.initialize();
    const std::string root="/synthetic/api";const auto id=db.ensureSourceForRoot(root);
    auto display=file(root,"actual.txt");display.name="independent display name";
    const auto slash=db.ensureSourceForRoot("/");
    db.mergeSourceSnapshot(db.source(slash),{file("/",".",true),file("/","root-child.txt")});
    db.mergeSourceSnapshot(db.source(id),{file(root,".",true),display,file(root,"Case/x.txt"),file(root,"case/x.txt")});
    const auto before=rows(path);const auto captured=db.source(id);optimize(path);assert(rows(path)==before);
    assert(rejects([&]{db.mergeSourceSnapshot(captured,{display});}));
    assert(db.searchReliable("independent").size()==1 && db.searchReliable("actual").empty());
    auto invalid=display;invalid.parent_path="/synthetic/wrong";
    assert(rejects([&]{db.upsertBatch({invalid},root,8);}));assert(rows(path)==before);
    // Repeated snapshots preserve IDs and do not write canonical records or FTS.
    {
        auto writer=open(path,true);
        sql(writer.get(),"CREATE TABLE entry_audit(count INTEGER); INSERT INTO entry_audit VALUES(0); CREATE TRIGGER audit_entry AFTER UPDATE ON file_entries BEGIN UPDATE entry_audit SET count=count+1; END;");
        const auto fts_before=rows(path,"files_name_fts_data");
        db.mergeSourceSnapshot(db.source(id),{file(root,".",true),display,file(root,"Case/x.txt"),file(root,"case/x.txt")});
        assert(scalar(writer.get(),"SELECT count FROM entry_audit")==0 && rows(path,"files_name_fts_data")==fts_before);
    }
    assert(db.forgetSource(slash)==2);
    db.beginRootScan(root,41);db.upsertBatch({display,file(root,"fresh.txt")},root,41);
    assert(db.deleteStaleForRoot(root,41)==3);db.completeRootScan(root,41,2);
    assert(db.currentGeneration(root)==41 && db.searchReliable("fresh").size()==1);
    assert(db.deletePathAndDescendants(root+"/actual.txt")==1);assert(db.searchReliable("independent").empty());
    auto source=db.source(id);source.root="/synthetic/rebound";source.status=SourceStatus::Offline;db.saveSource(source);
    assert(db.searchReliable("fresh").front().file.path=="/synthetic/rebound/fresh.txt");
    ExclusionRules rules;rules.files={"/synthetic/rebound/fresh.txt"};assert(db.saveExclusionPolicy(rules)==1);
    assert(db.totalFileCount()==0);db.saveExclusionPolicy({});
    assert(db.deleteRoot("/synthetic/rebound")==0);
    assert(db.totalFileCount()==0);db.clear();assert(db.sources().empty());db.verifyNameSearchIndex();
    auto raw=open(path);assert(scalar(raw.get(),"SELECT count(*) FROM directories")==0);
}
static void hiddenCompact(const fs::path& base) {
    const auto path=base/"hidden.sqlite";Database db(path.string());db.initialize();
    const std::string root="/synthetic/hidden";const auto id=db.ensureSourceForRoot(root);
    auto hidden=file(root,"finder-invisible.txt");hidden.finder_hidden=true;
    db.mergeSourceSnapshot(db.source(id),{file(root,".",true),hidden});
    optimize(path);
    SearchQuery visible_only;visible_only.terms={"finder-invisible"};visible_only.include_hidden=false;
    assert(db.search(visible_only).empty());
    assert(db.search("finder-invisible").size()==1);
    hidden.finder_hidden=false;
    db.upsertBatch({hidden},root,0);
    assert(db.search(visible_only).size()==1);
}
static void partialCompact(const fs::path& base) {
    const auto path=base/"partial.sqlite";Database db(path.string());db.initialize();
    const std::string root="/synthetic/partial";const auto id=db.ensureSourceForRoot(root);
    db.mergeSourceSnapshot(db.source(id),{file(root,".",true),file(root,"blocked",true),
        file(root,"blocked/cached.txt"),file(root,"healthy",true),file(root,"healthy/gone.txt")});
    optimize(path);
    const auto success=db.source(id).last_success;
    const std::vector<std::string> unreadable{root+"/blocked"};
    const auto delta=db.mergeSourceSnapshot(db.source(id),{file(root,".",true),
        file(root,"blocked",true),file(root,"healthy",true)},"",nullptr,&unreadable);
    assert(delta.removed==1 && db.search("gone.txt").empty());
    assert(db.search("cached.txt").size()==1);
    assert(db.source(id).status==SourceStatus::Failed && db.source(id).last_success==success);
    db.verifyNameSearchIndex();
}
static void setTrace(bool enabled) {
#ifdef _WIN32
    _putenv_s("EVERYTHING_LITE_SEARCH_TRACE",enabled?"1":"0");
#else
    setenv("EVERYTHING_LITE_SEARCH_TRACE",enabled?"1":"0",1);
#endif
}
static int traceSteps(const std::string& trace,const std::string& marker) {
    int total=0;std::size_t at=0;while((at=trace.find(marker,at))!=std::string::npos){at+=marker.size();total+=std::stoi(trace.substr(at));}return total;
}
static void performanceAndQueries(const fs::path& base) {
    const auto path=base/"queries.sqlite";Database db(path.string());db.initialize();
    const std::string root="/synthetic/queries";const auto id=db.ensureSourceForRoot(root);
    std::vector<FileRecord> data;
    for(int i=0;i<10000;++i){std::ostringstream name;name<<"z"<<std::setw(7)<<std::setfill('0')<<i<<".txt";data.push_back(file(root,name.str()));}
    for(const auto& scope:{std::string("a"),std::string("砀工匠"),std::string("%_'\"")}) {
        data.push_back(file(root,scope,true));data.push_back(file(root,scope+"/keep.txt"));data.push_back(file(root,scope+"/gone.txt"));data.push_back(file(root,scope+"/nested/gone.txt"));data.push_back(file(root,scope+"0/bound.txt"));data.push_back(file(root,scope+"-other/neighbor.txt"));
    }
    for(const auto& name:{"abcdef.txt","abc---bcd---cde---def.txt","aaaaaaaa.txt","aaa---aaa.txt","砀例甲乙.txt","工匠甲乙.txt","中砀例甲.txt","ALPHA-Beta.txt","abc\"def.txt","x%_abc.txt","Aa.txt","aa.TXT","AA.txt"})
        for(const auto& parent:{"A","b"})data.push_back(file(root,std::string(parent)+"/"+name));
    db.mergeSourceSnapshot(db.source(id),data);optimize(path);db.initialize();
    std::ostringstream captured;auto* previous=std::cerr.rdbuf(captured.rdbuf());setTrace(true);
    for(const auto& scope:{std::string("a"),std::string("砀工匠"),std::string("%_'\"")}) {
        captured.str("");const auto result=db.mergeSourceSnapshot(db.source(id),{file(root,scope,true),file(root,scope+"/keep.txt")},scope);
        assert(result.removed==2 && result.indexed==2);const int steps=traceSteps(captured.str(),"merge_delete vm_steps=");
        assert(steps>0 && steps<1000);
    }
    for(const auto& query:{"z","aa","工匠","abcdef","aaaaaa","砀例甲乙","工匠甲乙","中砀例甲","alpha","abc","%_","abcdef aa","ext:txt abc","matchpath: synthetic","path:queries alpha"}) {
        captured.str("");const auto fast=db.search(query,20000),safe=db.searchReliable(query,20000);
        assert(identities(fast)==identities(safe));
        assert(identities(db.search(query,2,1))==identities(db.searchReliable(query,2,1)));
        if(std::string(query)=="z") {
            const auto trace=captured.str();const auto begin=trace.find("SQL=");assert(begin!=std::string::npos);
            const auto statement=trace.substr(begin+4,trace.find('\n',begin)-begin-4);auto raw=open(path);sqlite3_stmt* q=nullptr;
            assert(sqlite3_prepare_v2(raw.get(),("EXPLAIN QUERY PLAN "+statement).c_str(),-1,&q,nullptr)==SQLITE_OK);
            bool indexed=false;while(sqlite3_step(q)==SQLITE_ROW)if(std::string(reinterpret_cast<const char*>(sqlite3_column_text(q,3))).find("idx_entries_search_name")!=std::string::npos)indexed=true;
            sqlite3_finalize(q);assert(indexed);
        }
    }
    setTrace(false);std::cerr.rdbuf(previous);
    assert(db.search("abcdef").size()==2);assert(db.search("aaaaaa").size()==2);
    SearchQuery quote;quote.terms={"abc\"def"};assert(db.search(quote).size()==2 && identities(db.search(quote))==identities(db.searchReliable(quote)));
    // Offline nested roots retain only the parent source's own cached IDs.
    const auto child=db.ensureSourceForRoot(root+"/offline");
    db.mergeSourceSnapshot(db.source(id),{file(root,"offline/cached.txt"),file(root,"keep.txt")});
    const auto owner=db.source(id);const auto descendants=db.captureSourceDescendants(owner);
    db.mergeSourceSnapshot(owner,{file(root,"keep.txt")},"",&descendants.descendants);
    assert(db.searchReliable("cached").size()==1);
    auto raw=open(path);assert(scalar(raw.get(),"SELECT file_count FROM roots WHERE root='/synthetic/queries'")==2);
    assert(db.forgetSource(child)==0);
    // Unready FTS has no delete/update triggers; a complete snapshot restores it atomically.
    raw.reset();auto writer=open(path,true);
    sql(writer.get(),"DROP TRIGGER files_name_fts_ai; DROP TRIGGER files_name_fts_ad; DROP TRIGGER files_name_fts_au; UPDATE app_metadata SET value='0' WHERE key='name_fts_ready'; INSERT INTO files_name_fts(files_name_fts) VALUES('delete-all');");writer.reset();
    db.initialize();db.upsertBatch({file(root,"keep.txt")},root,50);db.deletePathAndDescendants(root+"/offline");
    db.mergeSourceSnapshot(db.source(id),{file(root,"keep.txt")});db.verifyNameSearchIndex();assert(db.nameSearchIndexReady());
    db.rebuildNameSearchIndex();db.verifyNameSearchIndex();
}
int main() {
    const auto base=fs::temp_directory_path()/"everything-directory-storage-tests";fs::remove_all(base);fs::create_directories(base);
    failures(base);
#if defined(__APPLE__)
    cancellationAfterVacuum(base);
#endif
    writeApis(base);hiddenCompact(base);partialCompact(base);performanceAndQueries(base);
    fs::remove_all(base);std::cout<<"Directory storage guards passed\n";
}
