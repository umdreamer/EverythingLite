#include "core/index_storage.h"
#include "core/database.h"
#include "core/directory_storage.h"
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <set>
#include <sstream>
#include <cerrno>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/file.h>
#endif

namespace everything_lite {
namespace {
namespace fs=std::filesystem;
using Connection=std::unique_ptr<sqlite3,decltype(&sqlite3_close)>;
using Statement=std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)>;
struct SourceIdentity {
    fs::path location;
#ifndef _WIN32
    struct stat info{};
#endif
};
SourceIdentity sourceIdentity(const fs::path& source);
void unchangedSource(const SourceIdentity& expected,const fs::path& source);
void writeBackupProvenance(const fs::path& source,const fs::path& backup,const SourceIdentity& expected);
bool backupProvenanceMatches(const fs::path& source,const fs::path& backup);
fs::path provenancePath(const fs::path& backup) {return fs::path(backup.u8string()+".provenance");}

[[noreturn]] void fail(const char* reason) { throw std::runtime_error(reason); }
void cancelled(std::atomic_bool* cancel) {
    if(cancel && cancel->load()) fail("storage: cancelled; keep any completed backup");
}
void check(int rc, std::atomic_bool* cancel=nullptr) {
    cancelled(cancel);
    if(rc==SQLITE_OK || rc==SQLITE_DONE || rc==SQLITE_ROW) return;
    if((rc&0xff)==SQLITE_BUSY || (rc&0xff)==SQLITE_LOCKED)
        fail("storage: database busy or locked; retry after other connections finish");
    if((rc&0xff)==SQLITE_INTERRUPT) fail("storage: cancelled; keep any completed backup");
    throw std::runtime_error("storage: SQLite operation failed (code "+std::to_string(rc)+"); keep any completed backup");
}
std::uint64_t add(std::uint64_t a,std::uint64_t b) {
    if(b>std::numeric_limits<std::uint64_t>::max()-a) fail("storage: size overflow");
    return a+b;
}
std::uint64_t multiply(std::uint64_t a,std::uint64_t b) {
    if(a && b>std::numeric_limits<std::uint64_t>::max()/a) fail("storage: size overflow");
    return a*b;
}
fs::path absolutePath(const std::string& value) {
    if(value.empty() || value.find('\0')!=std::string::npos) fail("storage: invalid path");
    std::error_code ec;
    auto p=fs::absolute(fs::path(value),ec);
    if(ec) fail("storage: invalid path");
    // An absolute filesystem path cannot be interpreted as a file: URI.
    return p;
}
fs::path canonicalFileLocation(const fs::path& path) {
    std::error_code ec;
    const auto parent=fs::canonical(path.parent_path(),ec);
    if(ec) fail("storage: cannot inspect storage directory");
    return parent/path.filename();
}
bool sqliteSidecarName(const fs::path& path) {
    auto name=path.filename().u8string();
    for(char& c:name) if(c>='A' && c<='Z') c=static_cast<char>(c-'A'+'a');
    for(const std::string suffix:{"-journal","-wal","-shm"}) {
        if(name.size()>=suffix.size() && name.compare(name.size()-suffix.size(),suffix.size(),suffix)==0) return true;
    }
    return false;
}
void rejectReservedBackup(const fs::path& source,const fs::path& backup) {
    // Conservative maintenance naming rule: case-insensitive filesystems can
    // alias any basename. Also reject a sidecar-named source: opening the
    // backup could otherwise treat that source as the backup's own journal.
    if(sqliteSidecarName(source) || sqliteSidecarName(backup))
        fail("storage: maintenance paths must not use SQLite sidecar suffixes");
    const auto canonical_source=canonicalFileLocation(source);
    const auto canonical_backup=canonicalFileLocation(backup);
    if(canonical_backup==canonical_source)
        fail("storage: backup path conflicts with database storage files");
    for(const char* suffix:{"-journal","-wal","-shm"}) {
        if(canonical_backup==fs::path(canonical_source.u8string()+suffix))
            fail("storage: backup path conflicts with database storage files");
    }
}
void regular(const fs::path& path) {
    std::error_code ec;
    const auto status=fs::symlink_status(path,ec);
    if(ec || !fs::is_regular_file(status)) fail("storage: database must be an existing regular file");
}
SourceIdentity sourceIdentity(const fs::path& source) {
    SourceIdentity identity;
    identity.location=canonicalFileLocation(source);
#ifndef _WIN32
    if(::lstat(identity.location.c_str(),&identity.info)!=0 || !S_ISREG(identity.info.st_mode))
        fail("storage: cannot inspect source identity");
#endif
    return identity;
}
void unchangedSource(const SourceIdentity& expected,const fs::path& source) {
#ifndef _WIN32
    const auto actual=sourceIdentity(source);
    if(actual.location!=expected.location || actual.info.st_dev!=expected.info.st_dev ||
       actual.info.st_ino!=expected.info.st_ino
#if defined(__APPLE__)
       || actual.info.st_birthtimespec.tv_sec!=expected.info.st_birthtimespec.tv_sec ||
       actual.info.st_birthtimespec.tv_nsec!=expected.info.st_birthtimespec.tv_nsec
#endif
    )fail("storage: source identity changed during backup; keep the unclaimed backup");
#else
    (void)expected;(void)source;
#endif
}
std::uint64_t fileBytes(const fs::path& path,bool optional=false) {
    std::error_code ec;
    const auto status=fs::symlink_status(path,ec);
    if(optional && (status.type()==fs::file_type::not_found || ec==std::errc::no_such_file_or_directory)) return 0;
    if(ec || !fs::is_regular_file(status)) fail("storage: cannot inspect regular storage file");
    auto bytes=fs::file_size(path,ec);
    if(ec) fail("storage: cannot inspect storage size");
    return bytes;
}
bool missingRegularSidecar(const fs::path& path) {
    std::error_code ec;
    const auto status=fs::symlink_status(path,ec);
    if(status.type()==fs::file_type::not_found || ec==std::errc::no_such_file_or_directory) return true;
    if(ec || !fs::is_regular_file(status)) fail("storage: cannot inspect regular storage file");
    return false;
}
std::uint64_t readableFileBytes(const fs::path& path) {
    const auto bytes=fileBytes(path);
    std::ifstream input(path,std::ios::binary);
    if(!input || input.peek()==std::char_traits<char>::eof())
        fail("storage: verified backup is no longer readable");
    return bytes;
}
Connection openDatabase(const fs::path& path,bool readonly) {
    sqlite3* raw=nullptr;
    int flags=readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
#ifdef SQLITE_OPEN_NOFOLLOW
    flags|=SQLITE_OPEN_NOFOLLOW;
#endif
    const auto location=canonicalFileLocation(path);
    const int rc=sqlite3_open_v2(location.u8string().c_str(),&raw,flags,nullptr);
    Connection connection(raw,sqlite3_close);
    check(rc);
    // Waits are handled by the bounded, cancellable handler below for writes.
    sqlite3_busy_timeout(raw,0);
    return connection;
}
Statement statement(sqlite3* db,const char* sql,std::atomic_bool* cancel=nullptr) {
    sqlite3_stmt* raw=nullptr;
    const int rc=sqlite3_prepare_v2(db,sql,-1,&raw,nullptr);
    Statement out(raw,sqlite3_finalize);
    check(rc,cancel); return out;
}
std::uint64_t pragma(sqlite3* db,const char* sql) {
    auto query=statement(db,sql); check(sqlite3_step(query.get()));
    if(sqlite3_column_type(query.get(),0)!=SQLITE_INTEGER) fail("storage: invalid page statistics");
    const auto n=sqlite3_column_int64(query.get(),0);
    if(n<0) fail("storage: invalid page statistics");
    return static_cast<std::uint64_t>(n);
}
bool ownedBackup(const fs::path& database,const fs::path& candidate) {
    const auto name=candidate.filename().u8string();
    const auto base=database.filename().u8string();
    const std::string suffix=".backup.sqlite";
    for(const char* marker:{".pre-source-catalog-",".pre-storage-"}) {
        const auto prefix=base+marker;
        if(name.size()<=prefix.size()+suffix.size() || name.compare(0,prefix.size(),prefix)!=0 ||
           name.compare(name.size()-suffix.size(),suffix.size(),suffix)!=0) continue;
        const auto first=name.begin()+static_cast<std::ptrdiff_t>(prefix.size());
        const auto last=name.end()-static_cast<std::ptrdiff_t>(suffix.size());
        if(std::all_of(first,last,[](char c){return c>='0' && c<='9';})) return true;
    }
    return false;
}
bool ownedReceipt(const fs::path& database,const fs::path& candidate) {
    const auto value=candidate.u8string();const std::string suffix=".provenance";
    if(value.size()<=suffix.size() || value.compare(value.size()-suffix.size(),suffix.size(),suffix)!=0)return false;
    const fs::path backup(value.substr(0,value.size()-suffix.size()));
    std::error_code ec;const auto status=fs::symlink_status(backup,ec);
    return !ec && fs::is_regular_file(status) && ownedBackup(database,backup);
}
void total(IndexStorage& report) {
    report.total_bytes=add(add(add(report.main_bytes,report.wal_bytes),report.shm_bytes),report.backup_bytes);
}
struct Work {
    std::atomic_bool* cancel;
    std::chrono::steady_clock::time_point busy_start{};
    static int progress(void* data) {
        const auto* work=static_cast<Work*>(data);
        return work->cancel && work->cancel->load();
    }
    static int busy(void* data,int count) {
        auto& work=*static_cast<Work*>(data);
        if(progress(data)) return 0;
        if(count==0) work.busy_start=std::chrono::steady_clock::now();
        if(std::chrono::steady_clock::now()-work.busy_start>=std::chrono::seconds(1)) return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); return 1;
    }
    void attach(sqlite3* db) {
        sqlite3_progress_handler(db,1000,progress,this);
        sqlite3_busy_handler(db,busy,this);
    }
};
void enoughSpace(const fs::path& directory,std::uint64_t needed) {
    std::error_code ec;
    const auto space=fs::space(directory,ec);
    if(ec || space.available==static_cast<std::uintmax_t>(-1)) fail("storage: free space query failed");
    if(space.available<needed) fail("storage: insufficient free space for backup and compaction");
}
void checkSpace(const fs::path& source,const fs::path& destination,std::uint64_t bytes) {
#if defined(_WIN32)
    // Without a reliable device identity, check the full conservative budget
    // at both locations rather than underestimating a shared filesystem.
    enoughSpace(source,multiply(3,bytes));
    enoughSpace(destination,multiply(3,bytes));
#else
    struct stat source_info{},destination_info{};
    if(::stat(source.c_str(),&source_info)!=0 || ::stat(destination.c_str(),&destination_info)!=0)
        fail("storage: free space query failed");
    if(source_info.st_dev==destination_info.st_dev) {
        enoughSpace(source,multiply(3,bytes));
        enoughSpace(destination,multiply(3,bytes));
    } else {
        enoughSpace(source,multiply(2,bytes));
        enoughSpace(destination,bytes);
    }
#endif
}
void reserveBackup(const fs::path& path) {
    // O_EXCL rejects existing files, symlinks (including dangling links), and
    // directories. Do not remove an unsuccessful or completed backup later.
#if defined(_WIN32)
    const int fd=_wopen(path.c_str(),_O_WRONLY|_O_CREAT|_O_EXCL|_O_BINARY,_S_IREAD|_S_IWRITE);
    if(fd<0) fail("storage: backup must be a new writable regular file");
    _close(fd);
#else
    const int fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if(fd<0) fail("storage: backup must be a new writable regular file");
    ::close(fd);
#endif
}
void verifyIntegrity(sqlite3* db,std::atomic_bool* cancel,const char* failure) {
    auto query=statement(db,"PRAGMA integrity_check",cancel);
    check(sqlite3_step(query.get()),cancel);
    const auto* text=sqlite3_column_text(query.get(),0);
    if(!text || std::string(reinterpret_cast<const char*>(text))!="ok") fail(failure);
    const int rc=sqlite3_step(query.get()); check(rc,cancel);
    if(rc!=SQLITE_DONE) fail(failure);
}
void copyDatabase(sqlite3* source,sqlite3* destination,std::atomic_bool* cancel) {
    sqlite3_backup* raw=sqlite3_backup_init(destination,"main",source,"main");
    if(!raw) { check(sqlite3_errcode(destination),cancel); fail("storage: backup creation failed"); }
    std::unique_ptr<sqlite3_backup,decltype(&sqlite3_backup_finish)> backup(raw,sqlite3_backup_finish);
    // Allow multi-GB copies to make steady progress. Bound lock/no-progress
    // waits, repeated writer-driven restarts, and the total copy separately.
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::minutes(30);
    auto last_progress=std::chrono::steady_clock::now();
    int previous_remaining=-1,previous_pages=-1,restarts=0;
    for(;;) {
        cancelled(cancel);
        const int rc=sqlite3_backup_step(raw,128);
        if(rc==SQLITE_DONE) break;
        if(rc!=SQLITE_OK && rc!=SQLITE_BUSY && rc!=SQLITE_LOCKED) check(rc,cancel);
        const auto now=std::chrono::steady_clock::now();
        if(rc==SQLITE_OK) {
            const int remaining=sqlite3_backup_remaining(raw),pages=sqlite3_backup_pagecount(raw);
            if(previous_remaining<0 || remaining<previous_remaining) last_progress=now;
            // Growth or an increased remaining count indicates a new snapshot.
            if(previous_remaining>=0 && (remaining>previous_remaining || pages!=previous_pages)) ++restarts;
            previous_remaining=remaining; previous_pages=pages;
        }
        if(now>=deadline || now-last_progress>=std::chrono::seconds(5) || restarts>8)
            fail("storage: backup timed out; retry after other connections finish");
        if(rc==SQLITE_BUSY || rc==SQLITE_LOCKED) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(sqlite3_backup_finish(backup.release()),cancel);
}
} // namespace

IndexMaintenanceLock::IndexMaintenanceLock(const std::string& value) {
    const auto path=absolutePath(value);regular(path);
    const auto lock_path=canonicalFileLocation(path).u8string()+".maintenance.lock";
#if defined(_WIN32)
    const auto handle=CreateFileW(fs::path(lock_path).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)fail("storage: another maintenance operation is active or lock is unavailable");
    handle_=reinterpret_cast<std::intptr_t>(handle);
#else
    const int fd=::open(lock_path.c_str(),O_RDWR|O_CREAT|O_NOFOLLOW,0600);
    if(fd<0)fail("storage: maintenance lock is unavailable");
    struct stat info{};
    if(::fstat(fd,&info)!=0 || !S_ISREG(info.st_mode) || ::flock(fd,LOCK_EX|LOCK_NB)!=0) {
        ::close(fd);fail("storage: another maintenance operation is active or lock is unavailable");
    }
    handle_=fd;
#endif
}
IndexMaintenanceLock::~IndexMaintenanceLock() {
#if defined(_WIN32)
    if(handle_!=-1)CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    if(handle_!=-1)::close(static_cast<int>(handle_));
#endif
}

IndexStorage readIndexStorage(const std::string& value) {
    const auto path=absolutePath(value);
    regular(path);
    // Reject linked or special sidecars before asking SQLite to access them.
    missingRegularSidecar(path.u8string()+"-wal");
    missingRegularSidecar(path.u8string()+"-shm");
    auto connection=openDatabase(path,true);
    IndexStorage report;
    auto read_pages=[&] {
        // Metadata-only PRAGMAs: no application schema or file-row scans.
        check(sqlite3_exec(connection.get(),"BEGIN",nullptr,nullptr,nullptr));
        report.page_size=pragma(connection.get(),"PRAGMA page_size");
        report.page_count=pragma(connection.get(),"PRAGMA page_count");
        report.freelist_bytes=multiply(report.page_size,pragma(connection.get(),"PRAGMA freelist_count"));
        check(sqlite3_exec(connection.get(),"ROLLBACK",nullptr,nullptr,nullptr));
    };
    try { read_pages(); }
    catch(const std::runtime_error& error) {
        // SQLite cannot start a read-only WAL snapshot when its sidecars are
        // absent. A no-create, query-only connection lets SQLite recreate its
        // own sidecars. Disabling close-time checkpoint keeps the main file
        // unchanged; this fallback is restricted to a missing WAL sidecar.
        const bool missing_wal=missingRegularSidecar(path.u8string()+"-wal");
        const bool missing_shm=missingRegularSidecar(path.u8string()+"-shm");
        const bool missing_sidecar=missing_wal || missing_shm;
        std::ifstream input(path,std::ios::binary);
        char header[20]{};
        input.read(header,sizeof(header));
        const bool wal_header=input.gcount()==sizeof(header) &&
            std::string(header,15)=="SQLite format 3" && header[15]=='\0' &&
            static_cast<unsigned char>(header[18])==2 && static_cast<unsigned char>(header[19])==2;
        if(std::string(error.what()).find("(code 14)")==std::string::npos ||
           !missing_sidecar || !wal_header) throw;
        connection.reset();
        connection=openDatabase(path,false);
#ifdef SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE
        check(sqlite3_db_config(connection.get(),SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE,1,nullptr));
#else
        fail("storage: SQLite cannot report WAL statistics without checkpoint control");
#endif
        check(sqlite3_exec(connection.get(),"PRAGMA query_only=ON",nullptr,nullptr,nullptr));
        read_pages();
    }
    report.main_bytes=fileBytes(path);
    report.wal_bytes=fileBytes(path.u8string()+"-wal",true);
    report.shm_bytes=fileBytes(path.u8string()+"-shm",true);
    std::error_code ec;
    fs::directory_iterator it(path.parent_path(),ec),end;
    if(ec) fail("storage: cannot inspect backup directory");
    while(it!=end) {
        if(ownedBackup(path,it->path())) {
            const auto status=it->symlink_status(ec);
            if(ec) fail("storage: cannot inspect backup directory");
            if(fs::is_regular_file(status)) {
                report.backup_bytes=add(report.backup_bytes,fileBytes(it->path()));
                report.backup_count=add(report.backup_count,1);
            }
        } else if(ownedReceipt(path,it->path())) {
            const auto status=it->symlink_status(ec);
            if(ec)fail("storage: cannot inspect backup receipt");
            if(fs::is_regular_file(status))report.backup_bytes=add(report.backup_bytes,fileBytes(it->path()));
        }
        it.increment(ec); if(ec) fail("storage: cannot inspect backup directory");
    }
    total(report); return report;
}

IndexCompaction compactIndexStorage(const std::string& value,const std::string& backup_value,std::atomic_bool* cancel,const IndexStorageProgress& progress) {
    cancelled(cancel);
    const auto path=absolutePath(value),backup_path=backup_value.empty()?fs::path{}:absolutePath(backup_value);
    regular(path);
    if(!backup_value.empty())rejectReservedBackup(path,backup_path);
    std::error_code ec;
    if(!backup_value.empty()) {
        const auto status=fs::symlink_status(backup_path,ec);
        if((ec && ec!=std::errc::no_such_file_or_directory) || status.type()!=fs::file_type::not_found)
            fail("storage: backup must be a new writable regular file");
    }
    IndexCompaction result;
    result.before=readIndexStorage(path.u8string());
    const auto current=add(result.before.main_bytes,result.before.wal_bytes);
    if(backup_value.empty())enoughSpace(path.parent_path(),current);
    else checkSpace(path.parent_path(),backup_path.parent_path(),current);
    IndexMaintenanceLock maintenance(path.u8string());
    Work work{cancel};
    const auto source_identity=sourceIdentity(path);
    auto connection=openDatabase(path,false);
    unchangedSource(source_identity,path);
    work.attach(connection.get());
    cancelled(cancel);
    if(!backup_value.empty()) {
        if(progress)progress(IndexStoragePhase::Backup);
        cancelled(cancel);reserveBackup(backup_path);
        {
            auto backup=openDatabase(backup_path,false);
            work.attach(backup.get());
            copyDatabase(connection.get(),backup.get(),cancel);
            verifyIntegrity(backup.get(),cancel,"storage: backup integrity check failed");
        }
        if(progress)progress(IndexStoragePhase::BackupVerified);
        result.backup_bytes=fileBytes(backup_path);
        writeBackupProvenance(path,backup_path,source_identity);
    }
    cancelled(cancel);
    // VACUUM runs on the live database. SQLite owns the atomic transaction;
    // never rename a stale backup or replacement over other writers' commits.
    if(progress)progress(IndexStoragePhase::Vacuum);
    cancelled(cancel);
    const int vacuum_result=sqlite3_exec(connection.get(),"VACUUM",nullptr,nullptr,nullptr);
    // Record native completion before cancellation can turn success into an
    // interrupted final verification. The GUI uses Verify as this boundary.
    if(vacuum_result==SQLITE_OK && progress)progress(IndexStoragePhase::Verify);
    check(vacuum_result,cancel);
    verifyIntegrity(connection.get(),cancel,"storage: database integrity check failed");
    // No journal_mode changes or unconditional WAL checkpoint claims. A WAL
    // retained by another reader is included at its actual size in the report.
    connection.reset();
    // SQLite owns its sidecar lifecycle. Success requires the independent
    // verified backup to still exist and be readable after live maintenance.
    if(!backup_value.empty())result.backup_bytes=readableFileBytes(backup_path);
    result.after=readIndexStorage(path.u8string());
    if(backup_value.empty())return result;
    const auto source_parent=fs::canonical(path.parent_path(),ec);
    if(ec) fail("storage: cannot inspect backup directory");
    const auto backup_parent=fs::canonical(backup_path.parent_path(),ec);
    if(ec) fail("storage: cannot inspect backup directory");
    if(source_parent!=backup_parent || !ownedBackup(path,backup_path)) {
        result.after.backup_bytes=add(result.after.backup_bytes,add(result.backup_bytes,fileBytes(provenancePath(backup_path),true)));
        result.after.backup_count=add(result.after.backup_count,1);
        total(result.after);
    }
    return result;
}
namespace {
void legacyOptimizationInput(sqlite3* db) {
    if(pragma(db,"PRAGMA user_version")!=413)
        fail("storage: optimize requires initialized schema 413; first open and upgrade with a source-catalog capable version, then retry optimize");
    auto ready=statement(db,"SELECT value FROM app_metadata WHERE key='schema_413_ready_v1'");
    if(sqlite3_step(ready.get())!=SQLITE_ROW || !sqlite3_column_text(ready.get(),0) ||
       std::string(reinterpret_cast<const char*>(sqlite3_column_text(ready.get(),0)))!="1")
        fail("storage: optimize requires a verified initialized schema 413");
    statement(db,"SELECT id,source_id,relative_path,path,parent_path,name,ext,root,size,modified_time,is_dir,search_name,search_path,scan_generation,cloud_copy FROM files LIMIT 0");
}
void optimizationReport(IndexOptimization& result,const fs::path& path,const fs::path& backup_path) {
    result.after=readIndexStorage(path.u8string());
    if(!result.backup_bytes)return;
    result.backup_bytes=readableFileBytes(backup_path);
    std::error_code ec;
    const auto source_parent=fs::canonical(path.parent_path(),ec);
    if(ec)fail("storage: cannot inspect backup directory");
    const auto backup_parent=fs::canonical(backup_path.parent_path(),ec);
    if(ec)fail("storage: cannot inspect backup directory");
    if(source_parent!=backup_parent || !ownedBackup(path,backup_path)) {
        result.after.backup_bytes=add(result.after.backup_bytes,add(result.backup_bytes,fileBytes(provenancePath(backup_path),true)));
        result.after.backup_count=add(result.after.backup_count,1);total(result.after);
    }
}
}
IndexOptimization migrateIndexStorageToDirectory(const std::string& value,const std::string& backup_value,
    bool vacuum,std::atomic_bool* cancel,const IndexStorageProgress& progress) {
    IndexOptimization result;bool transaction=false;
    std::unique_ptr<IndexMaintenanceLock> maintenance;
    Work work{cancel};
    Connection connection(nullptr,sqlite3_close);
    fs::path path,backup_path;
    try {
        cancelled(cancel);path=absolutePath(value);regular(path);
        // Reject unsupported older input without initialization, a lock file, or a backup.
        {
            auto input=openDatabase(path,true);
            if(hasDirectoryStorage(input.get())) {
                if(!directoryStorageReady(input.get()))fail("storage: damaged or unsupported directory format");
                result.already_compact=true;result.before=result.after=readIndexStorage(path.u8string());return result;
            }
            legacyOptimizationInput(input.get());
        }
        if(!backup_value.empty()) {
            backup_path=absolutePath(backup_value);rejectReservedBackup(path,backup_path);
            std::error_code ec;const auto status=fs::symlink_status(backup_path,ec);
            if((ec && ec!=std::errc::no_such_file_or_directory)||status.type()!=fs::file_type::not_found)fail("storage: backup must be a new writable regular file");
        }
        maintenance=std::make_unique<IndexMaintenanceLock>(path.u8string());
        result.before=readIndexStorage(path.u8string());
        // New and old tables coexist transactionally, and WAL/VACUUM need workspace.
        // Reuse the conservative compaction budget with double the live size.
        if(backup_value.empty())enoughSpace(path.parent_path(),multiply(2,add(result.before.main_bytes,result.before.wal_bytes)));
        else checkSpace(path.parent_path(),backup_path.parent_path(),multiply(2,add(result.before.main_bytes,result.before.wal_bytes)));
        const auto source_identity=sourceIdentity(path);
        connection=openDatabase(path,false);unchangedSource(source_identity,path);
        work.attach(connection.get());registerDirectoryWriter(connection.get());
        check(sqlite3_exec(connection.get(),"BEGIN IMMEDIATE",nullptr,nullptr,nullptr),cancel);transaction=true;
        if(hasDirectoryStorage(connection.get())) {
            if(!directoryStorageReady(connection.get()))fail("storage: damaged or unsupported directory format");
            check(sqlite3_exec(connection.get(),"ROLLBACK",nullptr,nullptr,nullptr));transaction=false;
            result.already_compact=true;result.after=readIndexStorage(path.u8string());return result;
        }
        legacyOptimizationInput(connection.get());
        if(!backup_value.empty()) {
            if(progress)progress(IndexStoragePhase::Backup);cancelled(cancel);
            reserveBackup(backup_path);result.backup_path=backup_path.u8string();
            {
                auto source=openDatabase(path,true),backup=openDatabase(backup_path,false);
                work.attach(source.get());work.attach(backup.get());
                copyDatabase(source.get(),backup.get(),cancel);
                verifyIntegrity(backup.get(),cancel,"storage: backup integrity check failed");
            }
            if(progress)progress(IndexStoragePhase::BackupVerified);
            result.backup_bytes=readableFileBytes(backup_path);
            writeBackupProvenance(path,backup_path,source_identity);
        }
        if(progress)progress(IndexStoragePhase::Migrate);cancelled(cancel);
        migrateDirectorySchema(connection.get());
        verifyIntegrity(connection.get(),cancel,"storage: migrated database integrity check failed");
        cancelled(cancel);
        // Set committed from COMMIT's return code before observing cancellation again.
        const int committed=sqlite3_exec(connection.get(),"COMMIT",nullptr,nullptr,nullptr);
        if(committed!=SQLITE_OK)check(committed,cancel);
        transaction=false;result.migration_committed=true;
        if(vacuum) {
            if(progress)progress(IndexStoragePhase::Vacuum);cancelled(cancel);
            const int vacuum_result=sqlite3_exec(connection.get(),"VACUUM",nullptr,nullptr,nullptr);
            if(vacuum_result==SQLITE_OK)result.vacuum_completed=true;
            check(vacuum_result,cancel);
        }
        if(progress)progress(IndexStoragePhase::Verify);
        verifyIntegrity(connection.get(),cancel,"storage: migrated database integrity check failed");
        connection.reset();optimizationReport(result,path,backup_path);
    }catch(const std::exception& error) {
        result.error=error.what();
        if(connection) {
            sqlite3_progress_handler(connection.get(),0,nullptr,nullptr);
            sqlite3_busy_handler(connection.get(),nullptr,nullptr);
            if(transaction)sqlite3_exec(connection.get(),"ROLLBACK",nullptr,nullptr,nullptr);
        }
        connection.reset();
        if(!path.empty())try{optimizationReport(result,path,backup_path);}catch(const std::exception&){}
    }
    return result;
}
namespace {
bool independentRegular(const fs::path& path) {
    std::error_code ec;const auto status=fs::symlink_status(path,ec);
    if(ec || !fs::is_regular_file(status))return false;
    const auto links=fs::hard_link_count(path,ec);
    return !ec && links==1;
}
struct BackupIdentity {
    fs::path path;
    std::uint64_t bytes=0;
    fs::file_time_type modified;
#ifndef _WIN32
    dev_t device{};ino_t inode{};
#endif
};
BackupIdentity backupIdentity(const fs::path& path) {
    if(!independentRegular(path))fail("storage: backup must be an independent regular file");
    BackupIdentity identity;identity.path=path;identity.bytes=fileBytes(path);
    std::error_code ec;identity.modified=fs::last_write_time(path,ec);
    if(ec)fail("storage: cannot inspect backup identity");
#ifndef _WIN32
    struct stat info{};
    if(::lstat(path.c_str(),&info)!=0 || !S_ISREG(info.st_mode) || info.st_nlink!=1)
        fail("storage: cannot inspect backup identity");
    identity.device=info.st_dev;identity.inode=info.st_ino;
#endif
    return identity;
}
void unchangedBackup(const BackupIdentity& expected) {
    const auto actual=backupIdentity(expected.path);
    if(actual.bytes!=expected.bytes || actual.modified!=expected.modified
#ifndef _WIN32
       || actual.device!=expected.device || actual.inode!=expected.inode
#endif
    )fail("storage: backup changed; cleanup stopped");
}
#ifndef _WIN32
std::string provenanceContent(const fs::path& source,const fs::path& backup,const SourceIdentity* expected=nullptr) {
    const auto canonical_source=canonicalFileLocation(source),canonical_backup=canonicalFileLocation(backup);
    const auto current_source=sourceIdentity(source);
    if(expected)unchangedSource(*expected,source);
    const auto& source_info=expected?expected->info:current_source.info;
    const auto identity=backupIdentity(canonical_backup);
    struct stat backup_info{};
    if(::lstat(canonical_backup.c_str(),&backup_info)!=0 || !S_ISREG(backup_info.st_mode))
        fail("storage: cannot inspect backup provenance identity");
    const auto hex=[](const std::string& text){static constexpr char digits[]="0123456789abcdef";std::string out;for(const unsigned char c:text){out+=digits[c>>4];out+=digits[c&15];}return out;};
    std::ostringstream out;
    out<<"EverythingLite backup provenance 1\n"<<hex(canonical_source.u8string())<<'\n'
       <<static_cast<std::uintmax_t>(source_info.st_dev)<<' '<<static_cast<std::uintmax_t>(source_info.st_ino);
#if defined(__APPLE__)
    out<<' '<<source_info.st_birthtimespec.tv_sec<<' '<<source_info.st_birthtimespec.tv_nsec;
#endif
    out<<'\n'<<hex(canonical_backup.u8string())<<'\n'<<static_cast<std::uintmax_t>(backup_info.st_dev)<<' '
       <<static_cast<std::uintmax_t>(backup_info.st_ino)<<' '<<identity.bytes<<' ';
#if defined(__APPLE__)
    out<<backup_info.st_mtimespec.tv_sec<<' '<<backup_info.st_mtimespec.tv_nsec;
#else
    out<<backup_info.st_mtim.tv_sec<<' '<<backup_info.st_mtim.tv_nsec;
#endif
    out<<'\n';return out.str();
}
#endif
void writeBackupProvenance(const fs::path& source,const fs::path& backup,const SourceIdentity& expected) {
#ifdef _WIN32
    // No POSIX file identity exists here. Backups remain usable manually, but
    // cleanup below refuses to trust them without a reliable file-id binding.
    (void)source;(void)backup;(void)expected;
#else
    const auto content=provenanceContent(source,backup,&expected),receipt=provenancePath(backup).u8string();
    const int fd=::open(receipt.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if(fd<0)fail("storage: cannot create exclusive backup provenance receipt");
    int directory=-1;
    try {
        std::size_t offset=0;
        while(offset<content.size()) {
            const auto written=::write(fd,content.data()+offset,content.size()-offset);
            if(written<0 && errno==EINTR)continue;
            if(written<=0)fail("storage: cannot write backup provenance receipt");
            offset+=static_cast<std::size_t>(written);
        }
        if(::fsync(fd)!=0)fail("storage: cannot persist backup provenance receipt");
        directory=::open(canonicalFileLocation(backup).parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
        if(directory<0 || ::fsync(directory)!=0)fail("storage: cannot persist backup provenance directory");
        ::close(directory);directory=-1;::close(fd);
    }catch(...) {
        // An incomplete or non-durable receipt must never authorize cleanup.
        ::ftruncate(fd,0);::fsync(fd);::close(fd);if(directory>=0)::close(directory);
        ::unlink(receipt.c_str());throw;
    }
#endif
}
bool backupProvenanceMatches(const fs::path& source,const fs::path& backup) {
#ifdef _WIN32
    (void)source;(void)backup;return false;
#else
    try {
        const auto receipt=provenancePath(backup);const auto identity=backupIdentity(receipt);
        const auto expected=provenanceContent(source,backup);
        if(identity.bytes!=expected.size())return false;
        struct stat info{};if(::lstat(receipt.c_str(),&info)!=0 || (info.st_mode&0777)!=0600)return false;
        const int fd=::open(receipt.c_str(),O_RDONLY|O_NOFOLLOW);if(fd<0)return false;
        std::string actual(expected.size(),'\0');std::size_t offset=0;
        while(offset<actual.size()) {
            const auto count=::read(fd,actual.data()+offset,actual.size()-offset);
            if(count<0 && errno==EINTR)continue;
            if(count<=0){::close(fd);return false;}offset+=static_cast<std::size_t>(count);
        }
        char extra=0;const auto trailing=::read(fd,&extra,1);::close(fd);
        unchangedBackup(identity);return trailing==0 && actual==expected;
    }catch(const std::exception&) {return false;}
#endif
}
void standaloneSidecars(const fs::path& path) {
    for(const auto* suffix:{"-wal","-journal"})
        if(fileBytes(path.u8string()+suffix,true)!=0)
            fail("storage: recovery backup requires a standalone database without pending WAL or journal");
    // SHM is disposable coordination data; require a regular file if present.
    fileBytes(path.u8string()+"-shm",true);
}
Connection openStandaloneBackup(const fs::path& path) {
    // immutable is safe only after rejecting sidecars, and only for retained
    // backup files. Live database validation must continue to read its WAL.
    static constexpr char hex[]="0123456789ABCDEF";
    std::string uri="file:";
    for(const unsigned char c:path.generic_u8string()) {
        if((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='/' || c==':' || c=='-' || c=='_' || c=='.')uri+=static_cast<char>(c);
        else {uri+='%';uri+=hex[c>>4];uri+=hex[c&15];}
    }
    uri+="?mode=ro&immutable=1";
    sqlite3* raw=nullptr;int flags=SQLITE_OPEN_READONLY|SQLITE_OPEN_URI;
#ifdef SQLITE_OPEN_NOFOLLOW
    flags|=SQLITE_OPEN_NOFOLLOW;
#endif
    const int rc=sqlite3_open_v2(uri.c_str(),&raw,flags,nullptr);
    Connection connection(raw,sqlite3_close);check(rc);return connection;
}
void verifyApplicationRecovery(sqlite3* db,std::atomic_bool* cancel) {
    if(hasDirectoryStorage(db)) {
        if(!directoryStorageReady(db))fail("storage: recovery database has unsupported or incomplete application schema");
    }else if(!sourceCatalogSchemaReady(db)) {
        fail("storage: recovery database has unsupported or incomplete application schema");
    }
    verifyIntegrity(db,cancel,"storage: recovery database integrity check failed");
    auto foreign=statement(db,"PRAGMA foreign_key_check",cancel);
    const auto rc=sqlite3_step(foreign.get());check(rc,cancel);
    if(rc!=SQLITE_DONE)fail("storage: recovery database has invalid references");
    auto source=statement(db,"SELECT 1 FROM files f LEFT JOIN sources s ON s.id=f.source_id WHERE s.id IS NULL LIMIT 1",cancel);
    const auto source_rc=sqlite3_step(source.get());check(source_rc,cancel);
    if(source_rc!=SQLITE_DONE)fail("storage: recovery database has missing sources");
}
}
std::vector<IndexStorageBackup> listIndexStorageBackups(const std::string& value) {
    const auto input=absolutePath(value);regular(input);const auto path=canonicalFileLocation(input);
    std::vector<IndexStorageBackup> result;std::error_code ec;
    fs::directory_iterator it(path.parent_path(),ec),end;
    if(ec)fail("storage: cannot inspect backup directory");
    while(it!=end) {
        if(ownedBackup(path,it->path()) && independentRegular(it->path()))result.push_back({it->path().u8string(),fileBytes(it->path())});
        it.increment(ec);if(ec)fail("storage: cannot inspect backup directory");
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.path<b.path;});
    return result;
}
IndexBackupCleanup removeIndexStorageBackups(const std::string& value,const std::vector<std::string>& selected,std::atomic_bool* cancel,const std::function<void(std::size_t)>& removed_progress,bool allow_last_backup) {
    IndexBackupCleanup result;
    try {
        cancelled(cancel);
#ifdef _WIN32
        fail("storage: backup cleanup requires reliable file identity on this platform");
#endif
        const auto input=absolutePath(value);regular(input);const auto path=canonicalFileLocation(input);
        IndexMaintenanceLock maintenance(path.u8string());
        if(selected.empty())fail("storage: select exact backup files before cleanup");
        std::set<std::string> selected_names;std::vector<BackupIdentity> remove;
        for(const auto& name:selected) {
            const auto candidate=absolutePath(name).lexically_normal();
            if(candidate!=canonicalFileLocation(candidate) || candidate.parent_path()!=path.parent_path() || !ownedBackup(path,candidate))
                fail("storage: cleanup accepts only exact owned backup files in the database directory");
            rejectReservedBackup(path,candidate);
            if(selected_names.insert(candidate.u8string()).second) {
                const auto identity=backupIdentity(candidate);standaloneSidecars(identity.path);remove.push_back(identity);
            }
        }
        // Initialize nothing: validate the current application and committed WAL.
        readIndexStorage(path.u8string());Work work{cancel};auto current=openDatabase(path,true);work.attach(current.get());
        verifyApplicationRecovery(current.get(),cancel);
        std::unique_ptr<BackupIdentity> retained;
        if(!allow_last_backup) {
            for(const auto& candidate:listIndexStorageBackups(path.u8string())) {
                cancelled(cancel);if(selected_names.count(candidate.path))continue;
                try {
                    const auto identity=backupIdentity(fs::path(candidate.path));standaloneSidecars(identity.path);
                    auto recovery=openStandaloneBackup(identity.path);work.attach(recovery.get());
                    verifyApplicationRecovery(recovery.get(),cancel);unchangedBackup(identity);
                    if(!backupProvenanceMatches(path,identity.path))continue;
                    retained=std::make_unique<BackupIdentity>(identity);break;
                }catch(const std::exception&) {cancelled(cancel);}
            }
            if(!retained)fail("storage: keep at least one verified standalone application recovery backup");
        }
        for(const auto& identity:remove) {
            cancelled(cancel);unchangedBackup(identity);
            if(retained) {
                unchangedBackup(*retained);standaloneSidecars(retained->path);
                if(!backupProvenanceMatches(path,retained->path))fail("storage: retained backup provenance changed; cleanup stopped");
            }
            const auto receipt=provenancePath(identity.path);std::unique_ptr<BackupIdentity> receipt_identity;
            std::error_code ec;const auto receipt_status=fs::symlink_status(receipt,ec);
            if(receipt_status.type()!=fs::file_type::not_found)receipt_identity=std::make_unique<BackupIdentity>(backupIdentity(receipt));
            else if(ec && ec!=std::errc::no_such_file_or_directory)fail("storage: cannot inspect selected backup receipt");
            if(!fs::remove(identity.path,ec) || ec)fail("storage: could not remove a selected backup; earlier deletions remain completed");
            result.removed_paths.push_back(identity.path.u8string());result.removed_bytes=add(result.removed_bytes,identity.bytes);
            if(receipt_identity) {
                try {
                    unchangedBackup(*receipt_identity);ec.clear();
                    if(!fs::remove(receipt,ec) || ec)fail("storage: provenance receipt removal failed");
                    result.removed_bytes=add(result.removed_bytes,receipt_identity->bytes);
                }catch(const std::exception&) {
                    result.orphan_receipts.push_back(receipt.u8string());
                    fail("storage: backup was removed but its orphan provenance receipt remains; earlier deletions remain completed");
                }
            }
            if(removed_progress)removed_progress(result.removed_paths.size());
        }
    }catch(const std::exception& error) {result.error=error.what();result.cancelled=cancel && cancel->load();}
    return result;
}
} // namespace everything_lite
