#if defined(_WIN32)
#include "core/scanner.h"
#include "core/path_utils.h"

#include <chrono>
#include <filesystem>

namespace everything_lite {
namespace fs = std::filesystem;

namespace {
bool appBundleName(const std::string& name) {
    const auto folded=asciiFold(name);
    return folded.size()>=4 && folded.compare(folded.size()-4,4,".app")==0;
}
std::int64_t toUnixSeconds(const fs::file_time_type& value) {
    using namespace std::chrono;
    const auto system_time = time_point_cast<system_clock::duration>(
        value - fs::file_time_type::clock::now() + system_clock::now());
    return duration_cast<seconds>(system_time.time_since_epoch()).count();
}
}

FileScanner::FileScanner(std::size_t batch_size) : batch_size_(batch_size) {}

bool FileScanner::makeRecord(const fs::path& path, FileRecord& record) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec) {
        return false;
    }

    record.path = normalizePath(pathToUtf8(path));
    record.name = pathToUtf8(path.filename());
    if (record.name.empty()) {
        record.name = record.path;
    }
    record.parent_path = normalizePath(pathToUtf8(path.parent_path()));
    auto ext = pathToUtf8(path.extension());
    if (!ext.empty() && ext.front() == '.') {
        ext.erase(ext.begin());
    }
    record.extension = ext;
    record.is_directory = fs::is_directory(status);
    record.size = 0;
    if (fs::is_regular_file(status)) {
        const auto size = fs::file_size(path, ec);
        if (ec) return false;
        record.size = static_cast<std::uint64_t>(size);
    }
    const auto modified = fs::last_write_time(path, ec);
    if (ec) {
        // A symlink may intentionally have no target; its own metadata remains
        // enumerable, while other metadata failures invalidate the snapshot.
        if (!fs::is_symlink(status)) return false;
        record.modified_time = 0;
    } else record.modified_time = toUnixSeconds(modified);
    return true;
}

bool FileScanner::readSingle(const std::string& path, FileRecord& record, const ExclusionPolicy* policy) {
    return makeRecord(utf8ToPath(path), record) && (!policy || !policy->excludes(record.path,record.is_directory));
}

IndexStats FileScanner::scanTree(const std::string& root,
                                 const BatchCallback& callback,
                                 const std::atomic_bool* cancelled,
                                 const ExclusionPolicy* policy,
                                 const ScanBoundary* boundary) const {
    IndexStats stats;
    if(boundary){stats.error="directory boundary backend unsupported";return stats;}
    std::vector<FileRecord> batch;
    batch.reserve(batch_size_);
    auto isCancelled = [&]() { return cancelled && cancelled->load(); };
    auto flush = [&]() -> bool {
        if (isCancelled()) { stats.cancelled = true; return false; }
        if (batch.empty()) return true;
        const auto size = batch.size();
        try {
            if (!callback(std::move(batch))) { stats.cancelled = true; return false; }
        } catch (const std::exception& error) {
            stats.error = error.what(); return false;
        } catch (...) {
            stats.error = "scan callback failed"; return false;
        }
        stats.indexed += size;
        batch.clear(); batch.reserve(batch_size_);
        return true;
    };
    if (isCancelled()) { stats.cancelled = true; return stats; }
    const auto root_path = utf8ToPath(root);
    std::error_code ec;
    const auto status = fs::status(root_path, ec);
    if (ec || !fs::exists(status)) {
        stats.error = ec ? ec.message() : "root does not exist";
        return stats;
    }
    // Exclusions prune descendants only after the scan scope itself exists.
    // An excluded ancestor cannot turn a missing source into an online snapshot.
    const auto parent=normalizePath(pathToUtf8(root_path.parent_path()));
    if(policy && (policy->excludes(parent,true) || policy->excludes(root,fs::is_directory(status)))) {
        stats.completed=true; return stats;
    }
    auto append = [&](const fs::path& path,bool is_directory) {
        if(policy && policy->excludes(pathToUtf8(path),is_directory)) { ++stats.skipped; return true; }
        ++stats.scanned;
        FileRecord record;
        if (!makeRecord(path, record)) {
            ++stats.skipped; stats.error = "unable to read file metadata";
            return false;
        }
        if(policy && policy->excludes(record.path,record.is_directory)) { ++stats.skipped; return true; }
        batch.push_back(std::move(record));
        return batch.size() < batch_size_ || flush();
    };
    if (!append(root_path,fs::is_directory(status))) return stats;
    if (fs::is_directory(status) && !(policy && !policy->indexAppBundleContents() &&
                                      appBundleName(pathToUtf8(root_path.filename())))) {
        // Never skip permission errors: an incomplete enumeration cannot be
        // used to infer deletions from a previously complete snapshot.
        fs::recursive_directory_iterator it(root_path, fs::directory_options::none, ec), end;
        if (ec) { stats.error = ec.message(); return stats; }
        while (it != end) {
            if (isCancelled()) { stats.cancelled = true; return stats; }
            const auto path=pathToUtf8(it->path());
            const auto entry_status=it->symlink_status(ec);
            if(ec) { stats.error=ec.message(); return stats; }
            const bool is_directory=fs::is_directory(entry_status);
            if(is_directory && policy && policy->excludes(path,true)) {
                it.disable_recursion_pending(); ++stats.skipped;
            } else {
                if (!append(it->path(),is_directory)) return stats;
                if (is_directory && policy && !policy->indexAppBundleContents() &&
                    appBundleName(pathToUtf8(it->path().filename()))) it.disable_recursion_pending();
            }
            it.increment(ec);
            if (ec) { stats.error = ec.message(); return stats; }
        }
    }
    if (!flush()) return stats;
    if (isCancelled()) { stats.cancelled = true; return stats; }
    stats.completed = true;
    return stats;
}

} // namespace everything_lite

#else
#include "core/scanner.h"
#include "core/path_utils.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/xattr.h>
#endif
namespace everything_lite {
namespace fs=std::filesystem;
namespace {
bool appBundleName(const std::string& name) {
    const auto folded=asciiFold(name);
    return folded.size()>=4 && folded.compare(folded.size()-4,4,".app")==0;
}
bool finderInvisible(const std::string& path,const struct stat& st) {
#if defined(__APPLE__)
    if ((st.st_flags & UF_HIDDEN) != 0) return true;
    unsigned char finder_info[32]{};
    const auto size=getxattr(path.c_str(),XATTR_FINDERINFO_NAME,finder_info,sizeof(finder_info),0,XATTR_NOFOLLOW);
    return size>=10 && (finder_info[8]&0x40)!=0; // Finder kIsInvisible = 0x4000.
#else
    (void)path;(void)st;
    return false;
#endif
}
bool finderInvisibleInPath(fs::path path) {
    for (;;) {
        struct stat st{};
        if(lstat(path.c_str(),&st)==0 && finderInvisible(pathToUtf8(path),st))return true;
        const auto parent=path.parent_path();
        if(parent==path || parent.empty())return false;
        path=parent;
    }
}
struct Descriptor {
    int fd;
    explicit Descriptor(int value):fd(value) {}
    ~Descriptor(){if(fd>=0)close(fd);}
    Descriptor(const Descriptor&)=delete;
    Descriptor& operator=(const Descriptor&)=delete;
};
bool sameDirectory(const struct stat& a,const struct stat& b) {
    if(a.st_dev!=b.st_dev || a.st_ino!=b.st_ino)return false;
#if defined(__APPLE__)
    return a.st_birthtimespec.tv_sec==b.st_birthtimespec.tv_sec && a.st_birthtimespec.tv_nsec==b.st_birthtimespec.tv_nsec;
#else
    return true;
#endif
}
void fromStat(const fs::path& path,const struct stat& st,FileRecord& r,bool finder_hidden) {
    r.path=normalizePath(pathToUtf8(path));r.parent_path=normalizePath(pathToUtf8(path.parent_path()));
    r.name=pathToUtf8(path.filename());if(r.name.empty())r.name=r.path;
    r.extension=pathToUtf8(path.extension());if(!r.extension.empty() && r.extension[0]=='.')r.extension.erase(0,1);
    r.is_directory=S_ISDIR(st.st_mode);r.size=(S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))?static_cast<std::uint64_t>(st.st_size):0;
    // Link size is its own stored path-byte length, not target file size.
    // POSIX epoch seconds, read from the object itself, including broken links.
    r.modified_time=static_cast<std::int64_t>(st.st_mtime);
    r.finder_hidden=finder_hidden;
}
}
FileScanner::FileScanner(std::size_t n):batch_size_(std::max<std::size_t>(1,n)) {}
bool FileScanner::makeRecord(const fs::path& path,FileRecord& r) {
    struct stat st{};if(lstat(path.c_str(),&st)!=0)return false;fromStat(path,st,r,finderInvisibleInPath(path));return true;
}
bool FileScanner::readSingle(const std::string& path,FileRecord& r,const ExclusionPolicy* policy) {
    if(path.find('\0')!=std::string::npos)return false;
    return makeRecord(utf8ToPath(path),r) && (!policy || !policy->excludes(r.path,r.is_directory));
}
IndexStats FileScanner::scanTree(const std::string& root,const BatchCallback& callback,
    const std::atomic_bool* cancelled,const ExclusionPolicy* policy,const ScanBoundary* boundary) const {
    IndexStats stats;std::vector<FileRecord> batch;batch.reserve(batch_size_);
    auto stopped=[&] {if(cancelled && cancelled->load()){stats.cancelled=true;return true;}return false;};
    auto fail=[&](const char* message){stats.error=message;return false;};
    auto flush=[&] {
        if(stopped())return false;if(batch.empty())return true;
        const auto count=batch.size();
        if(!callback(std::move(batch))){stats.cancelled=true;return false;}
        stats.indexed+=count;batch.clear();batch.reserve(batch_size_);return true;
    };
    const auto scope=normalizePath(root);
    auto prune=[&](const std::string& path) {
        if(boundary)for(const auto& candidate:boundary->prune_roots)
            if(normalizePath(candidate)!=scope && pathIsWithin(path,candidate))return true;
        return false;
    };
    auto metadata=[&](const std::string& path) {
        return !boundary || !boundary->metadata_guard || boundary->metadata_guard(path);
    };
    auto enter=[&](const std::string& path,int fd) {
        return boundary && boundary->enter_directory ? boundary->enter_directory(path,fd):DirectoryDecision::Enter;
    };
    auto append=[&](const std::string& path,const struct stat& st,bool finder_hidden) {
        if(policy && policy->excludes(path,S_ISDIR(st.st_mode))){++stats.skipped;return true;}
        ++stats.scanned;FileRecord record;fromStat(utf8ToPath(path),st,record,finder_hidden);batch.push_back(std::move(record));
        return batch.size()<batch_size_ || flush();
    };
    try {
        if(stopped())return stats;
        if(root.empty() || root.find('\0')!=std::string::npos){fail("invalid scan root");return stats;}
        struct stat root_st{};
        if(!metadata(scope) || lstat(scope.c_str(),&root_st)!=0){fail("unable to read root metadata");return stats;}
        // Legacy scanTree(root) treats its root as a bound source directory.
        // A configured subtree contract opts out; descendant links never follow.
        const bool follow_root=S_ISLNK(root_st.st_mode) && (!boundary || boundary->allow_root_symlink);
        std::unique_ptr<Descriptor> root_fd;
        if(S_ISDIR(root_st.st_mode) || follow_root) {
            root_fd=std::make_unique<Descriptor>(open(scope.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|(follow_root?0:O_NOFOLLOW)));
            if(root_fd->fd<0 && follow_root && !boundary && errno==ENOTDIR) {
                // Legacy file-link scope: enumerate the link itself, without
                // obtaining its target's metadata or attempting file reads.
                root_fd.reset();
            } else {
                if(root_fd->fd<0 || fstat(root_fd->fd,&root_st)!=0){fail("unable to open scan directory");return stats;}
                if(enter(scope,root_fd->fd)!=DirectoryDecision::Enter){fail("scan root outside directory boundary");return stats;}
            }
        }
        if(policy && (policy->excludes(pathToUtf8(utf8ToPath(scope).parent_path()),true) || policy->excludes(scope,S_ISDIR(root_st.st_mode)))) {
            stats.completed=true;return stats;
        }
        const bool root_hidden=finderInvisibleInPath(utf8ToPath(scope));
        if(!append(scope,root_st,root_hidden))return stats;
        std::function<bool(int,const std::string&,bool)> walk=[&](int fd,const std::string& path,bool parent_hidden) {
            int copy=dup(fd);if(copy<0)return fail("unable to duplicate directory");
            DIR* opened=fdopendir(copy);if(!opened){close(copy);return fail("unable to enumerate directory");}
            std::unique_ptr<DIR,int(*)(DIR*)> directory(opened,closedir);
            for(;;) {
                if(stopped())return false;
                errno=0;dirent* entry=readdir(opened);
                if(!entry){if(errno)return fail("directory enumeration failed");break;}
                const std::string name=entry->d_name;if(name=="." || name=="..")continue;
                const auto child=path=="/"?"/"+name:path+"/"+name;
                // Whole subtree boundaries are applied before even metadata.
                if(prune(child) || (entry->d_type==DT_DIR && policy && policy->excludes(child,true))){++stats.skipped;continue;}
                struct stat st{};
                if(!metadata(child) || fstatat(fd,name.c_str(),&st,AT_SYMLINK_NOFOLLOW)!=0)return fail("unable to read file metadata");
                const bool child_hidden=parent_hidden || finderInvisible(child,st);
                if(S_ISDIR(st.st_mode)) {
                    if(policy && policy->excludes(child,true)){++stats.skipped;continue;}
                    Descriptor nested(openat(fd,name.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
                    if(nested.fd<0)return fail("unable to open child directory");
                    struct stat held{};if(fstat(nested.fd,&held)!=0 || !sameDirectory(st,held))return fail("directory changed before entry");
                    const auto decision=enter(child,nested.fd);
                    if(decision==DirectoryDecision::Prune){++stats.skipped;continue;}
                    if(decision==DirectoryDecision::Error)return fail("directory boundary check failed");
                    if(!append(child,st,child_hidden))return false;
                    if(!(policy && !policy->indexAppBundleContents() && appBundleName(name)) &&
                       !walk(nested.fd,child,child_hidden))return false;
                    // A held fd survives rename/replacement. Check the current
                    // entry again on exit, before accepting old-bound paths.
                    struct stat current{},after{};
                    if(fstatat(fd,name.c_str(),&current,AT_SYMLINK_NOFOLLOW)!=0 ||
                       fstat(nested.fd,&after)!=0 || !S_ISDIR(current.st_mode) ||
                       !sameDirectory(st,current) || !sameDirectory(st,after) ||
                       enter(child,nested.fd)!=DirectoryDecision::Enter)
                        return fail("directory binding changed during enumeration");
                } else if(!append(child,st,child_hidden))return false; // Descendant links never enter.
            }
            return true;
        };
        if(root_fd && !(policy && !policy->indexAppBundleContents() &&
                        appBundleName(pathToUtf8(utf8ToPath(scope).filename()))) &&
           !walk(root_fd->fd,scope,root_hidden))return stats;
        if(!flush() || stopped())return stats;stats.completed=true;
    } catch(const std::exception& error){stats.error=error.what();}
      catch(...){stats.error="scan callback or directory check failed";}
    return stats;
}
} // namespace everything_lite
#endif
