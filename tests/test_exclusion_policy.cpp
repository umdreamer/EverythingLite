#include "core/exclusion_policy.h"
#include "core/index_manager.h"
#include "core/path_utils.h"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sqlite3.h>
using namespace everything_lite;
namespace fs = std::filesystem;
static void sql(const fs::path& db,const char* text) {
    sqlite3* handle=nullptr; assert(sqlite3_open(db.string().c_str(),&handle)==SQLITE_OK);
    const auto rc=sqlite3_exec(handle,text,nullptr,nullptr,nullptr); sqlite3_close(handle); assert(rc==SQLITE_OK);
}
static bool rejected(const std::function<void()>& work) { try { work(); } catch (const std::exception&) { return true; } return false; }
int main() {
    ExclusionRules rules;
    rules.directories={"/synthetic/sample"}; rules.files={"/synthetic/exact.txt"};
    rules.folder_filter="cache*"; rules.file_filter="*.TMP;?例甲.txt;?例团.txt;\"a;b.txt\";\"a\"\"b.txt\"";
    ExclusionPolicy policy(rules);
    assert(policy.excludes("/synthetic/sample",true));
    assert(policy.excludes("/synthetic/SAMPLE/nested/a.txt",false));
    assert(!policy.excludes("/synthetic/sample-other/a.txt",false));
    assert(policy.excludes("/synthetic/cache-parent/deep/a.txt",false));
    assert(policy.excludes("/synthetic/exact.txt",false));
    assert(!policy.excludes("/synthetic/exact.txt",true));
    assert(policy.excludes("/synthetic/A.tmp",false));
    assert(!policy.excludes("/synthetic/A.tmp",true));
    assert(policy.excludes("/synthetic/砀例甲.txt",false)); // E7 A0 80 middle-byte regression
    assert(policy.excludes("/synthetic/砀例团.txt",false)); // E5 9B A2; 圠 below ends in A0
    rules.file_filter="?例圠.txt"; assert(ExclusionPolicy(rules).excludes("/synthetic/砀例圠.txt",false));
    assert(policy.excludes("/synthetic/a;b.txt",false)); assert(policy.excludes("/synthetic/a\"b.txt",false));
    assert(!policy.excludes("/synthetic/砀砀例甲.txt",false));
    assert(!policy.excludes("/synthetic/Ä.tmpx",false));
    rules.file_filter="/synthetic/*/a.txt"; ExclusionPolicy single(rules);
    assert(single.excludes("/synthetic/one/a.txt",false)); assert(!single.excludes("/synthetic/one/two/a.txt",false));
    rules.file_filter="/synthetic/**/a.txt"; assert(ExclusionPolicy(rules).excludes("/synthetic/one/two/a.txt",false));
    const std::string opaque="/synthetic/bad-"+std::string(1,static_cast<char>(0xff))+".tmp";
    rules.file_filter="*.tmp"; assert(ExclusionPolicy(rules).excludes(opaque,false));
    rules.file_filter="*"; assert(ExclusionPolicy(rules).excludes(opaque,false));
    rules.file_filter="bad-?.tmp"; assert(!ExclusionPolicy(rules).excludes(opaque,false));
    rules.file_filter="Ä.txt"; assert(!ExclusionPolicy(rules).excludes("/synthetic/ä.txt",false));
    rules.file_filter="*.tmp;regex:^draft_[0-9]+\\.txt$";
    rules.folder_filter="regex:^cache[0-9]+$";
    ExclusionPolicy regex_policy(rules);
    assert(regex_policy.excludes("/synthetic/draft_12.txt",false));
    assert(regex_policy.excludes("/synthetic/anything.tmp",false));
    assert(!regex_policy.excludes("/synthetic/draft_x.txt",false));
    assert(regex_policy.excludes("/synthetic/cache2/nested/file.txt",false));
    assert(!regex_policy.excludes("/synthetic/cachex/nested/file.txt",false));
    rules.file_filter="regex:^bad-.*\\.tmp$";
    assert(!ExclusionPolicy(rules).excludes(opaque,false));
    rules.folder_filter="";
    rules.file_filter="regex:^/synthetic/one/.+\\.txt$";
    assert(ExclusionPolicy(rules).excludes("/synthetic/one/a.txt",false));
    assert(!ExclusionPolicy(rules).excludes("/synthetic/two/a.txt",false));
    rules.file_filter="regex:^(a+)+$";
    ExclusionPolicy bounded_regex(rules);
    assert(rejected([&]{bounded_regex.excludes("/synthetic/"+std::string(64,'a')+"X",false);}));
    for(const auto& invalid: {"regex:", "regex:(", "ext:tmp", "attrib:hidden", "\"open", "bad\"quote", "\"closed\"tail", "\xc0\xaf"}) {
        rules.file_filter=invalid; assert(rejected([&]{ ExclusionPolicy invalid_policy(rules); }));
    }
    rules.file_filter=""; rules.directories={"relative"}; assert(rejected([&]{ExclusionPolicy invalid_policy(rules);}));
    assert(!ExclusionPolicy({}).excludes("/Applications/Sample.app",true));
    assert(ExclusionPolicy({}).excludes("/Applications/Sample.app/Contents/Info.plist",false));
    assert(ExclusionPolicy({}).excludes("/Applications/Sample.APP/Contents",true));
    ExclusionRules include_bundles;include_bundles.index_app_bundle_contents=true;
    assert(!ExclusionPolicy(include_bundles).excludes("/Applications/Sample.app/Contents/Info.plist",false));
    const auto nonce=std::chrono::steady_clock::now().time_since_epoch().count();
    auto base=fs::temp_directory_path()/("everything-lite-policy-test-"+std::to_string(nonce));
    auto root=base/"root"; fs::create_directories(root/"cache-parent"/"nested"); fs::create_directories(root/"sample-other");
    std::ofstream(root/"cache-parent"/"nested"/"hidden.txt")<<"hidden";
    std::ofstream(root/"sample-other"/"kept.txt")<<"kept";
    std::ofstream(root/"exact.txt")<<"exact";
    // File filters do not suppress a directory's own record; folder filters
    // must not suppress an ordinary file with the same basename.
    auto typed_root=base/"types"; fs::create_directories(typed_root/"directory.tmp");
    std::ofstream(typed_root/"directory.tmp"/"inside.txt")<<"inside";
    std::ofstream(typed_root/"cache-file")<<"ordinary file";
    const auto app_root=base/"apps",app=app_root/"Sample.app";
    fs::create_directories(app/"Contents");
    std::ofstream(app/"Contents"/"Info.plist")<<"synthetic";
    IndexManager app_manager((base/"apps.db").string());auto& app_db=app_manager.database();
    assert(app_manager.rebuildRoot(app_root.string()).completed);
    assert(app_db.search("Sample.app").size()==1);
    assert(app_db.search("Info.plist").empty());
    assert(app_manager.applyPathChange(app_root.string(),(app/"Contents"/"Info.plist").string()).completed);
    assert(app_db.search("Info.plist").empty());
    app_db.saveExclusionPolicy(include_bundles);
    assert(app_manager.rebuildRoot(app_root.string()).completed);
    assert(app_db.search("Info.plist").size()==1);
    sql(base/"apps.db","DELETE FROM app_metadata WHERE key IN ('index_app_bundle_contents','app_bundle_policy_applied')");
    // The first unchanged save after upgrading an older index must remove
    // package descendants; later unchanged saves can skip the full sweep.
    app_db.saveExclusionPolicy({});
    assert(app_db.search("Info.plist").empty() && app_db.search("Sample.app").size()==1);
    app_db.saveExclusionPolicy(include_bundles);
    assert(app_manager.rebuildRoot(app_root.string()).completed && app_db.search("Info.plist").size()==1);
    app_db.saveExclusionPolicy({});
    assert(app_db.search("Info.plist").empty());
    ExclusionRules typed_rules; typed_rules.file_filter="*.tmp"; typed_rules.folder_filter="cache*";
    ExclusionPolicy typed(typed_rules); std::vector<FileRecord> typed_rows;
    auto typed_scan=FileScanner().scanTree(typed_root.string(),[&](std::vector<FileRecord>&& batch) {
        typed_rows.insert(typed_rows.end(),batch.begin(),batch.end()); return true;
    },nullptr,&typed);
    assert(typed_scan.completed && typed_rows.size()==4);
    auto file_scope=FileScanner().scanTree((typed_root/"cache-file").string(),[](std::vector<FileRecord>&& batch) { return batch.size()==1; },nullptr,&typed);
    assert(file_scope.completed && file_scope.indexed==1);
    FileRecord typed_directory; assert(FileScanner::readSingle((typed_root/"directory.tmp").string(),typed_directory,&typed));
    IndexManager manager((base/"index.db").string()); auto& db=manager.database();
    assert(db.exclusionPolicy().revision==0);
    auto first=manager.rebuildRoot(root.string()); assert(first.completed);
    auto old_source=db.sources().front();
    ExclusionRules applied; applied.folder_filter="cache*"; applied.files={(root/"exact.txt").string()};
    std::vector<std::pair<std::uint64_t,std::uint64_t>> progress;
    const auto purged=db.saveExclusionPolicy(applied,[&](std::uint64_t done,std::uint64_t total) {
        progress.emplace_back(done,total);
    });
    assert(purged==4);
    assert(!progress.empty() && progress.front().first==0);
    assert(progress.back().first==progress.back().second && progress.back().second==first.indexed);
    const auto strict_policy=db.exclusionPolicy(); assert(strict_policy.revision==1);
    assert(db.search("hidden").empty() && db.search("exact").empty());
    auto pruned=manager.rebuildRoot(root.string()); assert(pruned.completed && pruned.scanned<first.scanned);
    auto event=manager.applyPathChange(root.string(),(root/"cache-parent"/"nested"/"hidden.txt").string());
    assert(event.completed && event.scanned==0 && db.search("hidden").empty());
    FileRecord hidden; assert(FileScanner::readSingle((root/"cache-parent"/"nested"/"hidden.txt").string(),hidden));
    db.upsertBatch({hidden},root.string(),19); assert(db.search("hidden").empty());
    auto strict_source=db.sources().front();
    const auto staging=base/"snapshot.sqlite";
    sql(staging,R"SQL(CREATE TABLE snapshot_metadata(complete INTEGER NOT NULL); INSERT INTO snapshot_metadata VALUES(1);
CREATE TABLE snapshot_files(path TEXT,parent_path TEXT,name TEXT,ext TEXT,root TEXT,size INTEGER,modified_time INTEGER,is_dir INTEGER,scan_generation INTEGER,source_id INTEGER,relative_path TEXT,cloud_copy INTEGER);)SQL");
    sqlite3* staged=nullptr; assert(sqlite3_open(staging.string().c_str(),&staged)==SQLITE_OK);
    sqlite3_stmt* insert=nullptr;
    assert(sqlite3_prepare_v2(staged,"INSERT INTO snapshot_files VALUES(?,?, 'hidden.txt','txt',?,1,1,0,0,0,'cache-parent/nested/hidden.txt',2)",-1,&insert,nullptr)==SQLITE_OK);
    sqlite3_bind_text(insert,1,hidden.path.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(insert,2,hidden.parent_path.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(insert,3,root.string().c_str(),-1,SQLITE_TRANSIENT);
    assert(sqlite3_step(insert)==SQLITE_DONE); sqlite3_finalize(insert); sqlite3_close(staged);
    auto staged_merge=db.mergeSourceSnapshotFile(strict_source,staging.string(),"cache-parent");
    assert(staged_merge.completed && staged_merge.indexed==0 && db.search("hidden").empty());
    strict_source=db.sources().front();
    auto merged=db.mergeSourceSnapshot(strict_source,{hidden}); assert(merged.indexed==0 && db.totalFileCount()==0);
    assert(rejected([&]{db.mergeSourceSnapshot(old_source,{});}));
    assert(manager.rebuildRoot(root.string()).completed);
    auto before_invalid=db.totalFileCount(); auto revision=db.exclusionPolicy().revision;
    applied.file_filter="ext:txt"; assert(rejected([&]{db.saveExclusionPolicy(applied);}));
    assert(db.exclusionPolicy().revision==revision && db.totalFileCount()==before_invalid);
    assert(db.exclusionPolicy().rules().folder_filter==strict_policy.rules().folder_filter);
    assert(db.exclusionPolicy().rules().files==strict_policy.rules().files);
    assert(db.sources().front().policy_revision==revision);
    strict_source=db.sources().front();
    db.saveExclusionPolicy({}); assert(db.totalFileCount()==before_invalid); // loosening does not synthesize rows
    // Even a snapshot captured with a stricter policy may not delete rows after loosening.
    assert(rejected([&]{db.mergeSourceSnapshot(strict_source,{});}));
    assert(rejected([&]{db.mergeSourceSnapshotFile(strict_source,staging.string());}));
    assert(manager.rebuildRoot(root.string()).completed); assert(db.search("hidden").size()==1);
    auto loosened=db.sources().front(); assert(rejected([&]{db.mergeSourceSnapshot(strict_source,{});}));
    assert(db.search("hidden").size()==1);
    const auto before_race=db.totalFileCount();
    auto raced=manager.rebuildRoot(root.string(),[&](const std::string&,std::uint64_t) {
        ExclusionRules race_rules; race_rules.file_filter="*.txt"; db.saveExclusionPolicy(race_rules);
    });
    assert(!raced.completed && raced.error.find("policy changed")!=std::string::npos);
    assert(db.search("type:file").empty());
    db.saveExclusionPolicy({}); assert(manager.rebuildRoot(root.string()).completed);
    assert(db.totalFileCount()==before_race);
    loosened=db.sources().front();
    fs::rename(root,base/"offline"); assert(!manager.rebuildSource(loosened.id).completed);
    applied={}; applied.folder_filter="cache*"; assert(db.saveExclusionPolicy(applied)==3);
    assert(db.search("hidden").empty()); assert(db.source(loosened.id).status==SourceStatus::Offline);
    fs::rename(base/"offline",root);
    // Rebinding an indexed source cannot bypass the policy via UPDATE.
    auto rebound=db.source(loosened.id); rebound.root=(base/"cache-rebound").string(); db.saveSource(rebound);
    assert(db.totalFileCount()==0);
    // Excluding an ancestor does not prove a previously indexed source online.
    auto excluded_root=base/"cache-parent"/"source"; fs::create_directories(excluded_root);
    std::ofstream(excluded_root/"retained.txt")<<"retained";
    IndexManager excluded_manager((base/"excluded-offline.db").string()); auto& excluded_db=excluded_manager.database();
    assert(excluded_manager.rebuildRoot(excluded_root.string()).completed);
    const auto excluded_id=excluded_db.sources().front().id;
    const auto last_success=excluded_db.source(excluded_id).last_success;
    fs::rename(excluded_root,base/"moved-source");
    assert(!excluded_manager.rebuildSource(excluded_id).completed);
    assert(excluded_db.source(excluded_id).status==SourceStatus::Offline);
    ExclusionRules excluded_ancestor; excluded_ancestor.folder_filter="cache*"; excluded_db.saveExclusionPolicy(excluded_ancestor);
    auto absent_excluded=excluded_manager.rebuildSource(excluded_id);
    assert(!absent_excluded.completed);
    assert(excluded_db.source(excluded_id).status==SourceStatus::Offline);
    assert(excluded_db.source(excluded_id).last_success==last_success);
    fs::rename(base/"moved-source",excluded_root);
    auto online_excluded=excluded_manager.rebuildSource(excluded_id);
    assert(online_excluded.completed && online_excluded.scanned==0 && excluded_db.totalFileCount()==0);
    assert(excluded_db.source(excluded_id).status==SourceStatus::Accessible);
    // Compatibility scans capture the same source/policy contract as snapshots.
    auto legacy_root=base/"legacy-scan"; fs::create_directories(legacy_root);
    std::ofstream(legacy_root/"hidden.tmp")<<"hidden"; std::ofstream(legacy_root/"kept.txt")<<"kept";
    IndexManager legacy((base/"legacy.db").string()); auto& legacy_db=legacy.database();
    assert(legacy.rebuildRoot(legacy_root.string()).completed);
    ExclusionRules strict_legacy; strict_legacy.file_filter="*.tmp"; legacy_db.saveExclusionPolicy(strict_legacy);
    legacy_db.beginRootScan(legacy_root.string(),1);
    std::vector<FileRecord> old_rows;
    for(const auto& row:legacy_db.search("",100)) old_rows.push_back(row.file);
    legacy_db.saveExclusionPolicy({}); assert(legacy.rebuildRoot(legacy_root.string()).completed);
    const auto restored_count=legacy_db.totalFileCount(); assert(legacy_db.search("hidden").size()==1);
    assert(rejected([&]{legacy_db.upsertBatch(old_rows,legacy_root.string(),1);}));
    assert(rejected([&]{legacy_db.deleteStaleForRoot(legacy_root.string(),1);}));
    assert(rejected([&]{legacy_db.completeRootScan(legacy_root.string(),1,old_rows.size());}));
    assert(legacy_db.totalFileCount()==restored_count && legacy_db.search("hidden").size()==1);
    // A policy-only change also invalidates a started scan, without source writes.
    legacy_db.beginRootScan(legacy_root.string(),2); legacy_db.saveExclusionPolicy({});
    assert(rejected([&]{legacy_db.upsertBatch(old_rows,legacy_root.string(),2);}));
    assert(rejected([&]{legacy_db.deleteStaleForRoot(legacy_root.string(),2);}));
    assert(rejected([&]{legacy_db.completeRootScan(legacy_root.string(),2,0);}));
    // Independent source edits and a newer begin invalidate old generation ownership.
    legacy_db.beginRootScan(legacy_root.string(),3);
    auto edited_source=legacy_db.sources().front(); edited_source.label="edited"; legacy_db.saveSource(edited_source);
    assert(rejected([&]{legacy_db.upsertBatch(old_rows,legacy_root.string(),3);}));
    assert(rejected([&]{legacy_db.deleteStaleForRoot(legacy_root.string(),3);}));
    assert(rejected([&]{legacy_db.completeRootScan(legacy_root.string(),3,0);}));
    legacy_db.beginRootScan(legacy_root.string(),4); legacy_db.beginRootScan(legacy_root.string(),5);
    assert(rejected([&]{legacy_db.upsertBatch(old_rows,legacy_root.string(),4);}));
    assert(rejected([&]{legacy_db.deleteStaleForRoot(legacy_root.string(),4);}));
    std::vector<FileRecord> complete_rows;
    for(const auto& row:legacy_db.search("",100)) complete_rows.push_back(row.file);
    legacy_db.upsertBatch(complete_rows,legacy_root.string(),5);
    assert(legacy_db.deleteStaleForRoot(legacy_root.string(),5)==0);
    legacy_db.completeRootScan(legacy_root.string(),5,complete_rows.size());
    // A standalone batch remains usable; it cannot authorize deletion/completion.
    legacy_db.upsertBatch(complete_rows,legacy_root.string(),6);
    assert(rejected([&]{legacy_db.deleteStaleForRoot(legacy_root.string(),6);}));
    assert(rejected([&]{legacy_db.completeRootScan(legacy_root.string(),6,0);}));
    assert(legacy_db.totalFileCount()==restored_count);
    // A rebound/forgotten/cleared source must not turn its old begun generation
    // into an apparently standalone write that silently creates a new source.
    legacy_db.beginRootScan(legacy_root.string(),7);
    auto legacy_rebind=legacy_db.sources().front(); legacy_rebind.root=(base/"legacy-rebound").string();
    legacy_db.saveSource(legacy_rebind);
    assert(rejected([&]{legacy_db.upsertBatch(old_rows,legacy_root.string(),7);}));
    assert(legacy_db.sources().size()==1 && legacy_db.totalFileCount()==restored_count);
    legacy_db.beginRootScan(legacy_rebind.root,8);
    legacy_db.forgetSource(legacy_rebind.id);
    assert(rejected([&]{legacy_db.upsertBatch({},legacy_rebind.root,8);}));
    assert(legacy_db.sources().empty());
    legacy_db.beginRootScan(legacy_root.string(),9); legacy_db.clear();
    assert(rejected([&]{legacy_db.upsertBatch(old_rows,legacy_root.string(),9);}));
    assert(legacy_db.sources().empty() && legacy_db.totalFileCount()==0);
    if(db.nameSearchIndexAvailable()) db.verifyNameSearchIndex();
    fs::remove_all(base);
    std::cout<<"Exclusion policy tests passed\n";
}
