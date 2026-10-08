#include "core/scan_request.h"
#include "core/path_utils.h"
#include <filesystem>
#include <algorithm>
#include <stdexcept>
#include <utility>
#if defined(__APPLE__)
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace everything_lite {
namespace {
void require(bool ok,const char* error){if(!ok)throw std::runtime_error(error);}
constexpr std::size_t maxManifest=16*1024*1024, maxText=1024*1024, maxEntries=65536;
bool absolute(const std::string& p){return !p.empty() && p.front()=='/' && p.find('\0')==std::string::npos && normalizePath(p)==p;}
void cleanText(const std::string& s){require(s.size()<=maxText && s.find('\0')==std::string::npos,"invalid task text");}
void paths(const std::vector<std::string>& list){require(list.size()<=maxEntries,"too many task paths");for(const auto& p:list)require(absolute(p),"task paths must be normalized absolute paths");}
// SourceProbe's captured fingerprint is seven byte-length-prefixed fields:
// mount point, filesystem, UUID, sanitized share/local marker, mount token,
// canonical root, directory token. Parsing this evidence never probes the root.
// Current identity and path membership are checked by SourceScan AFTER flock.
std::string evidenceField(const std::string& data,std::size_t& offset) {
    require(offset<data.size(),"truncated captured evidence");
    const auto first=offset;std::size_t length=0;
    while(offset<data.size() && data[offset]>='0' && data[offset]<='9') {
        const auto digit=static_cast<std::size_t>(data[offset++]-'0');
        require(length<=(maxText-digit)/10,"captured evidence length out of range");
        length=length*10+digit;
    }
    require(offset>first && (offset==first+1 || data[first]!='0') &&
        offset<data.size() && data[offset]==':',"invalid captured evidence length");
    ++offset;require(length<=data.size()-offset,"truncated captured evidence field");
    auto result=data.substr(offset,length);offset+=length;return result;
}
std::vector<std::string> evidenceFields(const std::string& data,std::size_t count) {
    cleanText(data);std::size_t offset=0;std::vector<std::string> fields;
    for(std::size_t i=0;i<count;++i)fields.push_back(evidenceField(data,offset));
    require(offset==data.size(),"trailing captured evidence fields");return fields;
}
void mountFields(const std::vector<std::string>& fields,bool child_boundary=false) {
    require(absolute(fields[0]) && !fields[1].empty() && !fields[4].empty(),"invalid captured mount evidence");
    // SourceProbe retains every child boundary for pruning, even when a
    // nonlocal child's remote syntax cannot establish a reusable identity.
    // Only child evidence may carry the producer's empty sanitized marker;
    // owning source evidence still requires local or a sanitized remote share.
    require((child_boundary && fields[3].empty()) || fields[3]=="local" ||
        (!fields[3].empty() && sanitizeRemoteSource("//"+fields[3])==fields[3]),"invalid captured remote evidence");
}
void validateRequest(const SourceScanRequest& r) {
    const auto& s=r.source;validateSource(s);
    require(s.id>0 && s.status==SourceStatus::Accessible && !s.identity.empty() && !s.mount_fingerprint.empty(),"task requires verified source evidence");
    require(s.last_check>=0 && s.last_success>=0 && s.policy_revision>=0 && s.policy_revision==r.policy.revision,"invalid task source revision or timestamp");
    cleanText(s.label);cleanText(s.identity);
    const auto captured_root=capturedSourceCanonicalRoot(s.mount_fingerprint);
    require(absolute(s.root) && absolute(r.scan_path) && absolute(r.snapshot_path),"task paths must be normalized absolute paths");
    require(pathIsWithin(r.scan_path,s.root) && !pathIsWithin(r.snapshot_path,s.root),"task scope or private output outside allowed boundary");
    require(!pathIsWithin(r.snapshot_path,captured_root),"private output cannot be inside captured canonical source root");
    require(!r.nonce.empty() && r.nonce.size()<=256,"invalid task nonce");
    for(unsigned char c:r.nonce)require((c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') || c=='-' || c=='_',"invalid task nonce");
    paths(r.nested_source_roots);paths(r.policy.rules().directories);paths(r.policy.rules().files);
    cleanText(r.policy.rules().folder_filter);cleanText(r.policy.rules().file_filter);
}
void validate(const ScanTask& task) {
    validateRequest(task.scan);
    const auto& r=task.scan;const auto& s=r.source;
    require(absolute(task.lock_path) && !pathIsWithin(task.lock_path,s.root),"task lock outside allowed boundary");
    require(!pathIsWithin(task.lock_path,capturedSourceCanonicalRoot(s.mount_fingerprint)),"private output cannot be inside captured canonical source root");
    require(std::filesystem::path(task.lock_path).filename().string()=="source-"+std::to_string(s.id)+".scan.lock","invalid persistent source lock name");
}
// Canonical little-endian framing. Each field has an explicit type byte; integer,
// text and list tags are distinct. Fixed field order and exact singleton counts
// reject duplicate metadata/source/version records and any trailing data.
struct Encoder {
    std::string data=std::string("ELSCAN\0\1",8);
    void raw(std::uint64_t n){for(int i=0;i<8;++i){data.push_back(static_cast<char>(n&255));n>>=8;}}
    void number(std::int64_t n){data.push_back(1);raw(static_cast<std::uint64_t>(n));}
    void text(const std::string& s){cleanText(s);data.push_back(2);raw(s.size());data+=s;require(data.size()<=maxManifest,"task manifest too large");}
    void list(const std::vector<std::string>& values){data.push_back(3);raw(values.size());for(const auto& v:values)text(v);}
};
struct Decoder {
    const std::string& data;std::size_t pos=8;
    explicit Decoder(const std::string& bytes):data(bytes){require(bytes.size()>=8 && bytes.compare(0,8,std::string("ELSCAN\0\1",8))==0,"invalid task format magic");}
    std::uint64_t raw(){require(data.size()-pos>=8,"truncated task manifest");std::uint64_t n=0;for(int i=0;i<8;++i)n|=static_cast<std::uint64_t>(static_cast<unsigned char>(data[pos++]))<<(8*i);return n;}
    void tag(unsigned char t){require(pos<data.size() && static_cast<unsigned char>(data[pos++])==t,"invalid task field type");}
    std::int64_t number(){tag(1);auto n=raw();require(n<=static_cast<std::uint64_t>(INT64_MAX),"invalid task integer");return static_cast<std::int64_t>(n);}
    std::string text(){tag(2);auto n=raw();require(n<=maxText && n<=data.size()-pos,"invalid task text length");auto value=data.substr(pos,static_cast<std::size_t>(n));pos+=static_cast<std::size_t>(n);cleanText(value);return value;}
    std::vector<std::string> list(){tag(3);auto n=raw();require(n<=maxEntries,"invalid task path count");std::vector<std::string> values;for(std::uint64_t i=0;i<n;++i)values.push_back(text());return values;}
};
#if defined(__APPLE__)
struct Fd {int value=-1;explicit Fd(int v=-1):value(v){}~Fd(){if(value>=0)close(value);}Fd(const Fd&)=delete;Fd& operator=(const Fd&)=delete;};
bool same(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev && a.st_ino==b.st_ino && a.st_birthtimespec.tv_sec==b.st_birthtimespec.tv_sec && a.st_birthtimespec.tv_nsec==b.st_birthtimespec.tv_nsec;}
// Held directory/file identities are rechecked before and after IO. This matches
// SourceScan's private-output contract, not an atomic hostile-same-UID guarantee
// or a general ACL audit. Ancestor aliases are resolved by F_GETPATH.
struct PrivateFile {
    Fd parent,file;struct stat directory{},leaf_stat{};std::string parent_path,leaf;
    PrivateFile(const std::string& path,bool create,bool lock=false,const std::string& captured_root={}) {
        require(absolute(path),"private task path must be normalized and absolute");
        parent_path=std::filesystem::path(path).parent_path().string();leaf=std::filesystem::path(path).filename().string();
        parent.value=open(parent_path.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        require(parent.value>=0 && fstat(parent.value,&directory)==0,"unable to hold task parent");
        require(directory.st_uid==geteuid() && (directory.st_mode&07777)==0700,"task parent requires caller ownership and mode 0700");
        char canonical[MAXPATHLEN]{};require(fcntl(parent.value,F_GETPATH,canonical)==0,"unable to resolve task parent");parent_path=canonical;
        if(!captured_root.empty())outside(captured_root);
        require(parentUnchanged(),"task parent changed before open");
        if(create || lock) {
            file.value=openat(parent.value,leaf.c_str(),O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
            if(file.value>=0)require(fchmod(file.value,0600)==0,"unable to secure task file");
            else if(lock && errno==EEXIST)file.value=openat(parent.value,leaf.c_str(),O_RDWR|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        } else file.value=openat(parent.value,leaf.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        require(file.value>=0 && fstat(file.value,&leaf_stat)==0,"private task leaf exists or cannot be opened");
        require(unchanged(),"task file requires held regular inode, caller ownership and mode 0600");
        require(leaf_stat.st_size>=0 && (lock?leaf_stat.st_size==0:static_cast<std::uint64_t>(leaf_stat.st_size)<=maxManifest),"invalid task file size");
    }
    bool parentUnchanged() const {struct stat held{},current{};return fstat(parent.value,&held)==0 && lstat(parent_path.c_str(),&current)==0 && S_ISDIR(current.st_mode) && same(directory,held) && same(directory,current) && current.st_uid==geteuid() && (current.st_mode&07777)==0700;}
    bool unchanged() const {struct stat held{},current{};return parentUnchanged() && fstat(file.value,&held)==0 && fstatat(parent.value,leaf.c_str(),&current,AT_SYMLINK_NOFOLLOW)==0 && S_ISREG(current.st_mode) && same(leaf_stat,held) && same(leaf_stat,current) && current.st_uid==geteuid() && (current.st_mode&07777)==0600 && current.st_nlink==1;}
    void outside(const std::string& captured_root) const {
        // Use held PRIVATE parent evidence and captured source evidence only.
        // Never realpath/stat/open a potentially unavailable source before flock.
        require(absolute(captured_root) && !pathIsWithin(parent_path+"/"+leaf,captured_root),
            "private task file cannot be inside captured canonical source root");
    }
};
#endif
}
void validateSourceScanRequest(const SourceScanRequest& request) { validateRequest(request); }
std::array<std::string, 7> capturedSourceFingerprintFields(const std::string& fingerprint) {
    const auto fields=evidenceFields(fingerprint,7);mountFields(fields);
    require(absolute(fields[5]) && !fields[6].empty(),"invalid captured canonical source evidence");
    std::array<std::string, 7> out;std::copy(fields.begin(),fields.end(),out.begin());return out;
}
std::string capturedSourceCanonicalRoot(const std::string& fingerprint) {
    return capturedSourceFingerprintFields(fingerprint)[5];
}
std::vector<std::string> capturedChildMountRoots(const std::string& fingerprint,const std::string& canonical_root) {
    cleanText(fingerprint);require(absolute(canonical_root),"invalid captured canonical root");
    std::size_t offset=0;std::vector<std::string> roots;std::string previous;
    while(offset<fingerprint.size()) {
        require(roots.size()<maxEntries,"too many captured child mounts");
        const auto proof=evidenceField(fingerprint,offset);
        require(previous.empty() || previous<=proof,"unordered captured child mount evidence");previous=proof;
        const auto fields=evidenceFields(proof,2);
        require(absolute(fields[0]) && fields[0]!=canonical_root && pathIsWithin(fields[0],canonical_root),"child mount outside captured root");
        mountFields(evidenceFields(fields[1],5),true);roots.push_back(fields[0]);
    }
    std::sort(roots.begin(),roots.end());roots.erase(std::unique(roots.begin(),roots.end()),roots.end());return roots;
}
ScanTask::ScanTask(SourceScanRequest request,std::string lock):scan(std::move(request)),lock_path(std::move(lock)){validate(*this);}
void writeScanTask(const std::string& path,const ScanTask& task) {
#if defined(__APPLE__)
    validate(task);Encoder out;const auto& r=task.scan;const auto& s=r.source;
    out.number(1);out.number(1);out.number(1); // version, one metadata, one source
    out.number(s.id);out.text(s.label);out.number(static_cast<int>(s.type));out.number(static_cast<int>(s.mode));out.number(static_cast<int>(s.status));out.number(s.interval_seconds);
    out.text(s.identity);out.text(s.root);out.text(s.mount_fingerprint);out.number(s.last_check);out.number(s.last_success);out.number(s.revision);out.number(s.policy_revision);
    out.number(r.policy.revision);out.list(r.policy.rules().directories);out.list(r.policy.rules().files);out.text(r.policy.rules().folder_filter);out.text(r.policy.rules().file_filter);
    out.list(r.nested_source_roots);out.text(r.scan_path);out.text(r.snapshot_path);out.text(r.nonce);out.text(task.lock_path);
    require(!pathIsWithin(path,s.root),"task manifest cannot be inside source root");
    PrivateFile file(path,true,false,capturedSourceCanonicalRoot(s.mount_fingerprint));
    std::size_t offset=0;while(offset<out.data.size()){auto n=write(file.file.value,out.data.data()+offset,out.data.size()-offset);if(n<0 && errno==EINTR)continue;require(n>0,"task manifest write failed");offset+=static_cast<std::size_t>(n);}
    require(fsync(file.file.value)==0 && file.unchanged(),"task manifest changed during write");
#else
    (void)path;(void)task;throw std::runtime_error("native scan task backend unsupported");
#endif
}
ScanTask readScanTask(const std::string& path) {
#if defined(__APPLE__)
    PrivateFile file(path,false);std::string bytes(static_cast<std::size_t>(file.leaf_stat.st_size),'\0');std::size_t offset=0;
    while(offset<bytes.size()){auto n=read(file.file.value,bytes.data()+offset,bytes.size()-offset);if(n<0 && errno==EINTR)continue;require(n>0,"truncated task file");offset+=static_cast<std::size_t>(n);}
    char extra;auto n=read(file.file.value,&extra,1);struct stat end{};
    require(n==0 && fstat(file.file.value,&end)==0 && end.st_size==file.leaf_stat.st_size && end.st_mtimespec.tv_sec==file.leaf_stat.st_mtimespec.tv_sec && end.st_mtimespec.tv_nsec==file.leaf_stat.st_mtimespec.tv_nsec && end.st_ctimespec.tv_sec==file.leaf_stat.st_ctimespec.tv_sec && end.st_ctimespec.tv_nsec==file.leaf_stat.st_ctimespec.tv_nsec && file.unchanged(),"task file changed during read");
    Decoder in(bytes);require(in.number()==1 && in.number()==1 && in.number()==1,"invalid task version or singleton count");
    SourceRecord s;s.id=in.number();s.label=in.text();
    auto type=in.number(),mode=in.number(),status=in.number();require(type<=static_cast<int>(SourceType::Unknown) && mode<=static_cast<int>(IndexMode::Periodic) && status<=static_cast<int>(SourceStatus::Failed),"invalid task source enum");
    s.type=static_cast<SourceType>(type);s.mode=static_cast<IndexMode>(mode);s.status=static_cast<SourceStatus>(status);s.interval_seconds=in.number();s.identity=in.text();s.root=in.text();s.mount_fingerprint=in.text();s.last_check=in.number();s.last_success=in.number();s.revision=in.number();s.policy_revision=in.number();
    auto revision=in.number();ExclusionRules rules;rules.directories=in.list();rules.files=in.list();rules.folder_filter=in.text();rules.file_filter=in.text();
    paths(rules.directories);paths(rules.files); // validate before policy normalization
    auto nested=in.list();auto scope=in.text(),snapshot=in.text(),nonce=in.text(),lock=in.text();require(in.pos==bytes.size(),"trailing task data");
    require(!pathIsWithin(path,s.root),"task manifest cannot be inside source root");file.outside(capturedSourceCanonicalRoot(s.mount_fingerprint));
    return ScanTask({std::move(s),ExclusionPolicy(std::move(rules),revision),std::move(nested),std::move(scope),std::move(snapshot),std::move(nonce)},std::move(lock));
#else
    (void)path;throw std::runtime_error("native scan task backend unsupported");
#endif
}
SourceScanLock::SourceScanLock(const ScanTask& task) {
#if defined(__APPLE__)
    validate(task);PrivateFile file(task.lock_path,false,true,capturedSourceCanonicalRoot(task.scan.source.mount_fingerprint));
    require(flock(file.file.value,LOCK_EX|LOCK_NB)==0,"source scan lock is busy or unavailable");
    require(file.unchanged(),"source scan lock changed during acquisition");fd_=file.file.value;file.file.value=-1;
#else
    (void)task;throw std::runtime_error("native source scan lock backend unsupported");
#endif
}
SourceScanLock::~SourceScanLock(){
#if defined(__APPLE__)
    if(fd_>=0)close(fd_);
#endif
}
// No unlink, no unlock, no early close: flock is advisory and only protects
// cooperating processes using this persistent path. OS exit owns the descriptor.
void SourceScanLock::releaseToProcess() noexcept {fd_=-1;}
} // namespace everything_lite
