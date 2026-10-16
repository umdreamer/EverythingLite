#include "core/index_storage.h"
#include <sqlite3.h>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>
#ifndef _WIN32
#include <sys/stat.h>
#endif
using namespace everything_lite;
namespace fs=std::filesystem;
static void sql(sqlite3* db,const char* text) { assert(sqlite3_exec(db,text,nullptr,nullptr,nullptr)==SQLITE_OK); }
static sqlite3* open(const fs::path& p) { sqlite3* db=nullptr; assert(sqlite3_open(p.string().c_str(),&db)==SQLITE_OK); return db; }
static std::string scalar(sqlite3* db,const char* text) {
    sqlite3_stmt* s=nullptr; assert(sqlite3_prepare_v2(db,text,-1,&s,nullptr)==SQLITE_OK);
    assert(sqlite3_step(s)==SQLITE_ROW); const std::string out=reinterpret_cast<const char*>(sqlite3_column_text(s,0));
    sqlite3_finalize(s); return out;
}
static bool rejected(const std::function<void()>& fn) { try {fn();} catch(const std::exception& e) {assert(std::string(e.what()).find("/private/")==std::string::npos); return true;} return false; }
static std::string bytes(const fs::path& p) {std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main() {
    const auto base=fs::path("/private/tmp")/("everything-storage-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(base);
    const auto path=base/"index.sqlite";
    auto* db=open(path);
    sql(db,"CREATE TABLE files(id INTEGER PRIMARY KEY,source INTEGER, payload TEXT); CREATE TABLE sources(id INTEGER PRIMARY KEY,status TEXT,metadata TEXT); INSERT INTO sources VALUES(1,'accessible','one'),(2,'offline','two'); BEGIN; WITH RECURSIVE n(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<5000) INSERT INTO files SELECT x,1+x%2,hex(zeroblob(1000)) FROM n; DELETE FROM files WHERE id>25; COMMIT;");
    const auto content=scalar(db,"SELECT group_concat(id||':'||source||':'||payload,'|') FROM files ORDER BY id");
    const auto sources=scalar(db,"SELECT group_concat(id||':'||status||':'||metadata,'|') FROM sources ORDER BY id");
    sqlite3_close(db);
    const auto original=bytes(path);
    const auto report=readIndexStorage(path.string());
    assert(report.main_bytes==fs::file_size(path) && "read-only report returns actual main bytes");
    assert(report.freelist_bytes>0 && report.page_count>0 && report.page_size>0);
    assert(bytes(path)==original);
    std::ofstream(base/"index.sqlite.pre-source-catalog-123.backup.sqlite")<<"old";
    std::ofstream(base/"index.sqlite.pre-storage-123.backup.sqlite")<<"storage";
    std::ofstream(base/"index.sqlite.pre-storage-abc.backup.sqlite")<<"unrelated";
    std::ofstream(base/"index.sqlite.pre-source-catalog-1.backup.sqlite.extra")<<"unrelated";
    fs::create_symlink(path,base/"index.sqlite.pre-storage-999.backup.sqlite");
    const auto reported=readIndexStorage(path.string());
    assert(reported.backup_count==2 && reported.backup_bytes==10);
    assert(reported.total_bytes==reported.main_bytes+reported.wal_bytes+reported.shm_bytes+10);
    // SQLite owns these names even when the sidecars do not yet exist.
    // Refusal must happen before creating a backup or compacting the source.
    fs::create_directories(base/"child");
    fs::create_directory_symlink(base,base/"parent-alias");
    for(const char* suffix:{"-wal","-shm","-journal"}) {
        for(const auto& directory:{base,base/"child"/"..",base/"parent-alias",base.lexically_relative(fs::current_path())/"child"/".."}) {
            const auto guarded=base/(std::string("guarded")+suffix+".sqlite");
            fs::copy_file(path,guarded,fs::copy_options::overwrite_existing);
            const auto target=directory/(guarded.filename().string()+suffix);
            assert(!fs::exists(target));
            assert(rejected([&]{compactIndexStorage(guarded.string(),target.string());}) && "reserved sidecar backup must be rejected");
            assert(!fs::exists(target) && "refusal must not create a reserved backup");
            assert(bytes(guarded)==original && "refusal must not compact the original");
        }
    }
    assert(rejected([&]{compactIndexStorage(path.string(),(base/"parent-alias"/path.filename()).string());}));
    assert(bytes(path)==original);
    // Maintenance paths cannot be SQLite sidecars, including ASCII case
    // aliases and reverse relationships where opening backup owns the source.
    for(const char* suffix:{"-jOuRnAl","-WaL","-sHm"}) {
        const auto target=base/(std::string("INDEX.SQLITE")+suffix);
        assert(!fs::exists(target));
        assert(rejected([&]{compactIndexStorage(path.string(),target.string());}));
        assert(!fs::exists(target) && bytes(path)==original);
        const auto other_directory=base/"child"/(std::string("unrelated")+suffix);
        assert(rejected([&]{compactIndexStorage(path.string(),other_directory.string());}));
        assert(!fs::exists(other_directory) && bytes(path)==original);
        const auto reverse_source=base/(std::string("reverse.sqlite")+suffix);
        const auto reverse_backup=base/"reverse.sqlite";
        fs::copy_file(path,reverse_source);
        assert(rejected([&]{compactIndexStorage(reverse_source.string(),reverse_backup.string());}));
        assert(!fs::exists(reverse_backup) && bytes(reverse_source)==original);
    }
    const auto backup=base/"index.sqlite.pre-storage-456.backup.sqlite";
    const auto result=compactIndexStorage(path.string(),backup.string());
    assert(result.after.main_bytes<result.before.main_bytes && result.after.freelist_bytes==0);
    assert(result.backup_bytes==fs::file_size(backup));
    assert(result.after.backup_count==3 && result.after.backup_bytes==10+result.backup_bytes);
    assert(result.after.total_bytes==result.after.main_bytes+result.after.wal_bytes+result.after.shm_bytes+result.after.backup_bytes);
#ifndef _WIN32
    struct stat info{}; assert(::stat(backup.c_str(),&info)==0 && (info.st_mode&0777)==0600);
#endif
    // A relocated main file must recover without any companion WAL/SHM.
    const auto standalone=base/"standalone.sqlite";
    fs::copy_file(backup,standalone);
    for(const auto& p:{path,backup,standalone}) {
        db=open(p); assert(scalar(db,"PRAGMA integrity_check")=="ok");
        assert(scalar(db,"SELECT group_concat(id||':'||source||':'||payload,'|') FROM files ORDER BY id")==content);
        assert(scalar(db,"SELECT group_concat(id||':'||status||':'||metadata,'|') FROM sources ORDER BY id")==sources);
        sqlite3_close(db);
    }
    const auto absent=base/"missing.sqlite";
    assert(rejected([&]{readIndexStorage(absent.string());}));
    assert(rejected([&]{compactIndexStorage(absent.string(),(base/"missing.backup").string());}));
    assert(!fs::exists(absent) && !fs::exists(base/"missing.backup"));
    assert(rejected([&]{compactIndexStorage(path.string(),backup.string());}));
    const auto linked=base/"linked.sqlite"; fs::create_symlink(path,linked);
    assert(rejected([&]{compactIndexStorage(linked.string(),(base/"linked.backup").string());}));
    fs::create_symlink(absent,base/"dangling.backup");
    assert(rejected([&]{compactIndexStorage(path.string(),(base/"dangling.backup").string());}));
    assert(rejected([&]{compactIndexStorage(base.string(),(base/"directory.backup").string());}));
    std::atomic_bool cancel{true};
    assert(rejected([&]{compactIndexStorage(path.string(),(base/"cancel.backup").string(),&cancel);}));
    assert(!fs::exists(base/"cancel.backup"));
    db=open(path); sql(db,"PRAGMA journal_mode=WAL; BEGIN IMMEDIATE; UPDATE sources SET metadata='uncommitted' WHERE id=2;");
    const auto wal=readIndexStorage(path.string()); assert(wal.wal_bytes==fs::file_size(path.string()+"-wal"));
    const auto locked=base/"locked.backup";
    const auto start=std::chrono::steady_clock::now();
    assert(rejected([&]{compactIndexStorage(path.string(),locked.string());}));
    assert(std::chrono::steady_clock::now()-start<std::chrono::seconds(10));
    // The verified copy remains available when native VACUUM cannot acquire its write lock.
    assert(fs::is_regular_file(locked));
    auto* copy=open(locked); assert(scalar(copy,"PRAGMA integrity_check")=="ok"); sqlite3_close(copy);
    cancel=false;
    std::thread cancelling([&]{std::this_thread::sleep_for(std::chrono::milliseconds(25));cancel=true;});
    assert(rejected([&]{compactIndexStorage(path.string(),(base/"cancel-during.backup").string(),&cancel);}));
    cancelling.join();
    sql(db,"COMMIT;"); assert(scalar(db,"SELECT metadata FROM sources WHERE id=2")=="uncommitted");
    sqlite3_close(db);
    // A committed concurrent change must survive the live native VACUUM.
    db=open(path); sql(db,"PRAGMA journal_mode=WAL; BEGIN IMMEDIATE; UPDATE sources SET metadata='newer-commit' WHERE id=2;");
    std::thread commit([&]{std::this_thread::sleep_for(std::chrono::milliseconds(200));sql(db,"COMMIT;");});
    const auto concurrent=compactIndexStorage(path.string(),(base/"concurrent.backup").string());
    commit.join();
    assert(concurrent.backup_bytes>0);
    assert(scalar(db,"SELECT metadata FROM sources WHERE id=2")=="newer-commit");
    sqlite3_close(db);
    const auto external=base/"explicit.backup";
    const auto explicit_result=compactIndexStorage(path.string(),external.string());
    assert(explicit_result.after.backup_bytes==readIndexStorage(path.string()).backup_bytes+fs::file_size(external));
    const auto corrupt=base/"corrupt.sqlite";
    std::ofstream(corrupt)<<"not a database";
    const auto corrupt_before=bytes(corrupt);
    assert(rejected([&]{readIndexStorage(corrupt.string());}));
    assert(bytes(corrupt)==corrupt_before);
    assert(rejected([&]{compactIndexStorage(path.string(),(base/"absent-dir"/"backup").string());}));
    assert(!fs::exists(base/"absent-dir"));
    fs::remove_all(base);
    std::cout<<"index storage tests passed\n";
}
