#include "core/index_storage.h"
#include "core/database.h"
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
static std::uintmax_t receiptBytes(const fs::path& backup) {const fs::path receipt(backup.string()+".provenance");return fs::exists(receipt)?fs::file_size(receipt):0;}
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
    assert(result.after.backup_count==3 && result.after.backup_bytes==10+result.backup_bytes+receiptBytes(backup));
    assert(result.after.total_bytes==result.after.main_bytes+result.after.wal_bytes+result.after.shm_bytes+result.after.backup_bytes);
    const auto without_backup=compactIndexStorage(path.string(),"");
    assert(without_backup.backup_bytes==0);
    assert(without_backup.after.backup_count==result.after.backup_count);
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
    assert(rejected([&]{readIndexStorage(linked.string());}));
    assert(rejected([&]{compactIndexStorage(linked.string(),(base/"linked.backup").string());}));
    const auto missing_link=base/"dangling-main.sqlite";
    fs::create_symlink(absent,missing_link);
    assert(rejected([&]{readIndexStorage(missing_link.string());}));
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
    assert(explicit_result.after.backup_bytes==readIndexStorage(path.string()).backup_bytes+fs::file_size(external)+receiptBytes(external));
    const auto corrupt=base/"corrupt.sqlite";
    std::ofstream(corrupt)<<"not a database";
    const auto corrupt_before=bytes(corrupt);
    assert(rejected([&]{readIndexStorage(corrupt.string());}));
    assert(bytes(corrupt)==corrupt_before);
    // SQLite may remove WAL sidecars when the last unrelated reader closes.
    // A valid committed WAL database must still be reportable afterward.
    const auto cold_wal=base/"cold-wal.sqlite";
    db=open(cold_wal);
    sql(db,"CREATE TABLE probe(value TEXT); PRAGMA journal_mode=WAL; INSERT INTO probe VALUES('committed');");
    assert(sqlite3_close(db)==SQLITE_OK);
    // The last writer checkpointed the committed row into the main file.
    if(fs::exists(cold_wal.string()+"-wal")) {
        assert(fs::file_size(cold_wal.string()+"-wal")==0);
        fs::remove(cold_wal.string()+"-wal");
    }
    if(fs::exists(cold_wal.string()+"-shm")) fs::remove(cold_wal.string()+"-shm");
    assert(!fs::exists(cold_wal.string()+"-wal") && !fs::exists(cold_wal.string()+"-shm"));
    const auto sidecar_target=base/"outside-sidecar";
    fs::create_symlink(sidecar_target,cold_wal.string()+"-wal");
    assert(rejected([&]{readIndexStorage(cold_wal.string());}));
    assert(!fs::exists(sidecar_target));
    fs::remove(cold_wal.string()+"-wal");
    const auto cold_original=bytes(cold_wal);
    const auto cold_report=readIndexStorage(cold_wal.string());
    assert(cold_report.page_count>0 && cold_report.page_size>0);
    assert(bytes(cold_wal)==cold_original && "storage reporting must not rewrite the main database");
    db=open(cold_wal);
    assert(scalar(db,"SELECT value FROM probe")=="committed");
    sqlite3_close(db);
    assert(rejected([&]{compactIndexStorage(path.string(),(base/"absent-dir"/"backup").string());}));
    assert(!fs::exists(base/"absent-dir"));

#ifndef _WIN32
    // Cleanup is explicit, exact, schema-aware, and retains a standalone recovery copy.
    const auto cleanup_dir=base/"cleanup";fs::create_directory(cleanup_dir);
    const auto app=cleanup_dir/"app.sqlite";
    Database(app.string()).initialize();
    const auto kept=cleanup_dir/"app.sqlite.pre-storage-1.backup.sqlite";
    const auto deleted=cleanup_dir/"app.sqlite.pre-storage-2.backup.sqlite";
    compactIndexStorage(app.string(),kept.string());compactIndexStorage(app.string(),deleted.string());
    const auto deleted_bytes=fs::file_size(deleted)+receiptBytes(deleted);
    const auto cleanup=removeIndexStorageBackups(app.string(),{deleted.string()});
    if(!cleanup.error.empty())std::cerr<<cleanup.error<<"\n";
    assert(cleanup.error.empty() && cleanup.removed_paths==std::vector<std::string>{deleted.string()});
    assert(cleanup.removed_bytes==deleted_bytes && !fs::exists(deleted) && fs::exists(kept));
    const auto last=removeIndexStorageBackups(app.string(),{kept.string()});
    assert(!last.error.empty() && last.removed_paths.empty() && fs::exists(kept));
    // A valid, unrelated index renamed to the exact backup convention has no
    // creation receipt tying it to this source and must not authorize cleanup.
    const auto provenance_dir=base/"provenance";fs::create_directory(provenance_dir);
    const auto provenance_app=provenance_dir/"index.sqlite";
    Database(provenance_app.string()).initialize();
    const auto trusted=provenance_dir/"index.sqlite.pre-storage-1.backup.sqlite";
    compactIndexStorage(provenance_app.string(),trusted.string());
    const auto other=provenance_dir/"unrelated.sqlite";Database(other.string()).initialize();
    const auto renamed=provenance_dir/"index.sqlite.pre-storage-2.backup.sqlite";
    fs::copy_file(other,renamed);
    const auto unrelated_guard=removeIndexStorageBackups(provenance_app.string(),{trusted.string()});
    assert(!unrelated_guard.error.empty() && unrelated_guard.removed_paths.empty() && fs::exists(trusted) && "a same-name unrelated valid index is not this source's recovery copy");
    const auto receipt=fs::path(trusted.string()+".provenance"),saved_receipt=fs::path(receipt.string()+".saved");
    const auto valid_receipt=bytes(receipt);
#ifndef _WIN32
    struct stat receipt_mode{};assert(::stat(receipt.c_str(),&receipt_mode)==0 && (receipt_mode.st_mode&0777)==0600);
#endif
    const auto provenance_refuses=[&]{const auto refusal=removeIndexStorageBackups(provenance_app.string(),{renamed.string()});assert(!refusal.error.empty() && refusal.removed_paths.empty() && fs::exists(renamed));};
    std::ofstream(receipt,std::ios::trunc)<<"damaged receipt";provenance_refuses();
    std::ofstream(receipt,std::ios::trunc)<<valid_receipt;
    fs::rename(receipt,saved_receipt);fs::create_symlink(saved_receipt,receipt);provenance_refuses();fs::remove(receipt);fs::rename(saved_receipt,receipt);
    fs::rename(receipt,saved_receipt);fs::create_hard_link(saved_receipt,receipt);provenance_refuses();fs::remove(receipt);fs::rename(saved_receipt,receipt);
    fs::rename(receipt,saved_receipt);provenance_refuses();fs::rename(saved_receipt,receipt);
    // Reusing a real receipt for a different source or backup identity fails.
    std::ofstream(receipt,std::ios::trunc)<<bytes(kept.string()+".provenance");provenance_refuses();
    std::ofstream(receipt,std::ios::trunc)<<valid_receipt;
    const auto saved_backup=fs::path(trusted.string()+".saved");fs::rename(trusted,saved_backup);fs::copy_file(other,trusted);provenance_refuses();fs::remove(trusted);fs::rename(saved_backup,trusted);
    const auto saved_main=fs::path(provenance_app.string()+".saved");fs::rename(provenance_app,saved_main);fs::copy_file(trusted,provenance_app);provenance_refuses();fs::remove(provenance_app);fs::rename(saved_main,provenance_app);
    // Receipt creation must stop before VACUUM or migration can commit.
    const auto blocked=provenance_dir/"index.sqlite.pre-storage-9.backup.sqlite";
    std::ofstream(blocked.string()+".provenance")<<"existing unrelated file";
    const auto before_blocked=bytes(provenance_app);
    std::vector<IndexStoragePhase> receipt_failure_phases;
    assert(rejected([&]{compactIndexStorage(provenance_app.string(),blocked.string(),nullptr,[&](IndexStoragePhase phase){receipt_failure_phases.push_back(phase);});}));
    assert((receipt_failure_phases==std::vector<IndexStoragePhase>{IndexStoragePhase::Backup,IndexStoragePhase::BackupVerified}) && "verified backup completion must be observable before provenance creation can fail");
    assert(bytes(provenance_app)==before_blocked && fs::exists(blocked));
    assert(bytes(blocked.string()+".provenance")=="existing unrelated file");
    const auto blocked_migration=provenance_dir/"index.sqlite.pre-storage-10.backup.sqlite";
    fs::create_symlink(receipt,blocked_migration.string()+".provenance");
    const auto stopped_migration=migrateIndexStorageToDirectory(provenance_app.string(),blocked_migration.string());
    assert(!stopped_migration.error.empty() && !stopped_migration.migration_committed && fs::exists(blocked_migration));
    db=open(provenance_app);assert(scalar(db,"PRAGMA user_version")=="413");sqlite3_close(db);
    // Newline paths round-trip as safe hex fields, without receipt parsing ambiguity.
    const auto newline_app=base/"newline-source\n.sqlite";Database(newline_app.string()).initialize();
    const auto newline_backup=fs::path(newline_app.string()+".pre-storage-1.backup.sqlite");
    compactIndexStorage(newline_app.string(),newline_backup.string());
    const auto newline_extra=fs::path(newline_app.string()+".pre-storage-2.backup.sqlite");
    compactIndexStorage(newline_app.string(),newline_extra.string());
    const auto newline_cleanup=removeIndexStorageBackups(newline_app.string(),{newline_extra.string()});
    assert(newline_cleanup.error.empty() && !fs::exists(newline_extra) && !fs::exists(newline_extra.string()+".provenance"));
    // Orphan receipts are excluded from normal stats once their paired DB is absent.
    const auto receipt_stats=readIndexStorage(provenance_app.string());
    std::ofstream(provenance_dir/"index.sqlite.pre-storage-999.backup.sqlite.provenance")<<"orphan";
    assert(readIndexStorage(provenance_app.string()).backup_bytes==receipt_stats.backup_bytes);
    // Replacing the source pathname after the old SQLite connection opens
    // cannot bind that old snapshot to the replacement database's identity.
    for(const bool migrate:{false,true}) {
        const auto race_dir=base/(migrate?"migration-source-race":"compact-source-race");fs::create_directory(race_dir);
        const auto live=race_dir/"index.sqlite",old_live=race_dir/"old-index.sqlite",fresh=race_dir/"replacement.sqlite";
        Database(live.string()).initialize();Database(fresh.string()).initialize();
        db=open(fresh);sql(db,"INSERT INTO app_metadata VALUES('synthetic_marker','replacement')");sqlite3_close(db);
        const auto replacement_bytes=bytes(fresh);const auto race_backup=race_dir/"index.sqlite.pre-storage-1.backup.sqlite";
        bool replaced=false;
        const auto replace_source=[&](IndexStoragePhase phase){if(phase==IndexStoragePhase::Backup){fs::rename(live,old_live);fs::copy_file(fresh,live);replaced=true;}};
        if(migrate){const auto failure=migrateIndexStorageToDirectory(live.string(),race_backup.string(),true,nullptr,replace_source);assert(!failure.error.empty() && !failure.migration_committed);}
        else assert(rejected([&]{compactIndexStorage(live.string(),race_backup.string(),nullptr,replace_source);}));
        assert(replaced && fs::exists(race_backup) && !fs::exists(race_backup.string()+".provenance") && "a changed source identity must never receive a trusted receipt");
        assert(bytes(live)==replacement_bytes && "maintenance must preserve the replacement database");
    }
    // Generic SQLite integrity is insufficient: an empty or unrelated database
    // cannot replace the last verified application recovery backup.
    const auto empty=cleanup_dir/"app.sqlite.pre-storage-3.backup.sqlite";
    db=open(empty);sql(db,"CREATE TABLE unrelated(value TEXT)");sqlite3_close(db);
    const auto no_recovery=removeIndexStorageBackups(app.string(),{kept.string()});
    assert(!no_recovery.error.empty() && no_recovery.removed_paths.empty() && fs::exists(kept));
    const auto same_prefix=cleanup_dir/"app.sqlite.pre-storage-9.backup.sqlite.extra";
    std::ofstream(same_prefix)<<"unrelated";
    assert(!removeIndexStorageBackups(app.string(),{same_prefix.string()}).error.empty());
    assert(fs::exists(same_prefix));
    const auto cleanup_link=cleanup_dir/"app.sqlite.pre-storage-4.backup.sqlite";
    fs::create_symlink(kept,cleanup_link);
    assert(!removeIndexStorageBackups(app.string(),{cleanup_link.string()}).error.empty());
    fs::remove(cleanup_link);fs::create_hard_link(kept,cleanup_link);
    assert(!removeIndexStorageBackups(app.string(),{cleanup_link.string()}).error.empty());
    fs::remove(cleanup_link);
    const auto list=listIndexStorageBackups(app.string());
    assert(list.size()==2 && "listing excludes arbitrary same-prefix files and aliases");
    fs::create_directory_symlink(cleanup_dir,base/"cleanup-alias");
    assert(!removeIndexStorageBackups(app.string(),{(base/"cleanup-alias"/kept.filename()).string()}).error.empty());
    assert(!removeIndexStorageBackups(app.string(),{app.string()}).error.empty());
    assert(!removeIndexStorageBackups(app.string(),{(app.string()+"-wal")}).error.empty());
    std::atomic_bool cleanup_cancel{true};
    const auto cancelled_cleanup=removeIndexStorageBackups(app.string(),{empty.string()},&cleanup_cancel);
    assert(cancelled_cleanup.cancelled && cancelled_cleanup.removed_paths.empty() && fs::exists(empty));
    {
        IndexMaintenanceLock locked(app.string());
        const auto busy_cleanup=removeIndexStorageBackups(app.string(),{empty.string()});
        assert(!busy_cleanup.error.empty() && busy_cleanup.removed_paths.empty() && fs::exists(empty));
    }
    // Integrity can remain ok while initialize would still repair required
    // indexes or FTS readiness. Such a copy cannot be the sole recovery guard.
    const std::vector<std::string> recovery_damage={
        "DROP INDEX idx_files_search_name",
        "UPDATE app_metadata SET value='0' WHERE key='schema_413_fts_available'",
        "UPDATE app_metadata SET value='1' WHERE key='name_fts_ready'; DROP TRIGGER IF EXISTS files_name_fts_ai"
    };
    for(std::size_t i=0;i<recovery_damage.size();++i) {
        const auto check_dir=cleanup_dir/("readiness-"+std::to_string(i));fs::create_directory(check_dir);
        const auto check_app=check_dir/"index.sqlite";
        const auto healthy=check_dir/"index.sqlite.pre-storage-1.backup.sqlite";
        const auto damaged=check_dir/"index.sqlite.pre-storage-2.backup.sqlite";
        for(const auto& target:{check_app,healthy,damaged})fs::copy_file(kept,target);
        db=open(damaged);sql(db,recovery_damage[i].c_str());
        assert(scalar(db,"PRAGMA integrity_check")=="ok");sqlite3_close(db);
        const auto healthy_content=bytes(healthy),damaged_content=bytes(damaged);
        const auto refused=removeIndexStorageBackups(check_app.string(),{healthy.string()});
        assert(!refused.error.empty() && refused.removed_paths.empty() && fs::exists(healthy) && "an incomplete retained application backup must not authorize deleting the last ready copy");
        assert(bytes(healthy)==healthy_content && bytes(damaged)==damaged_content && "recovery verification must not initialize or repair backups");
    }
    // A damaged current application database also refuses every deletion.
    db=open(app);sql(db,"DROP TABLE exclusion_policy");sqlite3_close(db);
    const auto bad_current=removeIndexStorageBackups(app.string(),{empty.string()});
    assert(!bad_current.error.empty() && bad_current.removed_paths.empty() && fs::exists(empty));
    fs::copy_file(kept,app,fs::copy_options::overwrite_existing);
    // Cancellation after one real removal returns that removal, never a fake rollback.
    const auto extra=cleanup_dir/"app.sqlite.pre-storage-5.backup.sqlite";
    compactIndexStorage(app.string(),extra.string());cleanup_cancel=false;
    const auto partial=removeIndexStorageBackups(app.string(),{empty.string(),extra.string()},&cleanup_cancel,
        [&](std::size_t removed){assert(removed==1);cleanup_cancel=true;});
    assert(partial.cancelled && partial.removed_paths==std::vector<std::string>{empty.string()});
    assert(partial.removed_bytes>0 && !fs::exists(empty) && fs::exists(extra) && fs::exists(kept));
    // The same cleanup guard accepts the new format and preserves its legacy recovery copy.
    const auto single_copy=cleanup_dir/"single-copy.sqlite";
    Database(single_copy.string()).initialize();
    const auto single_optimized=migrateIndexStorageToDirectory(single_copy.string(),"");
    assert(single_optimized.error.empty() && single_optimized.migration_committed);
    assert(single_optimized.after.backup_count==0 && single_optimized.backup_bytes==0);
    Database(single_copy.string()).initialize();
    const auto removable=cleanup_dir/"single-copy.sqlite.pre-storage-1.backup.sqlite";
    compactIndexStorage(single_copy.string(),removable.string());
    const auto removed_last=removeIndexStorageBackups(single_copy.string(),{removable.string()},nullptr,{},true);
    assert(removed_last.error.empty() && removed_last.removed_paths.size()==1);
    assert(readIndexStorage(single_copy.string()).backup_count==0);
    const auto optimized=migrateIndexStorageToDirectory(app.string(),(cleanup_dir/"app.sqlite.pre-storage-6.backup.sqlite").string());
    assert(optimized.error.empty() && optimized.migration_committed);
    const auto no_backup_optimize=migrateIndexStorageToDirectory(app.string(),"");
    assert(no_backup_optimize.error.empty() && no_backup_optimize.already_compact && no_backup_optimize.backup_bytes==0);
    const auto compact_cleanup=removeIndexStorageBackups(app.string(),{extra.string()});
    assert(compact_cleanup.error.empty() && compact_cleanup.removed_paths.size()==1 && fs::exists(kept));
    std::vector<IndexStoragePhase> compact_phases;
    compactIndexStorage(app.string(),(cleanup_dir/"app.sqlite.pre-storage-7.backup.sqlite").string(),nullptr,
        [&](IndexStoragePhase phase){compact_phases.push_back(phase);});
    assert((compact_phases==std::vector<IndexStoragePhase>{IndexStoragePhase::Backup,IndexStoragePhase::BackupVerified,IndexStoragePhase::Vacuum,IndexStoragePhase::Verify}));
    // The compact-format guard is the same predicate used by initialize.
    for(const std::string damage:{"DROP INDEX idx_entries_search_name","DROP TRIGGER directory_guard_file_entries_update","DROP TRIGGER files_name_fts_ai"}) {
        const auto check_dir=cleanup_dir/("directory-readiness-"+std::to_string(std::hash<std::string>{}(damage)));fs::create_directory(check_dir);
        const auto check_app=check_dir/"index.sqlite",healthy=check_dir/"index.sqlite.pre-storage-1.backup.sqlite",damaged=check_dir/"index.sqlite.pre-storage-2.backup.sqlite";
        for(const auto& target:{check_app,healthy,damaged})fs::copy_file(cleanup_dir/"app.sqlite.pre-storage-7.backup.sqlite",target);
        db=open(damaged);sql(db,damage.c_str());assert(scalar(db,"PRAGMA integrity_check")=="ok");sqlite3_close(db);
        const auto damaged_content=bytes(damaged);
        const auto refused=removeIndexStorageBackups(check_app.string(),{healthy.string()});
        assert(!refused.error.empty() && refused.removed_paths.empty() && fs::exists(healthy));
        assert(bytes(damaged)==damaged_content);
    }
    // The successful-VACUUM phase is observable even when cancellation lands
    // exactly before final verification. No rollback is claimed for this case.
    std::atomic_bool cancel_verification{false};bool reached_vacuum=false,reached_verify=false;
    const auto partial_backup=cleanup_dir/"app.sqlite.pre-storage-8.backup.sqlite";
    assert(rejected([&]{compactIndexStorage(app.string(),partial_backup.string(),&cancel_verification,[&](IndexStoragePhase phase){
        if(phase==IndexStoragePhase::Vacuum)reached_vacuum=true;
        if(phase==IndexStoragePhase::Verify){reached_verify=true;cancel_verification=true;}
    });}));
    assert(reached_vacuum && reached_verify && fs::exists(partial_backup) && fs::exists(partial_backup.string()+".provenance"));
    assert(readIndexStorage(app.string()).freelist_bytes==0);

#else
    const auto unsupported_cleanup=removeIndexStorageBackups(path.string(),{backup.string()});
    assert(!unsupported_cleanup.error.empty() && unsupported_cleanup.removed_paths.empty());
#endif
    fs::remove_all(base);
    std::cout<<"index storage tests passed\n";
}
