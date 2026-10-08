#include "core/scan_request.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <functional>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
using namespace everything_lite;
namespace fs=std::filesystem;
static ScanTask fixture(const fs::path& base) {
    fs::create_directories(base/"root");fs::create_directories(base/"job");fs::create_directories(base/"locks");
    assert(chmod((base/"job").c_str(),0700)==0);assert(chmod((base/"locks").c_str(),0700)==0);
    std::ofstream(base/"root"/"砀例甲.txt")<<"synthetic";
    SourceRecord s;s.id=7;s.root=(base/"root").string();s.type=SourceType::Unknown;s.status=SourceStatus::Accessible;
    auto probe=beginSourceProbe(s).result();assert(!probe.identity.empty());
    s.identity=probe.identity;s.mount_fingerprint=probe.mount_fingerprint;
    s.label="砀例甲 \"quote\" \\ slash";s.mode=IndexMode::Periodic;s.interval_seconds=42;s.last_check=11;s.last_success=10;s.revision=3;s.policy_revision=9;
    ExclusionRules rules;rules.files={(base/"root"/"excluded").string()};rules.directories={(base/"root"/"hidden").string()};rules.folder_filter="cache*";rules.file_filter="*.bak";
    return ScanTask({s,ExclusionPolicy(rules,9),{(base/"root"/"nested").string()},s.root,(base/"job"/"snapshot.sqlite").string(),"synthetic-nonce"},(base/"locks"/"source-7.scan.lock").string());
}
static void rejects(const std::function<void()>& action) {bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}assert(rejected && "invalid or unsafe task must be rejected");}
static std::string bytes(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
static void overwrite(const fs::path& path,const std::string& data){std::ofstream out(path,std::ios::binary|std::ios::trunc);out.write(data.data(),data.size());assert(chmod(path.c_str(),0600)==0);}
static void regressions(const fs::path& base,const ScanTask& task,const std::string& path) {
    const auto original=bytes(path);rejects([&]{writeScanTask(path,task);});assert(bytes(path)==original);
    auto leaf=base/"job"/"link.bin";fs::create_symlink(path,leaf);rejects([&]{writeScanTask(leaf.string(),task);});rejects([&]{readScanTask(leaf.string());});assert(bytes(path)==original);
    assert(chmod(path.c_str(),0644)==0);rejects([&]{readScanTask(path);});assert(chmod(path.c_str(),0600)==0);
    assert(chmod((base/"job").c_str(),0755)==0);rejects([&]{readScanTask(path);});rejects([&]{writeScanTask((base/"job"/"new.bin").string(),task);});assert(chmod((base/"job").c_str(),0700)==0);
    fs::create_directory_symlink(base/"job",base/"job-link");rejects([&]{readScanTask((base/"job-link"/"request.bin").string());});
    auto fifo=base/"job"/"fifo.bin";assert(mkfifo(fifo.c_str(),0600)==0);
    auto child=fork();assert(child>=0);
    if(child==0){alarm(1);rejects([&]{readScanTask(fifo.string());});_exit(0);}
    int child_status=0;assert(waitpid(child,&child_status,0)==child);assert(WIFEXITED(child_status) && WEXITSTATUS(child_status)==0 && "nonregular manifest must fail without blocking open");
    auto malformed=base/"job"/"malformed.bin";
    for(auto data:{std::string("garbage"),original.substr(0,original.size()-1),original+"trailing",original+original}) {overwrite(malformed,data);rejects([&]{readScanTask(malformed.string());});}
    // Magic, format version, singleton counts and typed field corruption.
    for(auto offset:{std::size_t(0),std::size_t(8),std::size_t(9),std::size_t(18),std::size_t(27)}) {auto data=original;data[offset]=char(255);overwrite(malformed,data);rejects([&]{readScanTask(malformed.string());});}
    auto nonnormalized=original;auto where=nonnormalized.find((base/"root"/"hidden").string());assert(where!=std::string::npos);nonnormalized.replace(where+(base/"root").string().size()+1,6,"a/../b");overwrite(malformed,nonnormalized);rejects([&]{readScanTask(malformed.string());});
    auto badpolicy=original;where=badpolicy.find("cache*");assert(where!=std::string::npos);badpolicy.replace(where,6,"regex:");overwrite(malformed,badpolicy);rejects([&]{readScanTask(malformed.string());});
    fs::create_directory(base/"root"/"private");assert(chmod((base/"root"/"private").c_str(),0700)==0);fs::create_directory_symlink(base/"root",base/"root-alias");
    auto aliased=base/"root-alias"/"private"/"new.bin";rejects([&]{writeScanTask(aliased.string(),task);});assert(!fs::exists(aliased) && "canonical source boundary must be checked before creating a manifest");
    auto aliaslock=base/"root-alias"/"private"/"source-7.scan.lock";ScanTask alias_task(task.scan,aliaslock.string());rejects([&]{SourceScanLock lock(alias_task);});assert(!fs::exists(aliaslock) && "canonical source boundary must be checked before creating a lock");
    auto invalid=[&](SourceScanRequest request,const std::string& lock){rejects([&]{ScanTask t(std::move(request),lock);writeScanTask((base/"job"/"invalid.bin").string(),t);});assert(!fs::exists(base/"job"/"invalid.bin"));};
    auto r=task.scan;r.source.type=static_cast<SourceType>(99);invalid(r,task.lock_path);
    const auto fingerprint=task.scan.source.mount_fingerprint;
    // Fingerprint shape is captured SourceProbe evidence, parsed without source IO.
    for(const auto& bad:{std::string("malformed"),fingerprint+"0:",fingerprint+"trailing",fingerprint.substr(0,fingerprint.size()-1),std::string("999999999999999999999999999999:")+fingerprint,std::string("01:/")+fingerprint}) {
        r.source=task.scan.source;r.source.mount_fingerprint=bad;invalid(r,task.lock_path);
    }
    auto noncanonical=fingerprint;auto canonical_pos=noncanonical.find(task.scan.source.root);assert(canonical_pos!=std::string::npos);noncanonical[canonical_pos]='x';r.source=task.scan.source;r.source.mount_fingerprint=noncanonical;invalid(r,task.lock_path);
    auto relative_canonical=fingerprint;auto root_end=relative_canonical.find(task.scan.source.root)+task.scan.source.root.size();relative_canonical.replace(root_end-5,5,"/a/..");r.source=task.scan.source;r.source.mount_fingerprint=relative_canonical;invalid(r,task.lock_path);
    auto nul_fingerprint=fingerprint;nul_fingerprint.back()='\0';r.source=task.scan.source;r.source.mount_fingerprint=nul_fingerprint;invalid(r,task.lock_path);
    r.source=task.scan.source;r.source.policy_revision=8;invalid(r,task.lock_path);
    r.source=task.scan.source;r.source.status=SourceStatus::Unverified;invalid(r,task.lock_path);
    r.source=task.scan.source;r.source.identity.clear();invalid(r,task.lock_path);
    r.source=task.scan.source;r.nonce="";invalid(r,task.lock_path);r.nonce=task.scan.nonce;
    r.scan_path="relative";invalid(r,task.lock_path);r.scan_path=task.scan.scan_path;
    r.nested_source_roots={r.source.root+"/../other"};invalid(r,task.lock_path);r.nested_source_roots=task.scan.nested_source_roots;
    r.snapshot_path=r.source.root+"/out";invalid(r,task.lock_path);r.snapshot_path=task.scan.snapshot_path;
    invalid(r,(base/"locks"/"wrong.lock").string());invalid(r,r.source.root+"/source-7.scan.lock");
    r.nonce=std::string("a\0b",3);invalid(r,task.lock_path);
    rejects([]{ExclusionRules bad;bad.file_filter="regex:bad";ExclusionPolicy policy(bad);});
    {SourceScanLock lock(task);rejects([&]{SourceScanLock second(task);});}
    struct stat st{};assert(lstat(task.lock_path.c_str(),&st)==0 && S_ISREG(st.st_mode) && (st.st_mode&0777)==0600 && st.st_size==0);auto inode=st.st_ino;
    {SourceScanLock again(task);}assert(lstat(task.lock_path.c_str(),&st)==0 && st.st_ino==inode);
    assert(chmod(task.lock_path.c_str(),0644)==0);rejects([&]{SourceScanLock lock(task);});assert(chmod(task.lock_path.c_str(),0600)==0);
    fs::remove(task.lock_path);fs::create_symlink(path,task.lock_path);rejects([&]{SourceScanLock lock(task);});assert(bytes(path)==original);
}
int main(int argc,char** argv) {
    if(argc==3 && std::string(argv[1])=="--fixture") {auto task=fixture(argv[2]);writeScanTask((fs::path(argv[2])/"job"/"request.bin").string(),task);return 0;}
    char pattern[]="/private/tmp/everything-lite-scan-request-XXXXXX";auto* created=mkdtemp(pattern);assert(created);fs::path base(created);
    auto task=fixture(base);auto path=(base/"job"/"request.bin").string();writeScanTask(path,task);
    assert(fs::exists(path) && "writing an explicit task must create its manifest");
    auto read=readScanTask(path);const auto& s=read.scan.source;const auto& original=task.scan.source;
    assert(s.id==original.id && s.label==original.label && s.type==original.type && s.mode==original.mode && s.status==original.status && s.interval_seconds==original.interval_seconds && s.identity==original.identity && s.root==original.root && s.mount_fingerprint==original.mount_fingerprint && s.last_check==11 && s.last_success==10 && s.revision==3 && s.policy_revision==9);
    assert(read.scan.policy.revision==9 && read.scan.policy.rules().files==task.scan.policy.rules().files && read.scan.policy.rules().directories==task.scan.policy.rules().directories && read.scan.policy.rules().folder_filter=="cache*" && read.scan.policy.rules().file_filter=="*.bak");
    assert(read.scan.nested_source_roots==task.scan.nested_source_roots && read.scan.scan_path==s.root && read.scan.snapshot_path==task.scan.snapshot_path && read.scan.nonce==task.scan.nonce && read.lock_path==task.lock_path);
    regressions(base,task,path);
    fs::remove_all(base);std::cout<<"scan request tests passed\n";
}
