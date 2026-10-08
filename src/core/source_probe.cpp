#include "core/source_probe.h"
#include <algorithm>
#include <cctype>
#include <utility>
#include <cerrno>
#include <mutex>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#endif
namespace everything_lite {
std::string sanitizeRemoteSource(const std::string& raw) {
    std::string source=raw;
    // Reject controls, queries, fragments and ambiguous credential/path syntax.
    if (std::any_of(source.begin(),source.end(),[](unsigned char c){return c<32 || c==127;}) ||
        source.find_first_of("?#\\")!=std::string::npos) return {};
    const auto scheme=source.find("://");
    if (scheme!=std::string::npos) source.erase(0,scheme+3);
    else if (source.rfind("//",0)==0) source.erase(0,2);
    else {
        // NFS host:/export or [IPv6]:/export, never a local device path.
        auto delimiter=source.find(":/");
        if(delimiter==std::string::npos || source.front()=='/' || source.find('@')!=std::string::npos) return {};
        source.erase(delimiter,1);
    }
    const auto slash=source.find('/');
    if(slash==std::string::npos || slash==0 || slash+1==source.size()) return {};
    std::string authority=source.substr(0,slash), path=source.substr(slash);
    // A misplaced userinfo delimiter may be escaped or nested-escaped. Refuse
    // ambiguous path syntax instead of preserving a possible credential. Do not
    // decode/re-emit the source: reject encoded structural delimiters/controls,
    // including %25 (which could acquire a new delimiter after another decode).
    if(path.find_first_of("@:;")!=std::string::npos) return {};
    const auto hex=[](unsigned char c) {
        if(c>='0' && c<='9') return c-'0';
        if(c>='a' && c<='f') return c-'a'+10;
        if(c>='A' && c<='F') return c-'A'+10;
        return -1;
    };
    for(std::size_t i=0;i<path.size();++i) {
        if(path[i]!='%') continue;
        if(i+2>=path.size()) return {};
        const auto high=hex(path[i+1]), low=hex(path[i+2]);
        if(high<0 || low<0) return {};
        const auto decoded=static_cast<unsigned char>(high*16+low);
        if(decoded<32 || decoded==127 || std::string("@:/\\?#%;").find(decoded)!=std::string::npos) return {};
        i+=2;
    }
    const auto at=authority.rfind('@');
    if(at!=std::string::npos) authority.erase(0,at+1);
    else {
        std::string lower=authority;
        std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return std::tolower(c);});
        const auto encoded_at=lower.rfind("%40");
        if(encoded_at!=std::string::npos) authority.erase(0,encoded_at+3);
    }
    if(authority.empty() || authority.find('%')!=std::string::npos || authority.find(' ')!=std::string::npos) return {};
    // Colon outside IPv6 brackets may be a password without a user delimiter.
    if(authority.front()!='[') {
        // Conservative ASCII DNS/IPv4/NetBIOS subset, not a general URL parser.
        // Parameters, punctuation, non-ASCII names and ambiguous forms produce
        // no identity instead of preserving possible credentials in a hostname.
        const auto name_char=[](unsigned char c) {
            return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_';
        };
        if(authority.size()>253) return {};
        std::vector<std::string> labels;
        std::size_t start=0;
        for(std::size_t end=0;end<=authority.size();++end) {
            if(end<authority.size() && authority[end]!='.') continue;
            auto label=authority.substr(start,end-start);
            if(label.empty() || label.size()>63 || !name_char(label.front()) || !name_char(label.back()) ||
               !std::all_of(label.begin(),label.end(),[&](unsigned char c){return name_char(c) || c=='-';})) return {};
            labels.push_back(std::move(label)); start=end+1;
        }
        const bool numeric_address=labels.size()>1 && std::all_of(authority.begin(),authority.end(),[](unsigned char c){return (c>='0' && c<='9') || c=='.';});
        if(numeric_address) {
            if(labels.size()!=4) return {};
            for(const auto& octet:labels) {
                if(octet.size()>3 || (octet.size()>1 && octet[0]=='0')) return {};
                int value=0; for(char digit:octet) value=value*10+digit-'0';
                if(value>255) return {};
            }
        }
    }
    if(authority.front()=='[') {
        if(authority.back()!=']') return {};
        const auto address=authority.substr(1,authority.size()-2);
        if(std::count(address.begin(),address.end(),':')<2 ||
           !std::all_of(address.begin(),address.end(),[](unsigned char c){return std::isxdigit(c) || c==':' || c=='.';})) return {};
    }
    std::transform(authority.begin(),authority.end(),authority.begin(),[](unsigned char c){return std::tolower(c);});
    return authority+path;
}

namespace {
bool within(const std::string& path, const std::string& root) {
    return !root.empty() && (path==root || (root=="/" && !path.empty() && path[0]=='/') ||
        (path.size()>root.size() && path.compare(0,root.size(),root)==0 && path[root.size()]=='/'));
}
std::string field(const std::string& value) { return std::to_string(value.size())+":"+value; }
std::string mountFingerprint(const MountEvidence& mount) {
    return field(mount.mount_point)+field(mount.filesystem)+field(mount.volume_uuid)+
        field(mount.local ? "local" : sanitizeRemoteSource(mount.raw_remote_source))+field(mount.mount_token);
}
}
std::optional<std::size_t> selectMountForPath(const std::string& path, const std::vector<MountEvidence>& mounts) {
    std::optional<std::size_t> selected;
    for(std::size_t i=0;i<mounts.size();++i)
        if(within(path,mounts[i].mount_point) && (!selected || mounts[i].mount_point.size()>mounts[*selected].mount_point.size())) selected=i;
    return selected;
}
ProbeResult evaluateSourceEvidence(const SourceRecord& saved, const RootEvidence& root, const std::vector<MountEvidence>& mounts) {
    ProbeResult out;
    out.canonical_root=root.canonical_root;
    const auto& mount=root.actual_mount;
    const auto remote=sanitizeRemoteSource(mount.raw_remote_source);
    const bool network=!mount.local && (mount.filesystem=="smbfs" || mount.filesystem=="nfs" || mount.filesystem=="afpfs" || mount.filesystem=="webdav");
    if(network) {
        out.type=SourceType::NAS;
        if(!remote.empty()) out.identity="mac-share:"+mount.filesystem+":"+remote;
    } else if(mount.local) {
        if(mount.internal) out.type=*mount.internal ? SourceType::Local : SourceType::External;
        if(!mount.volume_uuid.empty()) out.identity="mac-volume:"+mount.volume_uuid;
    }
    // No USB or cloud inference from names, and no identity synthesized from paths.
    out.mount_fingerprint=mountFingerprint(mount)+field(root.canonical_root)+field(root.directory_token);
    auto roots=root.equivalent_roots;
    roots.push_back(root.canonical_root);
    std::vector<std::string> boundary_proofs;
    for(const auto& other:mounts) {
        if(mountFingerprint(other)==mountFingerprint(mount)) continue;
        for(const auto& alias:roots) {
            if(other.mount_point!=alias && within(other.mount_point,alias)) {
                auto suffix=other.mount_point.substr(alias=="/" ? 0 : alias.size());
                auto boundary=root.canonical_root=="/" ? suffix : root.canonical_root+suffix;
                out.child_mount_boundaries.push_back(boundary);
                boundary_proofs.push_back(field(boundary)+field(mountFingerprint(other)));
                break;
            }
        }
    }
    std::sort(boundary_proofs.begin(),boundary_proofs.end());
    for(const auto& proof:boundary_proofs) out.boundary_fingerprint+=field(proof);
    std::sort(out.child_mount_boundaries.begin(),out.child_mount_boundaries.end());
    out.child_mount_boundaries.erase(std::unique(out.child_mount_boundaries.begin(),out.child_mount_boundaries.end()),out.child_mount_boundaries.end());
    if(saved.type==SourceType::NAS && mount.local) {
        out.status=SourceStatus::Offline; out.issue=ProbeIssue::ExpectedNetworkMount; return out;
    }
    if(out.identity.empty() || saved.identity.empty() || saved.mount_fingerprint.empty() || root.directory_token.empty()) return out;
    if(saved.identity!=out.identity) { out.issue=ProbeIssue::IdentityChanged; return out; }
    if(saved.mount_fingerprint!=out.mount_fingerprint) { out.issue=ProbeIssue::ScopeChanged; return out; }
    out.status=SourceStatus::Accessible; out.issue=ProbeIssue::None; out.update_allowed=true;
    return out;
}


#if defined(__APPLE__)
namespace {
std::string directoryToken(const struct stat& st) {
    // Birth time plus inode/device are an instance guard while the fd stays open.
    // They are NOT a permanent source identity or a promise against mount ID reuse.
    return std::to_string(st.st_dev)+":"+std::to_string(st.st_ino)+":"+
        std::to_string(st.st_birthtimespec.tv_sec)+":"+std::to_string(st.st_birthtimespec.tv_nsec);
}
MountEvidence mountEvidence(const struct statfs& st) {
    MountEvidence out;
    out.mount_point=st.f_mntonname; out.filesystem=st.f_fstypename;
    out.local=(st.f_flags&MNT_LOCAL)!=0;
    if(!out.local) out.raw_remote_source=sanitizeRemoteSource(st.f_mntfromname);
    out.mount_token=std::to_string(st.f_fsid.val[0])+":"+std::to_string(st.f_fsid.val[1]);
    return out;
}
// mountEvidence immediately discards credentials; already sanitized host/share
// uses URL-like // syntax so the common sanitizer can process it once more.
void normalizeRemoteInput(MountEvidence& mount) {
    if(!mount.raw_remote_source.empty()) mount.raw_remote_source="//"+mount.raw_remote_source;
}
std::string cfString(CFTypeRef value) {
    if(!value || CFGetTypeID(value)!=CFStringGetTypeID()) return {};
    auto str=static_cast<CFStringRef>(value);
    std::string buffer(CFStringGetMaximumSizeForEncoding(CFStringGetLength(str),kCFStringEncodingUTF8)+1,'\0');
    if(!CFStringGetCString(str,buffer.data(),buffer.size(),kCFStringEncodingUTF8)) return {};
    buffer.resize(std::char_traits<char>::length(buffer.c_str())); return buffer;
}
void volumeProperties(const std::string& path, MountEvidence& mount) {
    auto url=CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(path.data()),path.size(),true);
    if(!url) return;
    CFTypeRef value=nullptr;
    if(CFURLCopyResourcePropertyForKey(url,kCFURLVolumeUUIDStringKey,&value,nullptr)) mount.volume_uuid=cfString(value);
    if(value) CFRelease(value);
    value=nullptr;
    if(CFURLCopyResourcePropertyForKey(url,kCFURLVolumeIsInternalKey,&value,nullptr) && value && CFGetTypeID(value)==CFBooleanGetTypeID())
        mount.internal=CFBooleanGetValue(static_cast<CFBooleanRef>(value));
    if(value) CFRelease(value);
    CFRelease(url);
}
std::vector<MountEvidence> mountedFilesystems() {
    // getmntinfo owns/reuses its returned storage: copy it inside this module lock.
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);
    struct statfs* entries=nullptr;
    int count=getmntinfo(&entries,MNT_NOWAIT);
    std::vector<MountEvidence> mounts;
    if(count<=0) return mounts;
    for(int i=0;i<count;++i) { auto mount=mountEvidence(entries[i]); normalizeRemoteInput(mount); mounts.push_back(std::move(mount)); }
    return mounts;
}
ProbeResult failure(int error) {
    ProbeResult result;
    if(error==EAGAIN) {result.issue=ProbeIssue::ScopeChanged;return result;}
    // Permission, I/O and malformed paths are failures, not proof of absence.
    result.status=(error==ENOENT || error==ENODEV || error==ENXIO || error==ESTALE || error==ENOTCONN)
        ? SourceStatus::Offline : SourceStatus::Failed;
    result.issue=result.status==SourceStatus::Offline ? ProbeIssue::Missing : ProbeIssue::CheckFailed;
    return result;
}
bool captureRoot(int fd, const std::string& path, RootEvidence& root, int& error) {
    struct stat held{}, current{}; struct statfs held_mount{}, after_mount{}, current_mount{};
    if(fstat(fd,&held)!=0 || fstatfs(fd,&held_mount)!=0) {error=errno;return false;}
    char* canonical=realpath(path.c_str(),nullptr);
    if(!canonical) {error=errno;return false;}
    root.canonical_root=canonical; free(canonical);
    root.directory_token=directoryToken(held);
    root.actual_mount=mountEvidence(held_mount); normalizeRemoteInput(root.actual_mount);
    // Query the actual directory, never its guessed longest-prefix mountpoint.
    volumeProperties(root.canonical_root,root.actual_mount);
    if(stat(path.c_str(),&current)!=0 || statfs(path.c_str(),&current_mount)!=0 || fstatfs(fd,&after_mount)!=0) {error=errno;return false;}
    char fd_path[MAXPATHLEN]{};
    if(fcntl(fd,F_GETPATH,fd_path)!=0) {error=errno;return false;}
    if(directoryToken(current)!=root.directory_token || root.canonical_root!=fd_path ||
        mountFingerprint(mountEvidence(held_mount))!=mountFingerprint(mountEvidence(after_mount)) ||
        mountFingerprint(mountEvidence(held_mount))!=mountFingerprint(mountEvidence(current_mount))) {error=EAGAIN;return false;}
    // Verify firmlink aliases by directory-object equivalence. A textual prefix
    // alone is insufficient; no blindly fabricated /System/Volumes/Data alias.
    if(root.actual_mount.local && root.actual_mount.mount_point!="/" &&
       !within(root.canonical_root,root.actual_mount.mount_point)) {
        std::string alias=root.actual_mount.mount_point+root.canonical_root;
        struct stat equivalent{};
        if(stat(alias.c_str(),&equivalent)==0 && directoryToken(equivalent)==root.directory_token)
            root.equivalent_roots.push_back(std::move(alias));
    }
    return true;
}
}
#endif
struct ProbeSession::Impl {
    ProbeResult initial;
    SourceRecord saved;
    RootEvidence root;
    bool active=false;
#if defined(__APPLE__)
    int fd=-1;
    ~Impl() { if(fd>=0) close(fd); }
    void release() { if(fd>=0) close(fd); fd=-1; active=false; }
#endif
};
ProbeSession::ProbeSession():impl_(std::make_unique<Impl>()) { impl_->initial.issue=ProbeIssue::Unsupported; }
ProbeSession::~ProbeSession()=default;
ProbeSession::ProbeSession(ProbeSession&&) noexcept=default;
ProbeSession& ProbeSession::operator=(ProbeSession&&) noexcept=default;
const ProbeResult& ProbeSession::result() const {
    static const ProbeResult empty;
    return impl_ ? impl_->initial : empty;
}
ProbeSession beginSourceProbe(const SourceRecord& source) {
    ProbeSession session;
#if defined(__APPLE__)
    auto& state=*session.impl_;
    state.saved=source;
    state.fd=open(source.root.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(state.fd<0) {state.initial=failure(errno);return session;}
    int error=0;
    if(!captureRoot(state.fd,source.root,state.root,error)) {state.initial=failure(error);state.release();return session;}
    auto mounts=mountedFilesystems();
    if(mounts.empty()) {state.initial=failure(EIO);state.release();return session;}
    state.initial=evaluateSourceEvidence(source,state.root,mounts);
    state.active=true;
#else
    (void)source; // Linux native discovery is explicitly deferred, not successful.
#endif
    return session;
}
ProbeResult ProbeSession::finish() {
    ProbeResult rejected;
    rejected.issue=ProbeIssue::ScopeChanged;
#if defined(__APPLE__)
    if(!impl_ || !impl_->active) return rejected;
    auto& state=*impl_;
    RootEvidence held; int error=0;
    if(!captureRoot(state.fd,state.saved.root,held,error)) {
        auto out=failure(error); state.release(); return out;
    }
    // Reopen the current path too: old fd may still reference a replaced mount.
    auto current=beginSourceProbe(state.saved);
    auto out=current.result();
    // A failed reopen has no new scope proof. Preserve its precise cause;
    // absence of evidence must not be mistaken for an observed scope change.
    if(out.status==SourceStatus::Failed || out.status==SourceStatus::Offline ||
       out.mount_fingerprint.empty() || out.identity.empty()) {
        out.update_allowed=false; state.release(); return out;
    }
    if(held.directory_token!=state.root.directory_token || held.canonical_root!=state.root.canonical_root ||
       mountFingerprint(held.actual_mount)!=mountFingerprint(state.root.actual_mount) ||
       out.mount_fingerprint!=state.initial.mount_fingerprint ||
       out.child_mount_boundaries!=state.initial.child_mount_boundaries ||
       out.boundary_fingerprint!=state.initial.boundary_fingerprint) {
        out.status=SourceStatus::Unverified; out.issue=ProbeIssue::ScopeChanged; out.update_allowed=false;
    }
    state.release();
    return out;
#else
    rejected.issue=ProbeIssue::Unsupported;
    return rejected;
#endif
}
} // namespace everything_lite
