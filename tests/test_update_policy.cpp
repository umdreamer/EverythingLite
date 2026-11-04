#include <cassert>
#include <iostream>
#include "core/update_policy.h"
#include "core/index_manager.h"
#include "core/index_storage.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <sqlite3.h>
#if defined(__APPLE__)
#include <sys/stat.h>
#endif
using namespace everything_lite;
namespace fs=std::filesystem;
std::uint64_t rootCount(const std::string& database,const std::string& root) {
    sqlite3* db=nullptr;assert(sqlite3_open_v2(database.c_str(),&db,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK);
    sqlite3_stmt* query=nullptr;assert(sqlite3_prepare_v2(db,"SELECT file_count FROM roots WHERE root=?",-1,&query,nullptr)==SQLITE_OK);
    sqlite3_bind_text(query,1,root.c_str(),-1,SQLITE_TRANSIENT);assert(sqlite3_step(query)==SQLITE_ROW);
    const auto result=static_cast<std::uint64_t>(sqlite3_column_int64(query,0));sqlite3_finalize(query);sqlite3_close(db);return result;
}
void scanPolicyRegression(const fs::path& base,bool compact) {
    fs::create_directories(base/"root"/".cache"/"urgent");
    fs::create_directories(base/"root"/"generated");fs::create_directories(base/"root"/"build-output");
    const auto root=(base/"root").string(),db_path=(base/"index.db").string();
    for(const auto& file:{"visible-before.txt",".cache/cached-before.txt","generated/generated-before.txt","build-output/ignored-before.txt"})std::ofstream(base/"root"/file)<<"synthetic";
    IndexManager manager(db_path);assert(manager.rebuildRoot(root).completed);
    if(compact){const auto result=migrateIndexStorageToDirectory(db_path,db_path+".recovery.sqlite",false);assert(result.error.empty());}
    auto source=manager.database().sources().front();source.last_success=10;manager.database().saveSource(source);
    for(const auto& file:{"visible-before.txt",".cache/cached-before.txt","generated/generated-before.txt","build-output/ignored-before.txt"})fs::remove(base/"root"/file);
    for(const auto& file:{"visible-after.txt",".cache/cached-after.txt",".cache/urgent/urgent-after.txt"})std::ofstream(base/"root"/file)<<"synthetic";
    const std::vector<UpdateRule> overrides={{root+"/.cache/urgent",UpdateCadence::Realtime},{root+"/generated",UpdateCadence::Periodic}};
    UpdatePolicy realtime(root,overrides,{root+"/build-output"});
    const auto updated=manager.rebuildRoots({root},{},nullptr,&realtime);assert(updated.completed && updated.deferred);
    auto& db=manager.database();
    assert(db.search("visible-before").empty() && db.search("visible-after").size()==1);
    assert(db.search("urgent-after").size()==1 && db.search("cached-after").empty());
    assert(db.search("cached-before").size()==1 && db.search("generated-before").size()==1 && db.search("ignored-before").size()==1);
    assert(db.source(source.id).last_success==10 && "Realtime-only reconciliation is not a full source refresh");
    UpdatePolicy background(root,overrides,{root+"/build-output"},true);
    assert(manager.rebuildRoot(root,{},nullptr,&background).completed);
    assert(db.search("cached-before").empty() && db.search("generated-before").empty() && db.search("cached-after").size()==1);
    assert(db.search("ignored-before").size()==1 && "Background refresh still ignores application build state");
    assert(rootCount(db_path,root)==db.totalFileCount() && "Source summary must include retained ignored cache rows");
    assert(manager.rebuildRoot(root).completed && db.search("ignored-before").empty());
    const auto ignore_root=base/"ignore-root";fs::create_directories(ignore_root/"build");
    std::ofstream(ignore_root/"build"/"only-ignored-before.txt")<<"synthetic";
    assert(manager.rebuildRoot(ignore_root.string()).completed);fs::remove(ignore_root/"build"/"only-ignored-before.txt");
    UpdatePolicy only_ignore(ignore_root.string(),{},{(ignore_root/"build").string()});
    const auto ignored=manager.rebuildRoot(ignore_root.string(),{},nullptr,&only_ignore);
    assert(ignored.completed && !ignored.deferred && "Ignored state must not create periodic work");
    assert(db.search("only-ignored-before").size()==1);
#if defined(__APPLE__)
    const auto newly_hidden=base/"root"/"newly-hidden";fs::create_directory(newly_hidden);
    const auto hidden_file=base/"root"/"newly-hidden-file.txt";
    std::ofstream(newly_hidden/"retained-visibility.txt")<<"synthetic";std::ofstream(hidden_file)<<"synthetic";
    assert(manager.rebuildRoot(root).completed);
    assert(chflags(newly_hidden.c_str(),UF_HIDDEN)==0 && chflags(hidden_file.c_str(),UF_HIDDEN)==0);
    assert(manager.rebuildRoot(root,{},nullptr,&realtime).completed);
    assert(db.search("retained-visibility").size()==1 && db.search("newly-hidden-file").size()==1);
    assert(manager.rebuildRoot(root,{},nullptr,&background).completed);
    assert(db.cachedFinderHidden((newly_hidden/"retained-visibility.txt").string())==std::optional<bool>(true));
    assert(chflags(newly_hidden.c_str(),0)==0 && chflags(hidden_file.c_str(),0)==0);
#endif


}
int main() {
    UpdatePolicy policy("/synthetic/home",{{"/synthetic/home/.cache/important",UpdateCadence::Realtime},
        {"/synthetic/home/Documents/generated",UpdateCadence::Periodic}},
        {"/synthetic/home/project/build-output"});
    assert(policy.classify("/synthetic/home/Documents/note.txt")==UpdateCadence::Realtime);
    assert(policy.classify("/synthetic/home/.cache/tool/file")==UpdateCadence::Periodic);
    assert(policy.classify("/synthetic/home/.cache/important/file")==UpdateCadence::Realtime);
    assert(policy.classify("/synthetic/home/Library/data/file")==UpdateCadence::Periodic);
    assert(policy.classify("/synthetic/home/LibraryOther/file")==UpdateCadence::Realtime);
    assert(policy.classify("/synthetic/home/Documents/generated/x")==UpdateCadence::Periodic);
    assert(policy.classify("/synthetic/home/Documents/item",true)==UpdateCadence::Periodic);
    assert(policy.classify("/synthetic/home/project/build-output/x")==UpdateCadence::Ignore);
    assert(policy.classify("/synthetic/home/project/build-output-old/x")==UpdateCadence::Realtime);
    bool rejected=false;try {UpdatePolicy invalid("/synthetic/home",{{"relative",UpdateCadence::Realtime}},{});} catch(const std::invalid_argument&) {rejected=true;}
    assert(rejected);
    for(const auto& invalid:{"relative","/synthetic//home","/synthetic/./home","/synthetic/../home","/synthetic/home/"}) {
        bool rejected_path=false;try{policy.classify(invalid);}catch(const std::invalid_argument&){rejected_path=true;}assert(rejected_path);
    }
    assert(policy.classify("/synthetic/home/Documents/.a")==UpdateCadence::Periodic);
    assert(policy.classify("/synthetic/home/Documents/a.b")==UpdateCadence::Realtime);
    assert(policy.shouldDescend("/synthetic/home/.cache"));
    assert(!policy.shouldDescend("/synthetic/home/Documents/generated"));
    const auto base=fs::temp_directory_path()/("everything-lite-tiered-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    scanPolicyRegression(base/"legacy",false);scanPolicyRegression(base/"compact",true);fs::remove_all(base);
    std::cout<<"update policy passed\n";
}
