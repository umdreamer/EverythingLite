#include "core/database.h"
#include "core/scan_request.h"
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <dlfcn.h>
using namespace everything_lite;
namespace fs=std::filesystem;
struct Connection {
    sqlite3* value=nullptr;
    explicit Connection(const std::string& path){assert(sqlite3_open(path.c_str(),&value)==SQLITE_OK);sqlite3_busy_timeout(value,5000);}
    ~Connection(){assert(sqlite3_close(value)==SQLITE_OK);}
    void exec(const std::string& query){char* error=nullptr;const int rc=sqlite3_exec(value,query.c_str(),nullptr,nullptr,&error);if(rc!=SQLITE_OK)std::cerr<<(error?error:"")<<"\n";sqlite3_free(error);assert(rc==SQLITE_OK);}
};
void sql(const std::string& path,const std::string& query){Connection db(path);db.exec(query);}
std::string scalar(const std::string& path,const std::string& query) {
    Connection db(path);sqlite3_stmt* statement=nullptr;assert(sqlite3_prepare_v2(db.value,query.c_str(),-1,&statement,nullptr)==SQLITE_OK);
    assert(sqlite3_step(statement)==SQLITE_ROW);const auto* bytes=sqlite3_column_text(statement,0);
    std::string result=bytes?reinterpret_cast<const char*>(bytes):"";sqlite3_finalize(statement);return result;
}
std::string state(const Database& db) {
    Connection connection(db.path());std::string result;
    for(const auto& query:{"SELECT * FROM files ORDER BY id","SELECT * FROM sources ORDER BY id","SELECT * FROM roots ORDER BY root","SELECT * FROM audit"}) {
        sqlite3_stmt* statement=nullptr;assert(sqlite3_prepare_v2(connection.value,query,-1,&statement,nullptr)==SQLITE_OK);
        int rc;while((rc=sqlite3_step(statement))==SQLITE_ROW)for(int i=0;i<sqlite3_column_count(statement);++i){const auto* bytes=sqlite3_column_text(statement,i);const int size=sqlite3_column_bytes(statement,i);result+=std::to_string(size)+":";if(bytes)result.append(reinterpret_cast<const char*>(bytes),size);}assert(rc==SQLITE_DONE);sqlite3_finalize(statement);result+=';';
    }
    return result;
}
template<class F> bool rejects(F action){try{action();return false;}catch(const std::exception&){return true;}}
std::string field(const std::string& value){return std::to_string(value.size())+":"+value;}
struct Fixture {
    fs::path base;Database db;std::int64_t id=0;int sequence=0;
    Fixture():base(makeBase()),db((base/"output"/"main.sqlite").string()) {
        fs::create_directory(base/"root");fs::create_directory(base/"output");assert(chmod((base/"output").c_str(),0700)==0);
        fs::create_directory(base/"root"/"sample");fs::create_directory(base/"root"/"sample-other");
        std::ofstream(base/"root"/"sample"/"inside.txt")<<"synthetic";std::ofstream(base/"root"/"sample-other"/"outside.txt")<<"synthetic";
        for(int i=0;i<1105;++i)std::ofstream(base/"root"/("item-"+std::to_string(i)+".txt"))<<"synthetic";
        db.initialize();SourceRecord source;source.root=(base/"root").string();source.status=SourceStatus::Accessible;source.type=SourceType::Local;source.label="synthetic source";source.last_check=11;source.last_success=10;
        auto evidence=beginSourceProbe(source).result();assert(!evidence.identity.empty());source.identity=evidence.identity;source.mount_fingerprint=evidence.mount_fingerprint;id=db.saveSource(source);
        sql(db.path(),"CREATE TABLE audit(writes INTEGER);INSERT INTO audit VALUES(0);CREATE TRIGGER audit_i AFTER INSERT ON files BEGIN UPDATE audit SET writes=writes+1;END;CREATE TRIGGER audit_u AFTER UPDATE ON files BEGIN UPDATE audit SET writes=writes+1;END;CREATE TRIGGER audit_d AFTER DELETE ON files BEGIN UPDATE audit SET writes=writes+1;END;");
    }
    static fs::path makeBase(){char pattern[]="/private/tmp/everything-lite-source-snapshot-XXXXXX";auto* created=mkdtemp(pattern);assert(created);return fs::path(created);}
    ~Fixture(){fs::remove_all(base);}
    SourceScanRequest capture(const std::string& scope={}){return db.captureSourceScanRequest(id,scope,(base/"output"/("snapshot-"+std::to_string(sequence++)+".sqlite")).string(),"synthetic-nonce");}
    SourceScanRequest scanned(const std::string& scope={}){auto r=capture(scope);auto outcome=scanSourceToSnapshot(r);if(!outcome.stats.completed)std::cerr<<outcome.stats.error<<"\n";assert(outcome.stats.completed);return r;}
    std::int64_t nested(const std::string& root){SourceRecord other;other.root=root;other.status=SourceStatus::Offline;other.label="nested synthetic";return db.saveSource(other);}
    void rejectUnchanged(const SourceScanRequest& request){const auto before=state(db);assert(rejects([&]{db.mergeSourceScanSnapshot(request);}) && "malformed or stale snapshot must reject");assert(state(db)==before && "failed admission must roll back files and source metadata");}
};
void basicAndMalformed(Fixture& f) {
    bool admitted=false;try{auto r=f.scanned();admitted=f.db.mergeSourceScanSnapshot(r).completed;}catch(const std::exception&){}
    assert(admitted && "valid native captured snapshot must be admitted");
    sql(f.db.path(),"UPDATE audit SET writes=0");auto unchanged=f.scanned();assert(f.db.mergeSourceScanSnapshot(unchanged).completed);assert(scalar(f.db.path(),"SELECT writes FROM audit")=="0");
    const auto old_mtime=scalar(f.db.path(),"SELECT modified_time FROM files WHERE relative_path='.'");fs::remove(f.base/"root"/"item-0.txt");auto deleted=f.scanned();const auto delta=f.db.mergeSourceScanSnapshot(deleted);assert(delta.completed && delta.removed==1);
    const auto new_mtime=scalar(f.db.path(),"SELECT modified_time FROM files WHERE relative_path='.'");assert(std::stoll(scalar(f.db.path(),"SELECT writes FROM audit"))==1+(old_mtime!=new_mtime));
    const auto last_success=f.db.source(f.id).last_success;fs::remove(f.base/"root"/"sample"/"inside.txt");auto subtree=f.scanned((f.base/"root"/"sample").string());auto sub=f.db.mergeSourceScanSnapshot(subtree);assert(sub.completed && sub.removed==1);assert(f.db.source(f.id).last_success==last_success);assert(f.db.search("outside.txt").size()==1);
    const std::vector<std::string> mutations={
        "UPDATE snapshot_proof SET nonce='wrong'","UPDATE snapshot_metadata SET complete=0","DELETE FROM snapshot_proof","INSERT INTO snapshot_proof SELECT * FROM snapshot_proof",
        "UPDATE snapshot_proof SET actual_scope=actual_scope||'/sample'","UPDATE snapshot_proof SET source_id=source_id+1","UPDATE snapshot_proof SET source_revision=source_revision+1","UPDATE snapshot_proof SET policy_revision=policy_revision+1",
        "UPDATE snapshot_proof SET binding=binding||'/sample'","UPDATE snapshot_proof SET identity='wrong'","UPDATE snapshot_proof SET mount_fingerprint='wrong'","UPDATE snapshot_proof SET canonical_root=canonical_root||'/sample'","UPDATE snapshot_proof SET subtree_relative='.'",
        "UPDATE snapshot_proof SET nonce=CAST(nonce AS BLOB)","UPDATE snapshot_proof SET source_id=1.5","INSERT INTO snapshot_metadata SELECT * FROM snapshot_metadata",
        "UPDATE snapshot_files SET path=path||'/..' WHERE rowid=1","UPDATE snapshot_files SET root=root||'/sample' WHERE rowid=1","UPDATE snapshot_files SET source_id=source_id+1 WHERE rowid=1","UPDATE snapshot_files SET relative_path='wrong' WHERE rowid=1",
        "UPDATE snapshot_files SET name='' WHERE rowid=1","UPDATE snapshot_files SET name=CAST(name AS BLOB) WHERE rowid=1","UPDATE snapshot_files SET name=name||char(0)||'hidden' WHERE rowid=1","UPDATE snapshot_files SET size=-1 WHERE rowid=1","UPDATE snapshot_files SET size=1.5 WHERE rowid=1","UPDATE snapshot_files SET is_dir=2 WHERE rowid=1","UPDATE snapshot_files SET cloud_copy=3 WHERE rowid=1","UPDATE snapshot_files SET scan_generation=1 WHERE rowid=1",
        "UPDATE snapshot_files SET parent_path='/wrong' WHERE rowid=1","UPDATE snapshot_files SET ext=ext||'/bad' WHERE rowid=1",
        "ALTER TABLE snapshot_files RENAME TO old_files;CREATE TABLE snapshot_files AS SELECT * FROM old_files;UPDATE snapshot_files SET name=NULL WHERE rowid=1",
        "ALTER TABLE snapshot_metadata RENAME TO old_metadata;CREATE TABLE snapshot_metadata(complete);INSERT INTO snapshot_metadata VALUES('1')",
        "UPDATE snapshot_files SET size=size+1 WHERE rowid<=1000;UPDATE snapshot_files SET size=-1 WHERE rowid=1001",
        "ALTER TABLE snapshot_files RENAME TO old_files;CREATE TABLE snapshot_files AS SELECT * FROM old_files;INSERT INTO snapshot_files SELECT * FROM old_files WHERE rowid=1",
        "ALTER TABLE snapshot_files RENAME TO old_files;CREATE VIEW snapshot_files AS SELECT path,parent_path,name,ext,root,CASE WHEN rowid=1001 THEN abs(-9223372036854775808) ELSE size+1 END AS size,modified_time,is_dir,scan_generation,source_id,relative_path,cloud_copy FROM old_files",
        "INSERT INTO snapshot_nested_roots VALUES('/wrong')","INSERT INTO snapshot_child_mounts VALUES('/wrong')","UPDATE snapshot_proof SET boundary_fingerprint='broken'"
    };
    for(const auto& mutation:mutations){auto r=f.scanned();sql(r.snapshot_path,mutation);f.rejectUnchanged(r);}
    auto negative=f.scanned();sql(negative.snapshot_path,"UPDATE snapshot_files SET modified_time=-1 WHERE relative_path='item-1.txt'");assert(f.db.mergeSourceScanSnapshot(negative).completed);
    auto policy_capture=f.scanned();ExclusionRules rules;rules.file_filter="*.tmp";f.db.saveExclusionPolicy(rules);f.rejectUnchanged(policy_capture);
    auto source_capture=f.scanned();auto source=f.db.source(f.id);source.label="changed label";f.db.saveSource(source);f.rejectUnchanged(source_capture);
    fs::rename(f.base/"root",f.base/"held-root");
    auto r=f.capture();const auto fixed_source=f.db.source(f.id);
    assert(r.source.policy_revision==r.policy.revision && r.scan_path==r.source.root && !fs::exists(r.snapshot_path));
    assert(r.source.label==fixed_source.label && r.source.type==fixed_source.type && r.source.mode==fixed_source.mode && r.source.status==fixed_source.status && r.source.interval_seconds==fixed_source.interval_seconds && r.source.identity==fixed_source.identity && r.source.mount_fingerprint==fixed_source.mount_fingerprint && r.source.last_check==fixed_source.last_check && r.source.last_success==fixed_source.last_success && r.source.revision==fixed_source.revision);
    fs::rename(f.base/"held-root",f.base/"root");
    assert(rejects([&]{f.db.captureSourceScanRequest(0,"",r.snapshot_path,r.nonce);}));assert(rejects([&]{f.db.captureSourceScanRequest(f.id,"/outside",r.snapshot_path,r.nonce);}));assert(rejects([&]{f.db.captureSourceScanRequest(f.id,"",r.snapshot_path,"");}));assert(rejects([&]{f.db.captureSourceScanRequest(f.id,"",r.source.root+"/output",r.nonce);}));
}
void nestedTopology(Fixture& f) {
    auto stale=f.scanned();const auto revision=f.db.source(f.id).revision;const auto nested=f.nested((f.base/"root"/"sample").string());assert(f.db.source(f.id).revision==revision);f.rejectUnchanged(stale);
    auto valid=f.scanned();assert(valid.nested_source_roots==std::vector<std::string>{(f.base/"root"/"sample").string()});assert(f.db.mergeSourceScanSnapshot(valid).completed);assert(f.db.search("outside.txt").size()==1);
    assert(rejects([&]{f.capture((f.base/"root"/"sample").string());}));
    auto moved=f.scanned();const auto before_move=f.db.source(f.id).revision;auto child=f.db.source(nested);child.root=(f.base/"root"/"sample-other").string();f.db.saveSource(child);assert(f.db.source(f.id).revision==before_move);f.rejectUnchanged(moved);
    auto forgot=f.scanned();const auto before_forget=f.db.source(f.id).revision;f.db.forgetSource(nested);assert(f.db.source(f.id).revision==before_forget);f.rejectUnchanged(forgot);
    f.nested((f.base/"root").string());f.nested((f.base/"root-other").string());auto same=f.scanned();assert(same.nested_source_roots.empty());assert(f.db.mergeSourceScanSnapshot(same).completed);
    const auto offline=f.nested((f.base/"root"/"sample").string());auto duplicate=f.nested((f.base/"root"/"sample").string());(void)duplicate;
    for(auto status:{SourceStatus::Offline,SourceStatus::Unverified,SourceStatus::Failed}){auto child=f.db.source(offline);child.status=status;f.db.saveSource(child);auto current=f.scanned();assert(current.nested_source_roots.size()==1);assert(f.db.mergeSourceScanSnapshot(current).completed);}
}
void emptySnapshot(Fixture& f) {
    ExclusionRules excluded;excluded.directories={ (f.base/"root").string() };f.db.saveExclusionPolicy(excluded);
    // Compatibility setup seeds cache after policy replacement to exercise a
    // complete empty staging snapshot deleting existing canonical records.
    sql(f.db.path(),"INSERT INTO files(source_id,relative_path,path,parent_path,name,ext,root,size,modified_time,is_dir,search_name,search_path,scan_generation,cloud_copy) SELECT id,'stale',root||'/stale',root,'stale','',root,1,0,0,'stale',root||'/stale',0,2 FROM sources WHERE id="+std::to_string(f.id));
    auto empty=f.scanned();assert(scalar(empty.snapshot_path,"SELECT COUNT(*) FROM snapshot_files")=="0");auto merged=f.db.mergeSourceScanSnapshot(empty);assert(merged.completed && merged.removed==1);
}
void boundaryDecoder(Fixture& f) {
    const auto canonical=capturedSourceCanonicalRoot(f.db.source(f.id).mount_fingerprint);const auto root=canonical+"/child";
    const auto mount=field("/synthetic-mount")+field("apfs")+field("synthetic-uuid")+field("local")+field("synthetic-token");
    const auto proof=field(field(root)+field(mount));assert(capturedChildMountRoots(proof,canonical)==std::vector<std::string>{root});
    for(const auto& bad:{std::string("1:x"),proof+"garbage",field(field(canonical)+field(mount)),field(field("/outside")+field(mount)),field(field(root)+field(mount+"0:")),field(field(root)+field(field("/synthetic-mount")+field("")+field("")+field("local")+field("token")))})assert(rejects([&]{capturedChildMountRoots(bad,canonical);}));
    auto r=f.scanned();sql(r.snapshot_path,"UPDATE snapshot_proof SET boundary_fingerprint='"+proof+"';INSERT INTO snapshot_child_mounts VALUES('"+root+"')");assert(f.db.mergeSourceScanSnapshot(r).completed);
    auto mismatched=f.scanned();sql(mismatched.snapshot_path,"UPDATE snapshot_proof SET boundary_fingerprint='"+proof+"'");f.rejectUnchanged(mismatched);
}
// Decode only the known-valid native parent's small captured evidence to
// supply the producer's RootEvidence. Child proofs themselves are generated
// by evaluateSourceEvidence, never by a parallel test implementation.
std::vector<std::string> nativeFields(const std::string& fingerprint) {
    std::vector<std::string> fields;std::size_t offset=0;
    for(int i=0;i<7;++i){const auto colon=fingerprint.find(':',offset);assert(colon!=std::string::npos);const auto size=std::stoull(fingerprint.substr(offset,colon-offset));offset=colon+1;assert(size<=fingerprint.size()-offset);fields.push_back(fingerprint.substr(offset,size));offset+=size;}
    assert(offset==fingerprint.size());return fields;
}
void installChildProof(const SourceScanRequest& request,const ProbeResult& evidence) {
    Connection db(request.snapshot_path);sqlite3_stmt* statement=nullptr;
    assert(sqlite3_prepare_v2(db.value,"UPDATE snapshot_proof SET boundary_fingerprint=?",-1,&statement,nullptr)==SQLITE_OK);
    assert(sqlite3_bind_text(statement,1,evidence.boundary_fingerprint.data(),static_cast<int>(evidence.boundary_fingerprint.size()),SQLITE_TRANSIENT)==SQLITE_OK);assert(sqlite3_step(statement)==SQLITE_DONE);sqlite3_finalize(statement);
    db.exec("DELETE FROM snapshot_child_mounts");
    assert(sqlite3_prepare_v2(db.value,"INSERT INTO snapshot_child_mounts VALUES(?)",-1,&statement,nullptr)==SQLITE_OK);
    for(const auto& boundary:evidence.child_mount_boundaries){sqlite3_reset(statement);assert(sqlite3_bind_text(statement,1,boundary.data(),static_cast<int>(boundary.size()),SQLITE_TRANSIENT)==SQLITE_OK);assert(sqlite3_step(statement)==SQLITE_DONE);}sqlite3_finalize(statement);
}
void unidentifiedChildProof(Fixture& f) {
    const auto saved=f.db.source(f.id);const auto fields=nativeFields(saved.mount_fingerprint);assert(fields[3]=="local");
    RootEvidence root;root.canonical_root=fields[5];root.directory_token=fields[6];
    root.actual_mount.mount_point=fields[0];root.actual_mount.filesystem=fields[1];root.actual_mount.volume_uuid=fields[2];root.actual_mount.mount_token=fields[4];root.actual_mount.local=true;root.actual_mount.internal=true;
    for(const auto& filesystem:{std::string("unsupportedfs"),std::string("smbfs")}) {
        MountEvidence child;child.mount_point=root.canonical_root+"/unknown-child";child.filesystem=filesystem;child.raw_remote_source="//host/share%";child.mount_token="synthetic-child-token";child.local=false;
        assert(sanitizeRemoteSource(child.raw_remote_source).empty());
        const auto produced=evaluateSourceEvidence(saved,root,{root.actual_mount,child});
        assert(produced.update_allowed && produced.identity==saved.identity && produced.mount_fingerprint==saved.mount_fingerprint && produced.child_mount_boundaries==std::vector<std::string>{child.mount_point});
        auto request=f.scanned();installChildProof(request,produced);
        bool accepted=false;try{accepted=f.db.mergeSourceScanSnapshot(request).completed;}catch(const std::exception& error){std::cerr<<"child proof admission: "<<error.what()<<"\n";}
        assert(accepted && "verified native parent must admit producer evidence pruning an unidentified child mount");
        assert(capturedChildMountRoots(produced.boundary_fingerprint,root.canonical_root)==produced.child_mount_boundaries);
        // An unidentified child still enforces its pruning boundary. A late
        // crossing row rolls back the first 1000 changed canonical records.
        auto crossing=f.scanned();installChildProof(crossing,produced);const auto binding=crossing.source.root+"/unknown-child";
        sql(crossing.snapshot_path,"UPDATE snapshot_files SET size=size+1 WHERE rowid<=1000;UPDATE snapshot_files SET path='"+binding+"/leak.txt',parent_path='"+binding+"',name='leak.txt',ext='txt',relative_path='unknown-child/leak.txt' WHERE rowid=1001");f.rejectUnchanged(crossing);
    }
    const auto credential_mount=field(root.canonical_root+"/unknown-child")+field("smbfs")+field("")+field("synthetic-user:synthetic-password@host/share")+field("synthetic-child-token");
    assert(rejects([&]{capturedChildMountRoots(field(field(root.canonical_root+"/unknown-child")+field(credential_mount)),root.canonical_root);}));
    std::string invalid_own;for(std::size_t i=0;i<fields.size();++i)invalid_own+=field(i==3?std::string{}:fields[i]);
    assert(rejects([&]{capturedSourceCanonicalRoot(invalid_own);}) && "own source evidence must retain strict remote/local validation");
}
struct StagingSequence {
    std::string path;std::mutex mutex;std::condition_variable changed;
    bool requested=false,committed=false;std::atomic_bool fired{false};
};
StagingSequence* staging_sequence=nullptr;
int stageTrace(unsigned kind,void*,void* statement,void*) {
    auto* sequence=staging_sequence;if(!sequence || kind!=SQLITE_TRACE_STMT)return 0;
    auto* stmt=static_cast<sqlite3_stmt*>(statement);const auto* query=sqlite3_sql(stmt);const auto* path=sqlite3_db_filename(sqlite3_db_handle(stmt),"main");
    if(!query || !path || sequence->path!=path || std::string(query).find("SELECT nonce,source_id")!=0 || sequence->fired.exchange(true))return 0;
    std::unique_lock<std::mutex> guard(sequence->mutex);sequence->requested=true;sequence->changed.notify_all();
    sequence->changed.wait(guard,[&]{return sequence->committed;});return 0;
}
// Link interception is confined to this test executable. Apple system SQLite
// rejects process-global auto extensions; production has no testing hook.
extern "C" int sqlite3_open_v2(const char* path,sqlite3** db,int flags,const char* vfs) {
    using Open=int(*)(const char*,sqlite3**,int,const char*);
    static const auto native=reinterpret_cast<Open>(dlsym(RTLD_NEXT,"sqlite3_open_v2"));assert(native);
    const int rc=native(path,db,flags,vfs);
    if(rc==SQLITE_OK && staging_sequence)assert(sqlite3_trace_v2(*db,SQLITE_TRACE_STMT,stageTrace,nullptr)==SQLITE_OK);
    return rc;
}
void pinnedStagingRead(Fixture& f) {
    auto request=f.scanned();sql(request.snapshot_path,"PRAGMA journal_mode=WAL");
    const auto original_size=scalar(request.snapshot_path,"SELECT size FROM snapshot_files WHERE relative_path='item-1.txt'");
    StagingSequence sequence;sequence.path=request.snapshot_path;staging_sequence=&sequence;
    std::thread producer([&]{
        Connection db(request.snapshot_path);std::unique_lock<std::mutex> guard(sequence.mutex);
        sequence.changed.wait(guard,[&]{return sequence.requested;});guard.unlock();
        db.exec("BEGIN IMMEDIATE;UPDATE snapshot_proof SET nonce='late-producer-nonce';UPDATE snapshot_files SET size=size+100;COMMIT");
        guard.lock();sequence.committed=true;sequence.changed.notify_all();
    });
    assert(f.db.mergeSourceScanSnapshot(request).completed);producer.join();
    assert(sequence.fired && sequence.committed);staging_sequence=nullptr;
    assert(scalar(request.snapshot_path,"SELECT nonce FROM snapshot_proof")=="late-producer-nonce");
    assert(scalar(request.snapshot_path,"SELECT size FROM snapshot_files WHERE relative_path='item-1.txt'")!=original_size);
    assert(scalar(f.db.path(),"SELECT size FROM files WHERE relative_path='item-1.txt' AND source_id="+std::to_string(f.id))==original_size);
}
void consistentCapture(Fixture& f) {
    const auto child=f.nested((f.base/"root"/"sample").string());
    auto update=[&](Connection& db,int epoch){
        const auto root=(f.base/"root"/(epoch%2?"sample":"sample-other")).string();
        const auto excluded=(f.base/"root"/("epoch-"+std::to_string(epoch))).string();
        db.exec("BEGIN IMMEDIATE;UPDATE sources SET label='epoch-"+std::to_string(epoch)+"' WHERE id="+std::to_string(f.id)+";UPDATE exclusion_policy SET revision="+std::to_string(epoch)+",file_filter='epoch-"+std::to_string(epoch)+"';DELETE FROM exclusion_paths;INSERT INTO exclusion_paths VALUES(1,0,'"+excluded+"');UPDATE sources SET root='"+root+"' WHERE id="+std::to_string(child)+";COMMIT");
    };
    {Connection db(f.db.path());update(db,100);}
    std::atomic_bool stop{false};std::atomic_int writes{0};
    std::thread producer([&]{Connection db(f.db.path());int epoch=101;while(!stop.load()){update(db,epoch++);++writes;}});
    for(int i=0;i<200;++i){
        const auto r=f.capture();const auto epoch=r.policy.revision;
        assert(r.source.policy_revision==epoch && r.source.label=="epoch-"+std::to_string(epoch));
        assert(r.policy.rules().file_filter==r.source.label && r.policy.rules().files==std::vector<std::string>{(f.base/"root"/("epoch-"+std::to_string(epoch))).string()});
        assert(r.nested_source_roots==std::vector<std::string>{(f.base/"root"/(epoch%2?"sample":"sample-other")).string()});
    }
    stop=true;producer.join();assert(writes>0);
}
int main(){Fixture f;basicAndMalformed(f);nestedTopology(f);boundaryDecoder(f);unidentifiedChildProof(f);pinnedStagingRead(f);emptySnapshot(f);Fixture concurrent;consistentCapture(concurrent);std::cout<<"source snapshot tests passed\n";}
