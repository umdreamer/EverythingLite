#include "core/database.h"
#include "core/search_revision.h"
#include "core/search_service.h"
#include <sqlite3.h>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace everything_lite;
namespace fs=std::filesystem;
static void execute(sqlite3* db,const char* sql) {
    char* error=nullptr;
    const auto rc=sqlite3_exec(db,sql,nullptr,nullptr,&error);
    if(error) { std::cerr<<error<<'\n'; sqlite3_free(error); }
    assert(rc==SQLITE_OK);
}
static void stableTies(const fs::path& path) {
    Database db(path.string()); db.initialize();
    const auto source=db.ensureSourceForRoot("/synthetic/paging");
    sqlite3* raw=nullptr; assert(sqlite3_open(path.string().c_str(),&raw)==SQLITE_OK);
    const auto insert="INSERT INTO files(id,source_id,relative_path,path,parent_path,name,ext,root,size,search_name,search_path) VALUES "
        "(13,"+std::to_string(source)+",'a','/synthetic/paging/tie.txt','/synthetic/paging','tie.txt','txt','/synthetic/paging',13,'tie.txt','/synthetic/paging/tie.txt'),"
        "(4,"+std::to_string(source)+",'b','/synthetic/paging/tie.txt','/synthetic/paging','tie.txt','txt','/synthetic/paging',4,'tie.txt','/synthetic/paging/tie.txt'),"
        "(21,"+std::to_string(source)+",'c','/synthetic/paging/tie.txt','/synthetic/paging','tie.txt','txt','/synthetic/paging',21,'tie.txt','/synthetic/paging/tie.txt'),"
        "(9,"+std::to_string(source)+",'d','/synthetic/paging/tie.txt','/synthetic/paging','tie.txt','txt','/synthetic/paging',9,'tie.txt','/synthetic/paging/tie.txt')";
    execute(raw,insert.c_str());
    // A valid alternate access path must not decide the order of equal names.
    execute(raw,"DROP INDEX idx_files_search_name; CREATE INDEX paging_ties ON files(search_name,parent_path COLLATE NOCASE,id DESC)");
    sqlite3_close(raw);
    db.rebuildNameSearchIndex();
    for(bool reliable : {false,true}) for(int repeat=0;repeat<3;++repeat) {
        const auto search=[&](std::size_t offset) {
            return reliable ? db.searchReliable("tie",2,offset) : db.search("tie",2,offset);
        };
        const auto first=search(0), second=search(2);
        assert(first.size()==2 && second.size()==2);
        assert(first[0].file.size==4 && first[1].file.size==9 && "equal names and parents must use ascending file id");
        assert(second[0].file.size==13 && second[1].file.size==21);
        assert(search(4).empty());
    }
}
static void revisionLifecycle(const fs::path& directory) {
    const auto missing=directory/"not-created"/"missing.db";
    bool refused=false;
    try { SearchRevision monitor(missing.string()); }
    catch(const std::runtime_error&) { refused=true; }
    assert(refused && !fs::exists(missing.parent_path()) && "the read-only monitor must never create paths or databases");
    const auto path=directory/"revision.db";
    Database db(path.string()); db.initialize(); db.ensureSourceForRoot("/synthetic/revision");
    SearchRevision monitor(path.string());
    const auto before=monitor.read(); assert(monitor.read()==before);
    sqlite3* raw=nullptr; assert(sqlite3_open(path.string().c_str(),&raw)==SQLITE_OK);
    execute(raw,"UPDATE sources SET label='updated'");
    const auto after=monitor.read(); assert(after!=before && monitor.read()==after);
    execute(raw,"BEGIN; UPDATE sources SET label='rolled back'; ROLLBACK");
    assert(monitor.read()==after && "rolled back writes must not invalidate pages");
    int frames=-1,checkpointed=-1;
    assert(sqlite3_wal_checkpoint_v2(raw,nullptr,SQLITE_CHECKPOINT_TRUNCATE,&frames,&checkpointed)==SQLITE_OK);
    assert(frames==0 && checkpointed==0 && "revision reads must not retain a read transaction");
    sqlite3_close(raw);
}
static void indexedFolderScope(const fs::path& path) {
    Database db(path.string()); db.initialize();
    const auto source=db.ensureSourceForRoot("/synthetic");
    sqlite3* raw=nullptr; assert(sqlite3_open(path.string().c_str(),&raw)==SQLITE_OK);
    const auto row=[&](int id,const std::string& relative,const std::string& name,bool directory=false) {
        const std::string full="/synthetic/"+relative;
        const auto slash=full.find_last_of('/');
        const std::string parent=full.substr(0,slash);
        const std::string sql="INSERT INTO files(id,source_id,relative_path,path,parent_path,name,ext,root,size,is_dir,search_name,search_path) VALUES ("
            +std::to_string(id)+","+std::to_string(source)+",'"+relative+"','"+full+"','"+parent+"','"+name+"','txt','/synthetic',1,"
            +(directory?"1":"0")+",'"+name+"','"+full+"')";
        execute(raw,sql.c_str());
    };
    row(1,"docs","docs",true);
    row(2,"docs/item-a.txt","item-a.txt");
    row(3,"docs/sub/item-b.txt","item-b.txt");
    row(4,"docs-old/item-c.txt","item-c.txt");
    row(5,"docs/space %_文/item-d.txt","item-d.txt");
    execute(raw,"UPDATE sources SET status=1"); // Offline records remain searchable.
    sqlite3_close(raw);
    db.rebuildNameSearchIndex();
    SearchQuery query; query.terms={"item"}; query.folder_scope="/synthetic/docs";
    for(bool reliable:{false,true}) {
        const auto first=reliable?db.searchReliable(query,2,0):db.search(query,2,0);
        const auto second=reliable?db.searchReliable(query,2,2):db.search(query,2,2);
        assert(first.size()==2 && second.size()==1);
        assert(first[0].file.path=="/synthetic/docs/item-a.txt");
        assert(first[1].file.path=="/synthetic/docs/sub/item-b.txt");
        assert(second[0].file.path=="/synthetic/docs/space %_文/item-d.txt");
        assert((reliable?db.searchReliable(query,2,3):db.search(query,2,3)).empty());
    }
    query.terms.clear();
    const auto descendants=db.search(query,10);
    assert(descendants.size()==3); // Folder itself is outside its contents.
    query.folder_scope="/";
    assert(db.search(query,10).size()==5);
    SearchOptions options; options.folder_scope="/synthetic/docs";
    const auto service=SearchService(path.string()).search("item",10,0,options);
    assert(service.size()==3 && service[0].source_status==SourceStatus::Offline);
}
int main() {
    const auto dir=fs::temp_directory_path()/("el-search-paging-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    stableTies(dir/"ties.db");
    revisionLifecycle(dir);
    indexedFolderScope(dir/"folder-scope.db");
    fs::remove_all(dir);
    std::cout<<"search paging tests passed\n";
}
