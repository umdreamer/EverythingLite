#include "core/source_probe.h"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <chrono>
#include <fstream>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <cerrno>
#endif
using namespace everything_lite;
static MountEvidence local(std::string path, std::string uuid, std::string token) {
    MountEvidence e; e.mount_point=path; e.filesystem="apfs"; e.volume_uuid=uuid; e.mount_token=token; e.local=true; e.internal=true; return e;
}
static void encodedRemoteTests(bool output_only=false) {
    // Synthetic markers only; no real account/userinfo fixture.
    const std::vector<std::string> ambiguous={
        "smb://synthetic-owner/synthetic-secret%40host/share",
        "//synthetic-owner/synthetic-secret%2540host/share",
        "//host/share%00synthetic-secret", "//host/share%0asynthetic-secret",
        "//host/share%7Fsynthetic-secret", "//synthetic-owner/synthetic-secret%3Aencoded%40host/share",
        "//synthetic-owner/synthetic-secret%2Fhost/share", "//host/share%5csynthetic-secret",
        "//host/share%3Fpassword=synthetic-secret", "//host/share%23synthetic-secret",
        "//host/share%", "//host/share;password=synthetic-secret"
    };
    for(const auto& raw:ambiguous) {
        if(!output_only) assert(sanitizeRemoteSource(raw).empty() && "reject encoded credential delimiter/control ambiguity before output");
        RootEvidence root; root.canonical_root="/synthetic/root"; root.directory_token="synthetic-directory";
        root.actual_mount.mount_point=root.canonical_root; root.actual_mount.filesystem="smbfs";
        root.actual_mount.raw_remote_source=raw; root.actual_mount.mount_token="synthetic-mount";
        const auto rejected=evaluateSourceEvidence({},root,{root.actual_mount});
        assert(rejected.identity.empty() && !rejected.update_allowed && "ambiguous source cannot produce a reusable identity");
        assert(rejected.mount_fingerprint.find("synthetic-secret")==std::string::npos);
        auto child=root.actual_mount; child.mount_point="/synthetic/root/nested";
        root.actual_mount=local("/synthetic/root","synthetic-uuid","synthetic-mount");
        const auto boundaries=evaluateSourceEvidence({},root,{root.actual_mount,child});
        assert(boundaries.boundary_fingerprint.find("synthetic-secret")==std::string::npos);
    }
    assert(sanitizeRemoteSource("//host/share%20name")=="host/share%20name");
}
static void pureTests() {
    encodedRemoteTests();
    assert(sanitizeRemoteSource("smb://alice:secret@SERVER/share")=="server/share");
    assert(sanitizeRemoteSource("//user:p%40ss%2Fword@server/share")=="server/share");
    assert(sanitizeRemoteSource("smb://user:p@ss@host/share")=="host/share");
    assert(sanitizeRemoteSource("//user%3Apw%40host/share")=="host/share");
    assert(sanitizeRemoteSource("[2001:db8::1]:/export")=="[2001:db8::1]/export");
    assert(sanitizeRemoteSource("host:/export")=="host/export");
    assert(sanitizeRemoteSource("//user:p/word@host/share").empty());
    assert(sanitizeRemoteSource("smb://user:pw@host/share?password=secret").empty());
    assert(sanitizeRemoteSource("/dev/disk1s1").empty());
    assert(sanitizeRemoteSource("//[user:password]/share").empty());
    assert(sanitizeRemoteSource(std::string("//host/share\0secret",19)).empty());
    const auto system=local("/","system-uuid","mount-system");
    const auto data=local("/System/Volumes/Data","data-uuid","mount-data");
    const auto child=local("/Users/foo/child","external-uuid","child-instance");
    const auto unrelated=local("/Users/foo-other","other-uuid","other-instance");
    std::vector<MountEvidence> mounts={system,data,child,unrelated};
    assert(selectMountForPath("/Users/foo-other/file",mounts)==3);
    assert(selectMountForPath("/Users/foo/file",mounts)==0);
    RootEvidence root; root.canonical_root="/Users/foo"; root.directory_token="directory-object"; root.actual_mount=data;
    SourceRecord saved; saved.root=root.canonical_root;
    auto candidate=evaluateSourceEvidence(saved,root,mounts);
    assert(candidate.identity=="mac-volume:data-uuid" && "actual root fd selects Data even though textual selection selects /");
    assert(candidate.status==SourceStatus::Unverified && !candidate.update_allowed);
    assert(candidate.type==SourceType::Local);
    assert(candidate.child_mount_boundaries==std::vector<std::string>{"/Users/foo/child"});
    auto swapped_child=child; swapped_child.mount_token="new-child-mount";
    auto changed_boundaries=evaluateSourceEvidence(saved,root,{system,data,swapped_child,unrelated});
    assert(changed_boundaries.child_mount_boundaries==candidate.child_mount_boundaries);
    assert(changed_boundaries.boundary_fingerprint!=candidate.boundary_fingerprint && "same child mount path with new instance must change end-session proof");
    saved.identity=candidate.identity; saved.mount_fingerprint=candidate.mount_fingerprint;
    auto matched=evaluateSourceEvidence(saved,root,mounts);
    assert(matched.status==SourceStatus::Accessible && matched.update_allowed);
    const auto unchanged_saved=saved;
    auto external=root; external.actual_mount.internal=false;
    assert(evaluateSourceEvidence(saved,external,mounts).type==SourceType::External);
    external.actual_mount.internal.reset();
    assert(evaluateSourceEvidence(saved,external,mounts).type==SourceType::Unknown);
    assert(saved.identity==unchanged_saved.identity && saved.mount_fingerprint==unchanged_saved.mount_fingerprint);
    auto replacement=root; replacement.actual_mount.volume_uuid="different-volume";
    assert(evaluateSourceEvidence(saved,replacement,mounts).issue==ProbeIssue::IdentityChanged);
    replacement=root; replacement.actual_mount.mount_token="new-mount";
    assert(!evaluateSourceEvidence(saved,replacement,mounts).update_allowed);
    replacement=root; replacement.directory_token="replacement-directory";
    assert(!evaluateSourceEvidence(saved,replacement,mounts).update_allowed);
    root.actual_mount.volume_uuid.clear();
    assert(evaluateSourceEvidence(saved,root,mounts).identity.empty());
    saved.type=SourceType::NAS;
    assert(evaluateSourceEvidence(saved,root,mounts).issue==ProbeIssue::ExpectedNetworkMount);
    auto unknown_remote=root; unknown_remote.actual_mount.local=false; unknown_remote.actual_mount.filesystem="unknown-network";
    assert(evaluateSourceEvidence(saved,unknown_remote,mounts).status==SourceStatus::Unverified && "unknown remote filesystem is not evidence of a missing NAS mount");
    MountEvidence network; network.mount_point="/Volumes/shared"; network.filesystem="smbfs";
    network.raw_remote_source="//user:secret@host/share"; network.mount_token="network-session";
    root.canonical_root="/Volumes/shared/sub"; root.actual_mount=network; saved={};
    candidate=evaluateSourceEvidence(saved,root,{network});
    assert(candidate.type==SourceType::NAS && candidate.identity=="mac-share:smbfs:host/share");
    assert(candidate.mount_fingerprint.find("secret")==std::string::npos);
    saved.identity=candidate.identity; saved.mount_fingerprint=candidate.mount_fingerprint;
    network.raw_remote_source="//different:password@HOST/share"; root.actual_mount=network;
    assert(evaluateSourceEvidence(saved,root,{network}).update_allowed);
    root.actual_mount.raw_remote_source="malformed-secret";
    assert(evaluateSourceEvidence(saved,root,{network}).identity.empty());
    // Equivalent roots are supplied only after native directory-object equality.
    root.canonical_root="/Users/foo"; root.actual_mount=data; root.equivalent_roots={"/System/Volumes/Data/Users/foo"}; saved={};
    auto alias_child=child; alias_child.mount_point="/System/Volumes/Data/Users/foo/nested";
    candidate=evaluateSourceEvidence(saved,root,{system,data,alias_child});
    assert(candidate.child_mount_boundaries==std::vector<std::string>{"/Users/foo/nested"});
    root.equivalent_roots.clear();
    assert(evaluateSourceEvidence(saved,root,{system,data,alias_child}).child_mount_boundaries.empty());
}

#if defined(__APPLE__)
namespace fs=std::filesystem;
static int fdCount() {
    int count=0;
    for(int fd=0;fd<1024;++fd) if(fcntl(fd,F_GETFD)!=-1) ++count;
    return count;
}
// fdopendir takes ownership on success. Actual readdir + fstatat evidence, not
// an inference from open(2), decides whether the chmod restriction is bypassed.
static bool enumerateAndStatSentinel(int fd) {
    DIR* directory=fdopendir(fd);
    if(!directory) {close(fd);return false;}
    bool found=false, metadata=false, complete=false;
    for(;;) {
        errno=0;
        const auto entry=readdir(directory);
        if(!entry) {complete=errno==0;break;}
        if(std::string(entry->d_name)=="permission-sentinel") {
            found=true;
            struct stat child{};
            metadata=fstatat(dirfd(directory),entry->d_name,&child,0)==0 && S_ISREG(child.st_mode) && child.st_size==8;
        }
    }
    closedir(directory);
    return complete && found && metadata;
}
static void nativeTests() {
    auto base=fs::temp_directory_path()/("everything-lite-probe-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(base/"root"); fs::create_directories(base/"other");
    SourceRecord saved; saved.root=(base/"root").string();
    auto discovery=beginSourceProbe(saved);
    const auto candidate=discovery.result();
    assert(candidate.status==SourceStatus::Unverified && !candidate.update_allowed);
    struct statfs actual{}; assert(statfs(saved.root.c_str(),&actual)==0);
    assert(candidate.mount_fingerprint.find(actual.f_mntonname)!=std::string::npos);
    auto url=CFURLCreateFromFileSystemRepresentation(nullptr,reinterpret_cast<const UInt8*>(saved.root.data()),saved.root.size(),true);
    assert(url);
    CFTypeRef uuid=nullptr;
    const bool has_uuid=CFURLCopyResourcePropertyForKey(url,kCFURLVolumeUUIDStringKey,&uuid,nullptr) && uuid && CFGetTypeID(uuid)==CFStringGetTypeID();
    if(has_uuid) {
        char volume_uuid[256]{};
        assert(CFStringGetCString(static_cast<CFStringRef>(uuid),volume_uuid,sizeof(volume_uuid),kCFStringEncodingUTF8));
        assert(candidate.identity==std::string("mac-volume:")+volume_uuid);
    } else {
        assert(candidate.identity.empty());
        std::cout<<"SKIP native UUID-based authorization: persistent volume UUID unavailable; scope/fd checks still run\n";
    }
    if(uuid) CFRelease(uuid);
    CFRelease(url);
    assert(candidate.canonical_root==fs::canonical(base/"root").string());
    assert(discovery.finish().issue==ProbeIssue::NeedsVerification);
    saved.identity=candidate.identity; saved.mount_fingerprint=candidate.mount_fingerprint;
    auto unchanged=beginSourceProbe(saved);
    assert(unchanged.result().update_allowed==has_uuid);
    assert(unchanged.finish().update_allowed==has_uuid);
    assert(!unchanged.finish().update_allowed && "finish is single-use");
    auto replaced=beginSourceProbe(saved);
    fs::rename(base/"root",base/"original"); fs::create_directory(base/"root");
    auto replaced_result=replaced.finish();
    assert(replaced_result.status==SourceStatus::Unverified && replaced_result.issue==ProbeIssue::ScopeChanged && "held fd alone misses same-path replacement");
    fs::remove(base/"root"); fs::rename(base/"original",base/"root");
    fs::create_directory_symlink(base/"root",base/"link");
    saved.root=(base/"link").string();
    auto symlink=beginSourceProbe(saved);
    assert(symlink.result().update_allowed==has_uuid);
    fs::remove(base/"link"); fs::create_directory_symlink(base/"other",base/"link");
    assert(!symlink.finish().update_allowed);
    saved.root=(base/"missing").string();
    assert(beginSourceProbe(saved).result().status==SourceStatus::Offline);
    std::ofstream(base/"regular-file")<<"not a directory";
    saved.root=(base/"regular-file").string();
    assert(beginSourceProbe(saved).result().status==SourceStatus::Failed);
    saved.root=(base/"root").string(); saved.type=SourceType::NAS;
    assert(beginSourceProbe(saved).result().issue==ProbeIssue::ExpectedNetworkMount);
    assert(beginSourceProbe(saved).result().status==SourceStatus::Offline);
    saved.type=SourceType::Unknown;
    const int before=fdCount();
    for(int i=0;i<80;++i) {auto short_scan=beginSourceProbe(saved); assert(short_scan.result().update_allowed==has_uuid);}
    assert(fdCount()==before && "RAII must release each single-scan directory fd");
    std::ofstream(base/"root"/"permission-sentinel")<<"sentinel";
    // Establish that the independent fixture is actually enumerable/stat-able
    // before restricting it, so a missing fixture cannot become a false skip.
    const int baseline=open(saved.root.c_str(),O_RDONLY|O_DIRECTORY);
    assert(baseline>=0 && enumerateAndStatSentinel(baseline));
    assert(chmod(saved.root.c_str(),0000)==0);
    const int permission_check=open(saved.root.c_str(),O_RDONLY|O_DIRECTORY);
    if(permission_check<0) {
        const auto permission_error=errno;
        assert(permission_error==EACCES || permission_error==EPERM);
        assert(beginSourceProbe(saved).result().status==SourceStatus::Failed);
        std::cout<<"native permission-denied: root open rejected and probe reported Failed\n";
    } else if(enumerateAndStatSentinel(permission_check)) {
        std::cout<<"SKIP native permission-denied: host UID enumerated chmod(0000) sentinel and read its metadata\n";
    } else {
        // Root-open success is weaker than traversal permission. This module
        // does not promise directory enumeration; the future Scanner must reject
        // traversal failure before committing any successful scan.
        const auto root_only=beginSourceProbe(saved).result();
        assert(root_only.status!=SourceStatus::Offline);
        std::cout<<"INFO native permission: root open succeeded but sentinel traversal failed; root-only probe does not establish traversal\n";
    }
    assert(chmod(saved.root.c_str(),0700)==0);
    fs::remove_all(base);
}
#endif
int main(int argc, char** argv) {
    if(argc==2 && std::string(argv[1])=="encoded-output") {encodedRemoteTests(true);return 0;}
    pureTests();
#if defined(__APPLE__)
    nativeTests();
    std::cout << "source probe pure + native tests passed\n";
#else
    std::cout << "source probe pure tests passed; native backend unsupported\n";
#endif
}
