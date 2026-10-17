#include "core/index_storage.h"
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
        }
        it.increment(ec); if(ec) fail("storage: cannot inspect backup directory");
    }
    total(report); return report;
}

IndexCompaction compactIndexStorage(const std::string& value,const std::string& backup_value,std::atomic_bool* cancel) {
    cancelled(cancel);
    const auto path=absolutePath(value),backup_path=absolutePath(backup_value);
    regular(path);
    rejectReservedBackup(path,backup_path);
    std::error_code ec;
    const auto status=fs::symlink_status(backup_path,ec);
    if((ec && ec!=std::errc::no_such_file_or_directory) || status.type()!=fs::file_type::not_found)
        fail("storage: backup must be a new writable regular file");
    IndexCompaction result;
    result.before=readIndexStorage(path.u8string());
    const auto current=add(result.before.main_bytes,result.before.wal_bytes);
    checkSpace(path.parent_path(),backup_path.parent_path(),current);
    IndexMaintenanceLock maintenance(path.u8string());
    Work work{cancel};
    auto connection=openDatabase(path,false);
    work.attach(connection.get());
    cancelled(cancel);
    reserveBackup(backup_path);
    {
        auto backup=openDatabase(backup_path,false);
        work.attach(backup.get());
        copyDatabase(connection.get(),backup.get(),cancel);
        verifyIntegrity(backup.get(),cancel,"storage: backup integrity check failed");
    }
    result.backup_bytes=fileBytes(backup_path);
    cancelled(cancel);
    // VACUUM runs on the live database. SQLite owns the atomic transaction;
    // never rename a stale backup or replacement over other writers' commits.
    check(sqlite3_exec(connection.get(),"VACUUM",nullptr,nullptr,nullptr),cancel);
    verifyIntegrity(connection.get(),cancel,"storage: database integrity check failed; keep the verified backup");
    // No journal_mode changes or unconditional WAL checkpoint claims. A WAL
    // retained by another reader is included at its actual size in the report.
    connection.reset();
    // SQLite owns its sidecar lifecycle. Success requires the independent
    // verified backup to still exist and be readable after live maintenance.
    result.backup_bytes=readableFileBytes(backup_path);
    result.after=readIndexStorage(path.u8string());
    const auto source_parent=fs::canonical(path.parent_path(),ec);
    if(ec) fail("storage: cannot inspect backup directory");
    const auto backup_parent=fs::canonical(backup_path.parent_path(),ec);
    if(ec) fail("storage: cannot inspect backup directory");
    if(source_parent!=backup_parent || !ownedBackup(path,backup_path)) {
        result.after.backup_bytes=add(result.after.backup_bytes,result.backup_bytes);
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
        result.after.backup_bytes=add(result.after.backup_bytes,result.backup_bytes);
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
        backup_path=absolutePath(backup_value);rejectReservedBackup(path,backup_path);
        std::error_code ec;const auto status=fs::symlink_status(backup_path,ec);
        if((ec && ec!=std::errc::no_such_file_or_directory)||status.type()!=fs::file_type::not_found)fail("storage: backup must be a new writable regular file");
        maintenance=std::make_unique<IndexMaintenanceLock>(path.u8string());
        result.before=readIndexStorage(path.u8string());
        // New and old tables coexist transactionally, and WAL/VACUUM need workspace.
        // Reuse the conservative compaction budget with double the live size.
        checkSpace(path.parent_path(),backup_path.parent_path(),multiply(2,add(result.before.main_bytes,result.before.wal_bytes)));
        connection=openDatabase(path,false);work.attach(connection.get());registerDirectoryWriter(connection.get());
        check(sqlite3_exec(connection.get(),"BEGIN IMMEDIATE",nullptr,nullptr,nullptr),cancel);transaction=true;
        if(hasDirectoryStorage(connection.get())) {
            if(!directoryStorageReady(connection.get()))fail("storage: damaged or unsupported directory format");
            check(sqlite3_exec(connection.get(),"ROLLBACK",nullptr,nullptr,nullptr));transaction=false;
            result.already_compact=true;result.after=readIndexStorage(path.u8string());return result;
        }
        legacyOptimizationInput(connection.get());
        if(progress)progress(IndexStoragePhase::Backup);cancelled(cancel);
        reserveBackup(backup_path);result.backup_path=backup_path.u8string();
        {
            auto source=openDatabase(path,true),backup=openDatabase(backup_path,false);
            work.attach(source.get());work.attach(backup.get());
            copyDatabase(source.get(),backup.get(),cancel);
            verifyIntegrity(backup.get(),cancel,"storage: backup integrity check failed");
        }
        result.backup_bytes=readableFileBytes(backup_path);
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
        verifyIntegrity(connection.get(),cancel,"storage: migrated database integrity check failed; keep recovery backup");
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
} // namespace everything_lite
