#include "core/database.h"
#include "core/index_manager.h"
#include <fstream>
#include <iomanip>
#include <cstdlib>
#include "core/search_service.h"
#include <sqlite3.h>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <vector>

using namespace everything_lite;
namespace fs=std::filesystem;
static void setTrace(bool enabled) {
#if defined(_WIN32)
    _putenv_s("EVERYTHING_LITE_SEARCH_TRACE",enabled?"1":"0");
#else
    setenv("EVERYTHING_LITE_SEARCH_TRACE",enabled?"1":"0",1);
#endif
}
static int traceSteps(const std::string& text,const std::string& marker) {
    int total=0; std::size_t cursor=0;
    while((cursor=text.find(marker,cursor))!=std::string::npos) {
        cursor+=marker.size(); total+=std::stoi(text.substr(cursor));
    }
    return total;
}
static void execute(sqlite3* db,const std::string& sql) {
    char* error=nullptr; int rc=sqlite3_exec(db,sql.c_str(),nullptr,nullptr,&error);
    if(error) { std::cerr<<error<<'\n'; sqlite3_free(error); }
    assert(rc==SQLITE_OK);
}
static std::vector<std::string> plan(sqlite3* db,const std::string& sql) {
    sqlite3_stmt* stmt=nullptr;
    assert(sqlite3_prepare_v2(db,("EXPLAIN QUERY PLAN "+sql).c_str(),-1,&stmt,nullptr)==SQLITE_OK);
    std::vector<std::string> result;
    while(sqlite3_step(stmt)==SQLITE_ROW) result.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt,3)));
    sqlite3_finalize(stmt); return result;
}
static FileRecord file(const std::string& root,const std::string& relative,bool directory=false) {
    FileRecord r; r.path=root+"/"+relative; r.parent_path=fs::path(r.path).parent_path().string();
    r.name=fs::path(relative).filename().string(); r.root=root; r.is_directory=directory; return r;
}
static std::vector<std::string> paths(const std::vector<SearchResult>& results) {
    std::vector<std::string> out; for(const auto& r:results) out.push_back(r.file.path); return out;
}
int main(int argc,char** argv) {
    const std::string mode=argc>1?argv[1]:"all";
    const auto base=fs::temp_directory_path()/("el-performance-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(base); const auto path=(base/"synthetic.db").string();
    if(mode=="path") {
        const auto root=base/"root", offline=base/"offline";
        fs::create_directories(root/"old"/"nested"); fs::create_directories(offline);
        std::ofstream(root/"old"/"nested"/"moved.txt")<<"synthetic";
#if !defined(_WIN32)
        std::ofstream(root/"literal\\slash.txt")<<"synthetic";
#endif
        std::ofstream(offline/"retained.txt")<<"synthetic";
        IndexManager manager(path);
        assert(manager.rebuildRoot(root.string()).completed);
        assert(manager.rebuildRoot(offline.string()).completed);
        fs::rename(offline,base/"disconnected");
#if !defined(_WIN32)
        auto literal=manager.database().searchReliable("slash.txt",20);
        assert(literal.size()==1);
        assert(fs::exists(literal.front().file.path) && "POSIX filename containing backslash must remain an existing path");
#endif
        fs::rename(root/"old",root/"new");
        assert(manager.applyPathChange(root.string(),(root/"old").string()).completed);
        assert(manager.applyPathChange(root.string(),(root/"new").string()).completed);
        auto moved=manager.database().searchReliable("moved.txt",20);
        assert(moved.size()==1 && moved.front().file.path==(root/"new"/"nested"/"moved.txt").string());
        for(const auto& entry:manager.database().searchReliable("",100)) {
            assert(entry.file.path.find((root/"old").string())!=0);
            if(entry.file.root==root.string()) assert(fs::exists(entry.file.path));
        }
        fs::remove_all(root/"new");
        assert(manager.applyPathChange(root.string(),(root/"new").string()).completed);
        assert(manager.database().searchReliable("moved.txt",20).empty());
        assert(manager.database().searchReliable("retained.txt",20).size()==1);
        fs::remove_all(base); std::cout<<"path guards passed\n"; return 0;
    }
    Database db(path); db.initialize();
    const auto source=db.ensureSourceForRoot("/synthetic/source");
    const auto second=db.ensureSourceForRoot("/synthetic/other");
    std::vector<FileRecord> data;
    const int count=mode=="benchmark" && argc>2?std::stoi(argv[2]):10000;
    assert(count>0 && count<=10000000);
    if(mode=="benchmark") std::cout<<"synthetic_rows="<<count<<" sqlite="<<sqlite3_libversion()<<'\n';
    for(int i=0;i<count;++i) {
        std::ostringstream name; name<<"z"<<std::setw(7)<<std::setfill('0')<<i<<".txt";
        data.push_back(file("/synthetic/source",name.str()));
    }
    for(const auto& scope:{std::string("a"),std::string("砀工匠"),std::string("%_'\"")}) {
        data.push_back(file("/synthetic/source",scope,true));
        data.push_back(file("/synthetic/source",scope+"/keep.txt"));
        data.push_back(file("/synthetic/source",scope+"/gone.txt"));
        data.push_back(file("/synthetic/source",scope+"/nested/gone.txt"));
        data.push_back(file("/synthetic/source",scope+"-other/neighbor.txt"));
        data.push_back(file("/synthetic/source",scope+"0/bound.txt"));
    }
    for(const auto& name:{"Aa.txt","aa.TXT","AA.txt","工匠.txt","砀例甲.txt","needle-uncommon.txt"})
        for(const auto& parent:{"A","b"}) data.push_back(file("/synthetic/source",std::string(parent)+"/"+name));
    db.mergeSourceSnapshot(db.source(source),data);
    db.mergeSourceSnapshot(db.source(second),{file("/synthetic/other","a/gone.txt")});
    // Capture existing opt-in diagnostics from real Database statements.
    // VM work is deterministic and does not depend on CPU speed.
    std::ostringstream captured;
    auto* previous=std::cerr.rdbuf(captured.rdbuf());
    setTrace(true);
    if(mode=="all" || mode=="subtree" || mode=="benchmark") {
        for(const auto& scope:{std::string("a"),std::string("砀工匠"),std::string("%_'\"")}) {
            captured.str(""); captured.clear();
            auto before=std::chrono::steady_clock::now();
            const auto result=db.mergeSourceSnapshot(db.source(source),{file("/synthetic/source",scope,true),file("/synthetic/source",scope+"/keep.txt")},scope);
            auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();
            assert(result.removed==2 && result.indexed==2);
            int steps=traceSteps(captured.str(),"merge_delete vm_steps=");
            assert(steps>0);
            if(mode=="benchmark") std::cout<<"subtree "<<scope<<" ms="<<elapsed<<" delete_vm_steps="<<steps<<'\n';
            else assert(steps<1000 && "subtree deletion must do bounded work independent of unrelated source rows");
            assert(db.searchReliable("neighbor.txt",100).size()==3);
            assert(db.searchReliable("bound.txt",100).size()==3);
            assert(db.searchReliable("gone.txt",100).size()==1+2*(scope=="a"?2:scope=="砀工匠"?1:0));
        }
        auto existing=file("/synthetic/source","Aa.txt");
        db.mergeSourceSnapshot(db.source(source),{existing},"Aa.txt");
        assert(db.mergeSourceSnapshot(db.source(source),{},"Aa.txt").removed==1);
        assert(db.mergeSourceSnapshot(db.source(source),{},"missing.txt").removed==0);
    }
    if(mode=="all" || mode=="search" || mode=="benchmark") {
        SearchService service(path);
        for(const auto& query:{"z","aa","工匠","needle-uncommon","%_"}) {
            captured.str(""); captured.clear(); auto before=std::chrono::steady_clock::now();
            const std::size_t offset=std::string(query)=="needle-uncommon"?0:3;
            auto results=service.search(query,50,offset);
            auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();
            const auto text=captured.str(); const auto start=text.find("SQL=SELECT files.path,");
            assert(start!=std::string::npos);
            std::string actual=text.substr(start+4,text.find('\n',start)-start-4);
            const int steps=traceSteps(text," vm_steps=");
            assert(!actual.empty());
            if(mode=="benchmark") std::cout<<"search "<<query<<" ms="<<elapsed<<" vm_steps="<<steps<<" count="<<results.size()<<'\n';
            else if(std::string(query)=="z") {
                sqlite3* raw=nullptr; assert(sqlite3_open(path.c_str(),&raw)==SQLITE_OK);
                bool name_index=false;
                for(const auto& entry:plan(raw,actual)) if(entry.find("idx_files_search_name")!=std::string::npos) name_index=true;
                sqlite3_close(raw); assert(name_index && "short substring search must reuse existing name sort index");
            }
            assert(paths(results)==paths(db.searchReliable(query,50,offset)));
            if(std::string(query)=="needle-uncommon" && db.nameSearchIndexReady()) {
                assert(results.size()==2);
                sqlite3* raw=nullptr; assert(sqlite3_open(path.c_str(),&raw)==SQLITE_OK);
                bool fts=false;
                for(const auto& entry:plan(raw,actual)) if(entry.find("VIRTUAL TABLE INDEX")!=std::string::npos) fts=true;
                sqlite3_close(raw); assert(fts && "selective long terms must retain FTS access");
            }
        }
        // Compare complete ordering with the former SQLite NOCASE expression,
        // including ASCII case ties, parent paths and non-ASCII byte sequences.
        sqlite3* raw=nullptr; assert(sqlite3_open(path.c_str(),&raw)==SQLITE_OK);
        sqlite3_stmt* stmt=nullptr;
        assert(sqlite3_prepare_v2(raw,"SELECT path FROM files ORDER BY name COLLATE NOCASE,parent_path COLLATE NOCASE",-1,&stmt,nullptr)==SQLITE_OK);
        std::vector<std::string> original;
        while(sqlite3_step(stmt)==SQLITE_ROW) original.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt,0)));
        sqlite3_finalize(stmt); sqlite3_close(raw);
        assert(paths(db.searchReliable("",count+100))==original);
        auto filtered=parseSearchQuery("aa"); filtered.source_id=source;
        assert(paths(db.search(filtered,4,1))==paths(db.searchReliable(filtered,4,1)));
    }
    if(mode=="all" || mode=="initialize") {
        // Keep the writer lock throughout actual initialize and SearchService
        // construction/search. The old initializer deterministically times out.
        sqlite3* writer=nullptr; assert(sqlite3_open(path.c_str(),&writer)==SQLITE_OK);
        const auto readableWhileLocked=[&] {
            execute(writer,"BEGIN IMMEDIATE");
            try {
                db.initialize(); SearchService service(path); assert(!service.search("z",20).empty());
            } catch(const std::exception& error) {
                std::cerr.rdbuf(previous);
                std::cerr<<"initialized readers blocked by writer: "<<error.what()<<'\n';
                execute(writer,"ROLLBACK"); sqlite3_close(writer); assert(false);
            }
            execute(writer,"ROLLBACK");
        };
        readableWhileLocked();
        // Pre-stamp/current and missing core objects must take the repair path
        // once, then regain the read-only path. Existing migration race tests
        // cover genuinely legacy files-table backups under competing writers.
        execute(writer,"DELETE FROM app_metadata WHERE key='schema_413_ready_v1'; PRAGMA user_version=0;");
        db.initialize(); readableWhileLocked();
        execute(writer,"DROP TABLE scan_seen; DROP INDEX idx_files_search_name;");
        db.initialize(); readableWhileLocked();
        if(db.nameSearchIndexAvailable()) {
            execute(writer,"DROP TRIGGER files_name_fts_ai;");
            db.initialize();
            assert(!db.nameSearchIndexReady());
            assert(db.search("needle-uncommon",10).size()==2);
            readableWhileLocked();
            db.rebuildNameSearchIndex(); db.verifyNameSearchIndex();
            execute(writer,"DROP TABLE files_name_fts;");
            db.initialize();
            assert(!db.nameSearchIndexReady() && "recreated empty FTS must not inherit old ready flag");
            assert(db.search("needle-uncommon",10).size()==2);
            // Updating old content while FTS is unready must remain safe.
            auto changed=data.front(); ++changed.size;
            db.mergeSourceSnapshot(db.source(source),{changed},fs::path(changed.path).filename().string());
            db.verifyNameSearchIndex();
            // Simulate a build/database without optional trigram support. The
            // current valid no-FTS schema remains usable while a writer runs.
            execute(writer,"DROP TRIGGER files_name_fts_ai; DROP TRIGGER files_name_fts_ad; DROP TRIGGER files_name_fts_au; DROP TABLE files_name_fts; UPDATE app_metadata SET value='0' WHERE key IN ('name_fts_ready','schema_413_fts_available');");
            readableWhileLocked();
        }
        sqlite3_close(writer);
    }
    setTrace(false); std::cerr.rdbuf(previous); fs::remove_all(base);
    std::cout<<"performance guards passed: "<<mode<<'\n';
}
