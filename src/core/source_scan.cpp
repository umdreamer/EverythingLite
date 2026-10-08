#include "core/source_scan.h"
#include "core/path_utils.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <sqlite3.h>
#if defined(__APPLE__)
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace everything_lite {
#if defined(__APPLE__)
namespace {
namespace fs=std::filesystem;
struct Fd {
    int value=-1;
    explicit Fd(int fd=-1):value(fd){}
    ~Fd(){if(value>=0)close(value);}
    Fd(const Fd&)=delete;
    Fd& operator=(const Fd&)=delete;
};
void require(bool ok,const char* error){if(!ok)throw std::runtime_error(error);}
bool validAbsolute(const std::string& path) {
    return !path.empty() && path.front()=='/' && path.find('\0')==std::string::npos && normalizePath(path)==path;
}
std::string relative(const std::string& path,const std::string& root) {
    require(pathIsWithin(path,root),"scan scope outside source binding");
    return path==root?std::string{}:path.substr(root=="/"?1:root.size()+1);
}
bool sameObject(const struct stat& a,const struct stat& b) {
    return a.st_dev==b.st_dev && a.st_ino==b.st_ino &&
        a.st_birthtimespec.tv_sec==b.st_birthtimespec.tv_sec && a.st_birthtimespec.tv_nsec==b.st_birthtimespec.tv_nsec;
}
bool sameMount(const struct statfs& a,const struct statfs& b) {
    // Current mount membership, not permanent identity; permanent UUID is only
    // established by SourceProbe at begin/end, never guessed per file.
    return a.f_fsid.val[0]==b.f_fsid.val[0] && a.f_fsid.val[1]==b.f_fsid.val[1] &&
        std::strcmp(a.f_fstypename,b.f_fstypename)==0 && std::strcmp(a.f_mntonname,b.f_mntonname)==0 &&
        (a.f_flags&MNT_LOCAL)==(b.f_flags&MNT_LOCAL);
}
struct Scope {
    Fd source;
    Fd directory;
    struct stat initial{};
    struct statfs mount{};
    std::string root,path,subtree;
    Scope(const std::string& binding,const std::string& scope):root(binding),path(scope),subtree(relative(scope,binding)) {
        source.value=open(binding.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        require(source.value>=0 && fstatfs(source.value,&mount)==0,"unable to hold source directory");
        if(subtree.empty()) {
            directory.value=dup(source.value);
            require(directory.value>=0 && fstat(directory.value,&initial)==0,"unable to hold scan root");
        } else {
            Fd parent(dup(source.value));require(parent.value>=0,"unable to hold subtree parent");
            const auto components=fs::path(subtree);
            for(auto it=components.begin();it!=components.end();++it) {
                auto next=it;++next;const auto name=it->string();
                if(next==components.end()) {
                    require(fstatat(parent.value,name.c_str(),&initial,AT_SYMLINK_NOFOLLOW)==0,"unable to read scan scope");
                    if(S_ISDIR(initial.st_mode)) directory.value=openat(parent.value,name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
                } else {
                    const int child=openat(parent.value,name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
                    require(child>=0,"subtree ancestor is missing, inaccessible or a symlink");
                    close(parent.value);parent.value=child;
                    struct stat st{};struct statfs volume{};
                    require(fstat(child,&st)==0 && fstatfs(child,&volume)==0 && sameMount(mount,volume),"subtree crosses a foreign filesystem");
                }
            }
            require(!S_ISDIR(initial.st_mode) || directory.value>=0,"unable to hold subtree directory");
            if(directory.value>=0) {
                struct stat held{};require(fstat(directory.value,&held)==0 && sameObject(initial,held),"subtree changed during open");
            }
        }
    }
    bool unchanged() const {
        // Fresh nofollow walk repeats the same scope contract. An fd alone can
        // remain usable after rename/replacement and is not sufficient proof.
        Scope fresh(root,path);
        if(!sameObject(initial,fresh.initial) || !sameMount(mount,fresh.mount))return false;
        if(directory.value>=0) {
            struct stat held{};char current[MAXPATHLEN]{},old[MAXPATHLEN]{};
            if(fstat(directory.value,&held)!=0 || !sameObject(initial,held) ||
               fcntl(directory.value,F_GETPATH,old)!=0 || fcntl(fresh.directory.value,F_GETPATH,current)!=0 ||
               std::strcmp(old,current)!=0)return false;
        }
        return true;
    }
};
struct Snapshot {
    Fd parent,file;
    struct stat parent_stat{},file_stat{};
    std::string parent_path,path,leaf;
    sqlite3* db=nullptr;
    explicit Snapshot(const std::string& output,const std::string& scope) {
        require(validAbsolute(output),"snapshot path must be a normalized absolute path");
        parent_path=fs::path(output).parent_path().string();leaf=fs::path(output).filename().string();
        require(!leaf.empty() && leaf!="." && leaf!="..","invalid snapshot leaf");
        parent.value=open(parent_path.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        require(parent.value>=0 && fstat(parent.value,&parent_stat)==0,"unable to open private snapshot parent");
        require(parent_stat.st_uid==geteuid() && (parent_stat.st_mode&07777)==0700,"snapshot parent must be owned by caller and mode 0700");
        char held[MAXPATHLEN]{};require(fcntl(parent.value,F_GETPATH,held)==0,"unable to resolve held snapshot parent");
        parent_path=held;path=parent_path+"/"+leaf;
        char* actual=realpath(scope.c_str(),nullptr);require(actual!=nullptr,"unable to resolve actual scan scope");
        const std::string canonical_scope=actual;free(actual);
        require(!pathIsWithin(path,canonical_scope) && !pathIsWithin(output,scope),"snapshot output cannot be inside scan scope");
        require(parentUnchanged(),"snapshot parent changed before create");
        file.value=openat(parent.value,leaf.c_str(),O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        require(file.value>=0,"snapshot leaf already exists or cannot be created");
        require(fchmod(file.value,0600)==0 && fstat(file.value,&file_stat)==0,"unable to secure snapshot file");
        require(unchanged(),"snapshot path changed before SQLite open");
        // SQLite opens the held parent's canonical path. Recheck inode before
        // every batch and handoff. This does not claim atomic resistance to a
        // hostile process with this same UID or privileged access. Ownership
        // and POSIX mode checks are required, not a general ACL security audit.
        const int rc=sqlite3_open_v2(path.c_str(),&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_FULLMUTEX,nullptr);
        if(rc!=SQLITE_OK){if(db)sqlite3_close(db);db=nullptr;throw std::runtime_error("unable to open snapshot SQLite");}
    }
    ~Snapshot(){if(db)sqlite3_close(db);}
    bool parentUnchanged() const {
        struct stat held{},current{};
        return fstat(parent.value,&held)==0 && lstat(parent_path.c_str(),&current)==0 &&
            S_ISDIR(current.st_mode) && sameObject(parent_stat,held) && sameObject(parent_stat,current) &&
            current.st_uid==geteuid() && (current.st_mode&07777)==0700;
    }
    bool unchanged() const {
        struct stat held{},current{};
        return parentUnchanged() && fstat(file.value,&held)==0 &&
            fstatat(parent.value,leaf.c_str(),&current,AT_SYMLINK_NOFOLLOW)==0 &&
            S_ISREG(current.st_mode) && sameObject(file_stat,held) && sameObject(file_stat,current) &&
            current.st_uid==geteuid() && (current.st_mode&07777)==0600;
    }
    void exec(const char* sql) {
        require(sqlite3_exec(db,sql,nullptr,nullptr,nullptr)==SQLITE_OK,"snapshot SQLite write failed");
    }
    void closeWriter() {
        auto* handle=db;db=nullptr;require(sqlite3_close(handle)==SQLITE_OK,"snapshot writer failed to close");
    }
};
struct Statement {
    sqlite3_stmt* value=nullptr;
    Statement(sqlite3* db,const char* sql){require(sqlite3_prepare_v2(db,sql,-1,&value,nullptr)==SQLITE_OK,"snapshot statement failed");}
    ~Statement(){if(value)sqlite3_finalize(value);}
};
void text(sqlite3_stmt* stmt,int i,const std::string& value) {
    require(value.size()<=static_cast<std::size_t>(INT_MAX),"snapshot text too large");
    require(sqlite3_bind_text(stmt,i,value.data(),static_cast<int>(value.size()),SQLITE_TRANSIENT)==SQLITE_OK,"snapshot text binding failed");
}
void integer(sqlite3_stmt* stmt,int i,std::int64_t value){require(sqlite3_bind_int64(stmt,i,value)==SQLITE_OK,"snapshot integer binding failed");}
void done(sqlite3_stmt* stmt){require(sqlite3_step(stmt)==SQLITE_DONE,"snapshot row write failed");sqlite3_reset(stmt);sqlite3_clear_bindings(stmt);}
}
#endif
SourceScanResult scanSourceToSnapshot(SourceScanRequest request,const std::atomic_bool* cancelled,const SourceScanProgress& progress) {
    SourceScanResult result;
#if defined(__APPLE__)
    try {
        const auto& source=request.source;
        validateSource(source);
        require(source.id>0 && source.status==SourceStatus::Accessible &&
            !source.identity.empty() && !source.mount_fingerprint.empty(),"source requires explicit verified evidence");
        require(source.policy_revision>=0 && source.policy_revision==request.policy.revision,"captured policy revision mismatch");
        require(validAbsolute(source.root),"source binding must be a normalized absolute path");
        result.actual_scan_path=request.scan_path.empty()?source.root:request.scan_path;
        require(validAbsolute(result.actual_scan_path),"scan scope must be a normalized absolute path");
        result.subtree_relative=relative(result.actual_scan_path,source.root);
        require(!request.nonce.empty() && request.nonce.find('\0')==std::string::npos,"snapshot nonce required");
        if(cancelled && cancelled->load()){result.stats.cancelled=true;return result;}
        std::vector<std::string> nested;
        for(const auto& root:request.nested_source_roots) {
            require(validAbsolute(root),"nested source binding must be a normalized absolute path");
            if(root!=source.root && pathIsWithin(root,source.root))nested.push_back(root);
        }
        std::sort(nested.begin(),nested.end());nested.erase(std::unique(nested.begin(),nested.end()),nested.end());
        for(const auto& root:nested)require(!pathIsWithin(result.actual_scan_path,root),"scan scope belongs to nested source");
        auto probe=beginSourceProbe(source);result.before=probe.result();
        require(result.before.update_allowed && result.before.identity==source.identity &&
            result.before.mount_fingerprint==source.mount_fingerprint,"source evidence does not authorize scan");
        Scope scope(source.root,result.actual_scan_path);
        ScanBoundary boundary;boundary.prune_roots=nested;boundary.allow_root_symlink=result.subtree_relative.empty();
        // Probe boundaries are canonical; records keep the captured binding.
        for(const auto& child:result.before.child_mount_boundaries) {
            require(pathIsWithin(child,result.before.canonical_root),"invalid probed child boundary");
            auto suffix=relative(child,result.before.canonical_root);
            boundary.prune_roots.push_back(source.root=="/"?"/"+suffix:source.root+"/"+suffix);
        }
        boundary.enter_directory=[&](const std::string& path,int fd) {
            struct stat st{};struct statfs mount{};
            if(fstat(fd,&st)!=0 || fstatfs(fd,&mount)!=0)return DirectoryDecision::Error;
            if(path==result.actual_scan_path && !sameObject(scope.initial,st))return DirectoryDecision::Error;
            if(st.st_dev!=scope.initial.st_dev || !sameMount(scope.mount,mount))return DirectoryDecision::Prune;
            return DirectoryDecision::Enter;
        };
        // Reject output against the entire owning root, even for a subtree scan.
        Snapshot snapshot(request.snapshot_path,source.root);
        snapshot.exec("PRAGMA journal_mode=DELETE; PRAGMA cache_size=-2048; PRAGMA temp_store=FILE; PRAGMA mmap_size=0; PRAGMA synchronous=FULL;");
        snapshot.exec("CREATE TABLE snapshot_metadata(complete INTEGER NOT NULL CHECK(complete IN(0,1))); INSERT INTO snapshot_metadata VALUES(0);"
            "CREATE TABLE snapshot_files(path TEXT NOT NULL,parent_path TEXT NOT NULL,name TEXT NOT NULL,ext TEXT NOT NULL,root TEXT NOT NULL,size INTEGER NOT NULL,modified_time INTEGER NOT NULL,is_dir INTEGER NOT NULL,scan_generation INTEGER NOT NULL,source_id INTEGER NOT NULL,relative_path TEXT NOT NULL UNIQUE,cloud_copy INTEGER NOT NULL);"
            "CREATE TABLE snapshot_proof(nonce TEXT NOT NULL,source_id INTEGER NOT NULL,source_revision INTEGER NOT NULL,policy_revision INTEGER NOT NULL,binding TEXT NOT NULL,identity TEXT NOT NULL,mount_fingerprint TEXT NOT NULL,actual_scope TEXT NOT NULL,canonical_root TEXT NOT NULL,boundary_fingerprint TEXT NOT NULL,subtree_relative TEXT NOT NULL);"
            "CREATE TABLE snapshot_nested_roots(root TEXT PRIMARY KEY); CREATE TABLE snapshot_child_mounts(root TEXT PRIMARY KEY);");
        {
            Statement proof(snapshot.db,"INSERT INTO snapshot_proof VALUES(?,?,?,?,?,?,?,?,?,?,?)");
            text(proof.value,1,request.nonce);integer(proof.value,2,source.id);integer(proof.value,3,source.revision);integer(proof.value,4,source.policy_revision);
            text(proof.value,5,source.root);text(proof.value,6,source.identity);text(proof.value,7,source.mount_fingerprint);
            text(proof.value,8,result.actual_scan_path);text(proof.value,9,result.before.canonical_root);text(proof.value,10,result.before.boundary_fingerprint);text(proof.value,11,result.subtree_relative);done(proof.value);
            Statement roots(snapshot.db,"INSERT INTO snapshot_nested_roots VALUES(?)");for(const auto& root:nested){text(roots.value,1,root);done(roots.value);}
            Statement mounts(snapshot.db,"INSERT INTO snapshot_child_mounts VALUES(?)");for(const auto& root:result.before.child_mount_boundaries){text(mounts.value,1,root);done(mounts.value);}
        }
        {
            Statement insert(snapshot.db,"INSERT INTO snapshot_files VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
            result.stats=FileScanner(1000).scanTree(result.actual_scan_path,[&](std::vector<FileRecord>&& rows) {
                require(rows.size()<=1000 && snapshot.unchanged(),"snapshot output changed during scan");
                snapshot.exec("BEGIN");
                for(auto& row:rows) {
                    row.source_id=source.id;row.relative_path=relative(row.path,source.root);if(row.relative_path.empty())row.relative_path=".";row.root=source.root;
                    text(insert.value,1,row.path);text(insert.value,2,row.parent_path);text(insert.value,3,row.name);text(insert.value,4,row.extension);text(insert.value,5,row.root);
                    require(row.size<=static_cast<std::uint64_t>(INT64_MAX),"snapshot file size out of range");
                    integer(insert.value,6,static_cast<std::int64_t>(row.size));integer(insert.value,7,row.modified_time);integer(insert.value,8,row.is_directory);integer(insert.value,9,0);integer(insert.value,10,source.id);text(insert.value,11,row.relative_path);integer(insert.value,12,static_cast<int>(CloudCopy::Unknown));done(insert.value);
                }
                snapshot.exec("COMMIT");result.staged+=rows.size();++result.batches;result.max_batch=std::max(result.max_batch,rows.size());
                if(progress)progress(result.staged);
                return !(cancelled && cancelled->load());
            },cancelled,&request.policy,&boundary);
        }
        result.stats.indexed=0;
        result.after=probe.finish();
        const bool scanned=result.stats.completed;result.stats.completed=false;
        if(!scanned)return result;
        require(scope.unchanged(),"scan scope changed before completion");
        require(result.after.update_allowed && result.after.identity==result.before.identity &&
            result.after.mount_fingerprint==result.before.mount_fingerprint &&
            result.after.canonical_root==result.before.canonical_root &&
            result.after.boundary_fingerprint==result.before.boundary_fingerprint &&
            result.after.child_mount_boundaries==result.before.child_mount_boundaries,"source evidence changed before completion");
        require(snapshot.unchanged(),"snapshot output changed before completion");
        if(cancelled && cancelled->load()){result.stats.cancelled=true;return result;}
        snapshot.exec("UPDATE snapshot_metadata SET complete=1");
        snapshot.closeWriter();require(snapshot.unchanged(),"snapshot output changed before handoff");
        result.snapshot_path=snapshot.path;result.stats.completed=true;
    } catch(const std::exception& error){result.stats.completed=false;result.stats.indexed=0;result.stats.error=error.what();}
      catch(...){result.stats.completed=false;result.stats.indexed=0;result.stats.error="source scan failed";}
#else
    (void)request;(void)cancelled;(void)progress;result.stats.error="native source scan backend unsupported";
#endif
    return result;
}
} // namespace everything_lite
