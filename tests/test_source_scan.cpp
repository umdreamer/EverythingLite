#include "core/source_scan.h"
#include "core/database.h"
#include "core/index_manager.h"
#include "core/path_utils.h"
#include <sqlite3.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
using namespace everything_lite;
namespace fs=std::filesystem;
static std::int64_t scalar(const fs::path& path,const char* sql) {
    sqlite3* db=nullptr; assert(sqlite3_open_v2(path.c_str(),&db,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK);
    sqlite3_stmt* st=nullptr; assert(sqlite3_prepare_v2(db,sql,-1,&st,nullptr)==SQLITE_OK);
    assert(sqlite3_step(st)==SQLITE_ROW); auto n=sqlite3_column_int64(st,0);
    sqlite3_finalize(st);sqlite3_close(db);return n;
}
static void sql(const fs::path& path,const char* text) {
    sqlite3* db=nullptr;assert(sqlite3_open(path.c_str(),&db)==SQLITE_OK);
    assert(sqlite3_exec(db,text,nullptr,nullptr,nullptr)==SQLITE_OK);assert(sqlite3_close(db)==SQLITE_OK);
}
static void incomplete(const SourceScanResult& result) {
    assert(!result.stats.completed && result.snapshot_path.empty() && result.stats.indexed==0);
    assert(result.stats.cancelled || !result.stats.error.empty());
}
static SourceRecord verified(const fs::path& root) {
    SourceRecord source;source.id=7;source.root=root.string();source.type=SourceType::Local;source.status=SourceStatus::Accessible;
    const auto proof=beginSourceProbe(source).result();source.identity=proof.identity;source.mount_fingerprint=proof.mount_fingerprint;return source;
}
static bool canEnumerateSentinel(const fs::path& root) {
    const int fd=open(root.c_str(),O_RDONLY|O_DIRECTORY);if(fd<0)return false;
    DIR* directory=fdopendir(fd);if(!directory){close(fd);return false;}
    bool seen=false;errno=0;
    while(auto* entry=readdir(directory)) {
        if(std::string(entry->d_name)=="sentinel") {struct stat st{};seen=fstatat(fd,"sentinel",&st,AT_SYMLINK_NOFOLLOW)==0;break;}
    }
    closedir(directory);return seen;
}
static void scannerRegressions(const fs::path& base) {
    fs::create_directory(base/"branches");
    auto root=base/"branches";
    std::ofstream(root/"target")<<"target";
    fs::create_symlink(root/"target",root/"link");
    struct timespec times[2]={{1170000000,0},{1170000000,0}};
    assert(utimensat(AT_FDCWD,(root/"link").c_str(),times,AT_SYMLINK_NOFOLLOW)==0);
    FileRecord r;assert(FileScanner::readSingle((root/"link").string(),r));
    assert(r.modified_time==1170000000 && "link own mtime must not follow target");
    struct stat own_link{};assert(lstat((root/"link").c_str(),&own_link)==0);
    assert(r.size==static_cast<std::uint64_t>(own_link.st_size) && r.size!=6 && "link size is its own path-byte length, not target size or zero");
    fs::remove(root/"target");assert(FileScanner::readSingle((root/"link").string(),r));assert(r.modified_time==1170000000 && r.size==static_cast<std::uint64_t>(own_link.st_size));
    fs::create_directory(root/"sample");fs::create_directory(root/"sample-other");
    std::ofstream(root/"sample"/"sentinel")<<"hidden";
    std::ofstream(root/"sample-other"/"中文\\name?.txt")<<"visible";
    ScanBoundary boundary;boundary.prune_roots={(root/"sample").string(),root.string()};
    boundary.metadata_guard=[&](const std::string& path){assert(!pathIsWithin(path,(root/"sample").string()));return true;};
    std::vector<FileRecord> all;
    auto stats=FileScanner(1000).scanTree(root.string(),[&](std::vector<FileRecord>&& rows){for(auto& f:rows)all.push_back(f);return true;},nullptr,nullptr,&boundary);
    assert(stats.completed && all.size()==4);
    for(const auto& row:all)if(row.name=="link")assert(row.size==static_cast<std::uint64_t>(own_link.st_size));
    assert(all.back().path.find('\\')!=std::string::npos || all[2].path.find('\\')!=std::string::npos);
    // A directory-name filter must not mistake a file with that basename for
    // an excluded directory. This checks the d_type optimization's fallback.
    ExclusionRules rules;rules.folder_filter="*.txt";ExclusionPolicy policy(rules);
    stats=FileScanner().scanTree(root.string(),[](std::vector<FileRecord>&&){return true;},nullptr,&policy);
    assert(stats.completed && stats.indexed==6);
    boundary.metadata_guard=[](const std::string&){return false;};
    stats=FileScanner().scanTree(root.string(),[](std::vector<FileRecord>&&){return true;},nullptr,nullptr,&boundary);
    assert(!stats.completed && !stats.error.empty());
    boundary.metadata_guard={};boundary.enter_directory=[&](const std::string& path,int fd) {
        assert(fd>=0);return path==(root/"sample-other").string()?DirectoryDecision::Prune:DirectoryDecision::Enter;
    };
    stats=FileScanner().scanTree(root.string(),[](std::vector<FileRecord>&&){return true;},nullptr,nullptr,&boundary);
    assert(stats.completed && stats.indexed==2 && stats.skipped==2);
    boundary.enter_directory=[](const std::string&,int){return DirectoryDecision::Error;};
    stats=FileScanner().scanTree(root.string(),[](std::vector<FileRecord>&&){return true;},nullptr,nullptr,&boundary);
    assert(!stats.completed && !stats.error.empty());
    std::atomic_bool cancel{false};
    stats=FileScanner(1).scanTree(root.string(),[&](std::vector<FileRecord>&&){cancel=true;return true;},&cancel);
    assert(!stats.completed && stats.cancelled);
    stats=FileScanner(1).scanTree(root.string(),[](std::vector<FileRecord>&&)->bool{throw std::runtime_error("synthetic exception");});
    assert(!stats.completed && stats.error=="synthetic exception");
    stats=FileScanner(1).scanTree(root.string(),[](std::vector<FileRecord>&&){return false;});
    assert(!stats.completed && stats.cancelled);
    fs::create_directory(base/"outside");std::ofstream(base/"outside"/"sentinel")<<"foreign";
    fs::create_directory_symlink(base/"outside",root/"directory-link");
    all.clear();stats=FileScanner().scanTree(root.string(),[&](std::vector<FileRecord>&& rows){all=std::move(rows);return true;});
    assert(stats.completed && all.size()==7);
    fs::create_directory_symlink(root,base/"legacy-root-link");
    auto legacy=FileScanner().scanTree((base/"legacy-root-link").string(),[](std::vector<FileRecord>&&){return true;});
    assert(legacy.completed && legacy.indexed==7 && "unconfigured scan API must preserve bound root symlink traversal");
    fs::create_symlink(base/"outside"/"sentinel",base/"root-file-link");
    auto file_link=FileScanner().scanTree((base/"root-file-link").string(),[](std::vector<FileRecord>&&){return true;});
    assert(file_link.completed && file_link.indexed==1 && "legacy scanTree file-link scope remains an own-metadata record");
    for(const auto& row:all)assert(row.relative_path!="directory-link/sentinel" && row.path!=(root/"directory-link"/"sentinel").string());
}
static void sourceRegressions(const fs::path& base,SourceScanRequest request) {
    int next=0;
    auto fresh=[&]{auto copy=request;copy.snapshot_path=(base/"output"/("case-"+std::to_string(++next)+".sqlite")).string();return copy;};
    auto reset=[&](SourceScanRequest& r){r.source=request.source;r.nested_source_roots.clear();r.scan_path.clear();r.snapshot_path=(base/"output"/("case-"+std::to_string(++next)+".sqlite")).string();r.nonce=request.nonce;};
    struct stat permissions{};assert(stat(request.snapshot_path.c_str(),&permissions)==0 && (permissions.st_mode&0777)==0600);
    assert(stat((base/"output").c_str(),&permissions)==0 && (permissions.st_mode&0777)==0700);
    assert(scalar(request.snapshot_path,"SELECT COUNT(*) FROM snapshot_proof WHERE nonce='synthetic-nonce' AND source_id=7 AND source_revision=0 AND policy_revision=0 AND subtree_relative=''")==1);
    assert(scalar(request.snapshot_path,"SELECT COUNT(*) FROM snapshot_files WHERE source_id<>7 OR cloud_copy<>2 OR root='' OR relative_path LIKE '/%'")==0);
    assert(scalar(request.snapshot_path,"SELECT COUNT(*) FROM snapshot_files WHERE relative_path='.' AND is_dir=1")==1);
    assert(!fs::exists(request.snapshot_path+"-journal"));
    auto r=fresh();r.source.type=SourceType::Unknown;r.source.identity.clear();incomplete(scanSourceToSnapshot(r));assert(!fs::exists(r.snapshot_path));
    reset(r);r.source.type=SourceType::Unknown;auto known_unknown=scanSourceToSnapshot(r);assert(known_unknown.stats.completed && "verified Unknown classification is not missing identity evidence");
    reset(r);r.source.type=SourceType::NAS;auto mismatch=scanSourceToSnapshot(r);incomplete(mismatch);assert(mismatch.before.issue==ProbeIssue::ExpectedNetworkMount);
    reset(r);r.source.status=SourceStatus::Unverified;incomplete(scanSourceToSnapshot(r));
    reset(r);r.source.policy_revision=8;incomplete(scanSourceToSnapshot(r));
    reset(r);r.source.identity.clear();incomplete(scanSourceToSnapshot(r));
    reset(r);r.source.root+="/missing";ExclusionRules excluded;excluded.directories={r.source.root};SourceScanRequest missing{r.source,ExclusionPolicy(excluded),{}, {},r.snapshot_path,r.nonce};incomplete(scanSourceToSnapshot(missing));
    for(const auto& scope:{std::string("relative"),request.source.root+"/../root",request.source.root+"/",request.source.root+"-other"}) {reset(r);r.scan_path=scope;incomplete(scanSourceToSnapshot(r));}
    reset(r);r.scan_path=request.source.root+std::string("\0bad",4);incomplete(scanSourceToSnapshot(r));
    reset(r);r.snapshot_path=(base/"root"/"new.sqlite").string();incomplete(scanSourceToSnapshot(r));assert(!fs::exists(r.snapshot_path));
    // Existing output (including the synthetic main DB) and symlink leaves are
    // rejected before SQLite touches any bytes.
    fs::path existing=base/"output"/"existing.sqlite";std::ofstream(existing)<<"sentinel-bytes";
    reset(r);r.snapshot_path=existing.string();incomplete(scanSourceToSnapshot(r));
    std::ifstream input(existing);std::string bytes;input>>bytes;assert(bytes=="sentinel-bytes");
    auto linked=base/"output"/"linked.sqlite";fs::create_symlink(existing,linked);reset(r);r.snapshot_path=linked.string();incomplete(scanSourceToSnapshot(r));assert(fs::is_symlink(linked));
    assert(chmod((base/"output").c_str(),0755)==0);reset(r);incomplete(scanSourceToSnapshot(r));assert(!fs::exists(r.snapshot_path));assert(chmod((base/"output").c_str(),0700)==0);
    std::atomic_bool cancel{true};reset(r);incomplete(scanSourceToSnapshot(r,&cancel));assert(!fs::exists(r.snapshot_path));
    cancel=false;reset(r);auto cancelled=scanSourceToSnapshot(r,&cancel,[&](std::uint64_t){cancel=true;});incomplete(cancelled);assert(cancelled.stats.cancelled && cancelled.staged==1000 && scalar(r.snapshot_path,"SELECT complete FROM snapshot_metadata")==0);
    reset(r);auto exception=scanSourceToSnapshot(r,nullptr,[](std::uint64_t){throw std::runtime_error("synthetic progress failure");});incomplete(exception);assert(exception.stats.error=="synthetic progress failure" && scalar(r.snapshot_path,"SELECT complete FROM snapshot_metadata")==0);
    reset(r);bool broken=false;auto sqlite_error=scanSourceToSnapshot(r,nullptr,[&](std::uint64_t){if(!broken){broken=true;sql(r.snapshot_path,"DROP TABLE snapshot_files");}});incomplete(sqlite_error);assert(scalar(r.snapshot_path,"SELECT complete FROM snapshot_metadata")==0);
    reset(r);bool replaced=false;auto root_replaced=scanSourceToSnapshot(r,nullptr,[&](std::uint64_t){if(!replaced){replaced=true;fs::rename(base/"root",base/"old-root");fs::create_directory(base/"root");}});incomplete(root_replaced);assert(scalar(r.snapshot_path,"SELECT complete FROM snapshot_metadata")==0);fs::remove_all(base/"root");fs::rename(base/"old-root",base/"root");
    // Offline nested roots are still captured/pruned. Exact-root duplicate and
    // component-prefix sibling are not nested and are omitted from proof.
    fs::create_directory(base/"root"/"sample");fs::create_directory(base/"root"/"sample-other");std::ofstream(base/"root"/"sample"/"sentinel")<<"hidden";std::ofstream(base/"root"/"sample-other"/"visible")<<"visible";
    assert(chmod((base/"root"/"sample").c_str(),0000)==0);
    reset(r);r.nested_source_roots={(base/"root"/"sample").string(),(base/"root"/"sample").string(),r.source.root,r.source.root+"-other"};
    auto nested=scanSourceToSnapshot(r);assert(nested.stats.completed);assert(scalar(r.snapshot_path,"SELECT COUNT(*) FROM snapshot_nested_roots")==1);assert(scalar(r.snapshot_path,"SELECT COUNT(*) FROM snapshot_files WHERE relative_path='sample' OR relative_path LIKE 'sample/%'")==0);assert(scalar(r.snapshot_path,"SELECT COUNT(*) FROM snapshot_files WHERE relative_path='sample-other/visible'")==1);
    assert(chmod((base/"root"/"sample").c_str(),0700)==0);
    reset(r);r.scan_path=(base/"root"/"sample-other").string();auto subtree=scanSourceToSnapshot(r);assert(subtree.stats.completed && subtree.subtree_relative=="sample-other" && subtree.staged==2);assert(scalar(r.snapshot_path,"SELECT COUNT(*) FROM snapshot_files WHERE relative_path='sample-other' AND is_dir=1")==1);
    reset(r);r.scan_path=(base/"root"/"sample").string();r.nested_source_roots={r.scan_path};incomplete(scanSourceToSnapshot(r));
    fs::create_directory_symlink(base/"outside",base/"root"/"subtree-link");reset(r);r.scan_path=(base/"root"/"subtree-link").string();auto link_file=scanSourceToSnapshot(r);assert(link_file.stats.completed && link_file.staged==1);assert(scalar(r.snapshot_path,"SELECT is_dir FROM snapshot_files")==0);
    struct stat subtree_link{};assert(lstat((base/"root"/"subtree-link").c_str(),&subtree_link)==0);assert(scalar(r.snapshot_path,"SELECT size FROM snapshot_files")==subtree_link.st_size);
    reset(r);r.scan_path=(base/"root"/"subtree-link"/"sentinel").string();incomplete(scanSourceToSnapshot(r));
    fs::create_directory_symlink(base/"root",base/"bound-root");reset(r);r.source=verified(base/"bound-root");auto bound=scanSourceToSnapshot(r);assert(bound.stats.completed && scalar(r.snapshot_path,"SELECT is_dir FROM snapshot_files WHERE relative_path='.'")==1);
    reset(r);r.source=verified(base/"bound-root");bool retargeted=false;auto retarget=scanSourceToSnapshot(r,nullptr,[&](std::uint64_t){if(!retargeted){retargeted=true;fs::remove(base/"bound-root");fs::create_directory_symlink(base/"outside",base/"bound-root");}});incomplete(retarget);assert(scalar(r.snapshot_path,"SELECT complete FROM snapshot_metadata")==0);
    fs::create_directory(base/"permission-root");std::ofstream(base/"permission-root"/"sentinel")<<"permission";reset(r);r.source=verified(base/"permission-root");assert(chmod((base/"permission-root").c_str(),0000)==0);
    if(canEnumerateSentinel(base/"permission-root"))std::cout<<"SKIP permission denial: host UID actually enumerated chmod(0000) sentinel and read metadata\n";
    else incomplete(scanSourceToSnapshot(r));assert(chmod((base/"permission-root").c_str(),0700)==0);
    std::ofstream(base/"root"/"sentinel")<<"permission";reset(r);bool permissions_changed=false;auto permission_change=scanSourceToSnapshot(r,nullptr,[&](std::uint64_t){if(!permissions_changed){permissions_changed=true;assert(chmod((base/"root").c_str(),0000)==0);}});
    const bool bypass=canEnumerateSentinel(base/"root");assert(chmod((base/"root").c_str(),0700)==0);if(bypass)std::cout<<"SKIP permission-change denial: host UID enumerated chmod(0000) sentinel and read metadata\n";else {incomplete(permission_change);assert(scalar(r.snapshot_path,"SELECT complete FROM snapshot_metadata")==0);}
    fs::create_directories(base/"mutable-root"/"child");
    for(int i=0;i<2101;++i)std::ofstream(base/"mutable-root"/"child"/("child-"+std::to_string(i)))<<"synthetic";
    reset(r);r.source=verified(base/"mutable-root");bool child_replaced=false;
    auto child_change=scanSourceToSnapshot(r,nullptr,[&](std::uint64_t){if(!child_replaced){child_replaced=true;fs::rename(base/"mutable-root"/"child",base/"mutable-root"/"moved");fs::create_directory(base/"mutable-root"/"child");}});
    assert(!child_change.stats.completed && "child directory binding change must invalidate full-root scan");incomplete(child_change);assert(scalar(r.snapshot_path,"SELECT complete FROM snapshot_metadata")==0);
    reset(r);r.source=verified(base/"mutable-root");r.scan_path=(base/"mutable-root"/"moved").string();bool scope_replaced=false;
    auto scope_change=scanSourceToSnapshot(r,nullptr,[&](std::uint64_t){if(!scope_replaced){scope_replaced=true;fs::rename(base/"mutable-root"/"moved",base/"mutable-root"/"moved-again");fs::create_directory(base/"mutable-root"/"moved");}});
    incomplete(scope_change);assert(scalar(r.snapshot_path,"SELECT complete FROM snapshot_metadata")==0);
    fs::create_directory(base/"replace-output");assert(chmod((base/"replace-output").c_str(),0700)==0);
    reset(r);r.snapshot_path=(base/"replace-output"/"snapshot.sqlite").string();bool parent_replaced=false;
    auto parent_change=scanSourceToSnapshot(r,nullptr,[&](std::uint64_t){if(!parent_replaced){parent_replaced=true;fs::rename(base/"replace-output",base/"held-output");fs::create_directory(base/"replace-output");assert(chmod((base/"replace-output").c_str(),0700)==0);std::ofstream(r.snapshot_path)<<"replacement-parent-sentinel";}});
    incomplete(parent_change);assert(scalar(base/"held-output"/"snapshot.sqlite","SELECT complete FROM snapshot_metadata")==0);
    std::ifstream parent_sentinel(r.snapshot_path);std::string parent_bytes;parent_sentinel>>parent_bytes;assert(parent_bytes=="replacement-parent-sentinel");
    reset(r);bool leaf_replaced=false;const auto old_leaf=base/"output"/"held-leaf.sqlite";
    auto leaf_change=scanSourceToSnapshot(r,nullptr,[&](std::uint64_t){if(!leaf_replaced){leaf_replaced=true;fs::rename(r.snapshot_path,old_leaf);fs::create_symlink(existing,r.snapshot_path);}});
    incomplete(leaf_change);assert(scalar(old_leaf,"SELECT complete FROM snapshot_metadata")==0);assert(fs::is_symlink(r.snapshot_path));std::ifstream leaf_target(existing);std::string leaf_bytes;leaf_target>>leaf_bytes;assert(leaf_bytes=="sentinel-bytes");
    fs::create_directory_symlink(base/"output",base/"output-link");reset(r);r.snapshot_path=(base/"output-link"/"symlink-parent.sqlite").string();incomplete(scanSourceToSnapshot(r));assert(!fs::exists(r.snapshot_path));
    // Real Database differential merge. Audit canonical files writes only.
    Database db((base/"output"/"main.sqlite").string());db.initialize();auto saved=request.source;saved.id=0;saved.id=db.saveSource(saved);saved=db.source(saved.id);
    reset(r);r.source=saved;auto first=scanSourceToSnapshot(r);assert(first.stats.completed);assert(scalar(first.snapshot_path,"SELECT size FROM snapshot_files WHERE relative_path='subtree-link'")==subtree_link.st_size);auto merged=db.mergeSourceSnapshotFile(saved,first.snapshot_path);assert(merged.completed);
    sql(db.path(),"CREATE TABLE audit(writes INTEGER);INSERT INTO audit VALUES(0);CREATE TRIGGER audit_i AFTER INSERT ON files BEGIN UPDATE audit SET writes=writes+1;END;CREATE TRIGGER audit_u AFTER UPDATE ON files BEGIN UPDATE audit SET writes=writes+1;END;CREATE TRIGGER audit_d AFTER DELETE ON files BEGIN UPDATE audit SET writes=writes+1;END;");
    saved=db.source(saved.id);reset(r);r.source=saved;auto unchanged=scanSourceToSnapshot(r);assert(unchanged.stats.completed);assert(db.mergeSourceSnapshotFile(saved,unchanged.snapshot_path).completed);assert(scalar(db.path(),"SELECT writes FROM audit")==0);
    reset(r);r.source=db.source(saved.id);r.snapshot_path=db.path();incomplete(scanSourceToSnapshot(r));assert(scalar(db.path(),"SELECT writes FROM audit")==0);
    auto main_bytes=[&]{std::ifstream stream(db.path(),std::ios::binary);return std::string(std::istreambuf_iterator<char>(stream),{});};
    const auto protected_main=main_bytes();incomplete(scanSourceToSnapshot(r));assert(main_bytes()==protected_main);
    fs::create_directory_symlink(base/"root",base/"manager-root-link");
    IndexManager manager((base/"output"/"legacy-manager.sqlite").string());assert(manager.rebuildRoot((base/"manager-root-link").string()).completed);assert(manager.database().search("item-13.txt").size()==1);
    const auto old_root_mtime=scalar(db.path(),"SELECT modified_time FROM files WHERE relative_path='.'");
    fs::remove(base/"root"/"item-0.txt");saved=db.source(saved.id);reset(r);r.source=saved;auto changed=scanSourceToSnapshot(r);assert(changed.stats.completed);auto delta=db.mergeSourceSnapshotFile(saved,changed.snapshot_path);assert(delta.completed && delta.removed==1 && scalar(db.path(),"SELECT writes FROM audit")==1+(old_root_mtime!=scalar(db.path(),"SELECT modified_time FROM files WHERE relative_path='.'")));
}
int main(int argc,char** argv) {
    char pattern[]="/private/tmp/everything-lite-source-scan-XXXXXX";
    char* created=mkdtemp(pattern);assert(created);fs::path base(created);
    scannerRegressions(base);
    if(argc==2 && std::string(argv[1])=="scanner") {fs::remove_all(base);return 0;}
    fs::create_directory(base/"root");fs::create_directory(base/"output");assert(chmod((base/"output").c_str(),0700)==0);
    for(int i=0;i<2107;++i) std::ofstream(base/"root"/("item-"+std::to_string(i)+".txt"))<<"synthetic";
    SourceRecord source;source.id=7;source.root=(base/"root").string();source.type=SourceType::Local;source.status=SourceStatus::Accessible;
    auto discovered=beginSourceProbe(source).result();
    if(discovered.identity.empty()) {std::cout<<"SKIP native authorization: no volume UUID evidence\n";fs::remove_all(base);return 0;}
    source.identity=discovered.identity;source.mount_fingerprint=discovered.mount_fingerprint;
    SourceScanRequest request;request.source=source;request.snapshot_path=(base/"output"/"snapshot.sqlite").string();request.nonce="synthetic-nonce";
    auto result=scanSourceToSnapshot(request);
    if(!result.stats.completed) std::cerr<<"stream snapshot failed: "<<result.stats.error<<"\n";
    assert(result.stats.completed && "verified source must produce a complete streamed snapshot");
    assert(result.stats.indexed==0 && result.staged==2108 && result.batches==3 && result.max_batch<=1000);
    assert(scalar(result.snapshot_path,"SELECT COUNT(*) FROM snapshot_metadata WHERE complete=1")==1);
    assert(scalar(result.snapshot_path,"SELECT COUNT(*) FROM snapshot_files")==2108);
    sourceRegressions(base,request);
    fs::remove_all(base);std::cout<<"source scan tests passed\n";
}
