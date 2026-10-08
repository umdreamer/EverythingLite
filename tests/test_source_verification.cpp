#include "core/database.h"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <functional>
#include <sqlite3.h>
#include "core/scan_request.h"
#include "core/scanner.h"
#include <unistd.h>
using namespace everything_lite;
namespace fs=std::filesystem;
static bool rejected(const std::function<void()>& work) {try {work();}catch(const std::exception&){return true;}return false;}
static std::string framed(const std::array<std::string,7>& fields) {
    std::string out;for(const auto& f:fields)out+=std::to_string(f.size())+":"+f;return out;
}
static void rawCache(const Database& db,const SourceRecord& s) {
    // Deliberately bypass revision handling to exercise the transaction's own
    // empty-cache check. All values refer to the independent synthetic fixture.
    sqlite3* handle=nullptr;assert(sqlite3_open(db.path().c_str(),&handle)==SQLITE_OK);
    sqlite3_stmt* stmt=nullptr;
    const char* text="INSERT INTO files(path,parent_path,name,ext,root,search_name,search_path,source_id,relative_path) VALUES(?,?,'cached.txt','txt',?,'cached.txt',?,?,'cached.txt')";
    assert(sqlite3_prepare_v2(handle,text,-1,&stmt,nullptr)==SQLITE_OK);
    const auto path=s.root+"/cached.txt";
    for(int i:{1,4})assert(sqlite3_bind_text(stmt,i,path.c_str(),-1,SQLITE_TRANSIENT)==SQLITE_OK);
    for(int i:{2,3})assert(sqlite3_bind_text(stmt,i,s.root.c_str(),-1,SQLITE_TRANSIENT)==SQLITE_OK);
    assert(sqlite3_bind_int64(stmt,5,s.id)==SQLITE_OK);assert(sqlite3_step(stmt)==SQLITE_DONE);
    sqlite3_finalize(stmt);assert(sqlite3_close(handle)==SQLITE_OK);
}
int main() {
    char pattern[]="/private/tmp/everything-verification-XXXXXX";
    const auto temporary=mkdtemp(pattern); assert(temporary);
    const fs::path base(temporary), root=base/"root"; fs::create_directory(root);
    Database db((base/"test.db").string()); db.initialize();
    SourceRecord source;source.root=root.string();source.label="synthetic";
    source.id=db.saveSource(source);source=db.source(source.id);
    const auto candidate=beginSourceProbe(source).result();
    assert(candidate.identity.rfind("mac-volume:",0)==0 && !candidate.mount_fingerprint.empty());
    const auto original_revision=source.revision;
    const auto evidence=observeSourceVerification(source);
    const auto result=db.publishSourceVerification(evidence,VerificationMode::Automatic);
    assert(result.published && "fresh native source must bind automatically");
    source=db.source(source.id);assert(source.status==SourceStatus::Accessible && source.revision==original_revision+1 && source.last_success==0);
    assert(source.identity==candidate.identity && source.type==candidate.type);
    auto request=db.captureSourceScanRequest(source.id,"",(base/"snapshot.db").string(),"verify-test");
    auto scan=scanSourceToSnapshot(request);assert(scan.stats.completed);assert(db.mergeSourceScanSnapshot(request).completed);
    source=db.source(source.id);
    const auto previous_success=source.last_success;assert(previous_success>0);
    // Simulated saved mount token, with actual native directory evidence. This
    // exercises pure automatic refresh; no physical remount is claimed.
    auto fields=capturedSourceFingerprintFields(source.mount_fingerprint);fields[4]+="-old";source.mount_fingerprint=framed(fields);
    source.label="manual label";source.type=SourceType::Cloud;source.mode=IndexMode::Realtime;source.interval_seconds=432;
    db.saveSource(source);source=db.source(source.id);const auto old=source;
    auto old_request=db.captureSourceScanRequest(source.id,"",(base/"old.db").string(),"old-test");
    const auto refreshed=db.publishSourceVerification(observeSourceVerification(source),VerificationMode::Automatic);
    assert(refreshed.published);source=db.source(source.id);
    assert(source.label==old.label && source.type==old.type && source.mode==old.mode && source.interval_seconds==old.interval_seconds);
    assert(source.root==old.root && source.last_success==previous_success && source.revision==old.revision+1);
    assert(rejected([&]{db.mergeSourceSnapshot(old_request.source,{});}));
    auto new_request=db.captureSourceScanRequest(source.id,"",(base/"new.db").string(),"new-test");
    assert(scanSourceToSnapshot(new_request).stats.completed);
    std::ofstream(root/"cached.txt")<<"synthetic";FileRecord file;assert(FileScanner::readSingle((root/"cached.txt").string(),file));
    assert(db.mergeSourceSnapshot(source,{file}).completed);source=db.source(source.id);const auto count=db.totalFileCount();
    // Real replacement on the same volume must not be inferred from UUID.
    fs::rename(root,base/"old-root");fs::create_directory(root);
    const auto changed=observeSourceVerification(source);assert(stableSourceVerification(changed));
    assert(changed.initial_before.identity==source.identity && changed.initial_before.mount_fingerprint!=source.mount_fingerprint);
    assert(!db.publishSourceVerification(changed,VerificationMode::Automatic).published);
    source=db.source(source.id);assert(db.totalFileCount()==count && source.last_success==previous_success);
    assert(db.publishSourceVerification(observeSourceVerification(source),VerificationMode::Explicit).published);
    source=db.source(source.id);assert(db.totalFileCount()==count);
    // Missing and permission failures preserve cached rows and last success.
    fs::rename(root,base/"missing-root");
    const auto offline=db.publishSourceVerification(observeSourceVerification(source),VerificationMode::Explicit);
    assert(!offline.published && offline.status==SourceStatus::Offline && offline.issue==ProbeIssue::Missing);
    source=db.source(source.id);assert(source.last_success==previous_success && db.totalFileCount()==count);
    fs::rename(base/"missing-root",root);
    if(geteuid()!=0) {
        fs::permissions(root,fs::perms::none);
        const auto failed=db.publishSourceVerification(observeSourceVerification(source),VerificationMode::Explicit);
        fs::permissions(root,fs::perms::owner_all);
        assert(!failed.published && failed.status==SourceStatus::Failed && failed.issue==ProbeIssue::CheckFailed);
        source=db.source(source.id);assert(source.last_success==previous_success && db.totalFileCount()==count);
    }
    assert(db.publishSourceVerification(observeSourceVerification(source),VerificationMode::Explicit).published);source=db.source(source.id);
    // Expired success AND failure evidence cannot overwrite concurrent label,
    // cache updates or a forgotten source. Policy changes are independent.
    auto stale=observeSourceVerification(source);source.label="changed concurrently";db.saveSource(source);source=db.source(source.id);
    assert(rejected([&]{db.publishSourceVerification(stale,VerificationMode::Explicit);}));
    assert(db.source(source.id).label==source.label);
    auto failure=stale;failure.initial_before.status=SourceStatus::Offline;failure.initial_before.issue=ProbeIssue::Missing;
    assert(rejected([&]{db.publishSourceVerification(failure,VerificationMode::Explicit);}));
    auto policy_evidence=observeSourceVerification(source);ExclusionRules rules;rules.file_filter="*.ignored";db.saveExclusionPolicy(rules);
    assert(db.publishSourceVerification(policy_evidence,VerificationMode::Explicit).published);
    assert(db.captureSourceScanRequest(source.id,"",(base/"policy.db").string(),"policy-test").policy.revision==db.exclusionPolicy().revision);
    source=db.source(source.id);auto cache_stale=observeSourceVerification(source);
    file.size=444;db.upsertBatch({file},source.root,0);
    assert(rejected([&]{db.publishSourceVerification(cache_stale,VerificationMode::Explicit);}));
    source=db.source(source.id);auto forgotten=observeSourceVerification(source);db.forgetSource(source.id);
    assert(rejected([&]{db.publishSourceVerification(forgotten,VerificationMode::Explicit);}));
    // Raw cache insertion after observation does not advance revision. The TX
    // must recheck this source's own cache before automatic legacy binding.
    SourceRecord legacy;legacy.root=root.string();legacy.type=SourceType::USB;legacy.label="legacy manual";
    legacy.id=db.saveSource(legacy);legacy=db.source(legacy.id);auto legacy_evidence=observeSourceVerification(legacy);rawCache(db,legacy);
    assert(!db.publishSourceVerification(legacy_evidence,VerificationMode::Automatic).published);
    legacy=db.source(legacy.id);assert(legacy.identity.empty() && db.totalFileCount()==1);
    assert(db.publishSourceVerification(observeSourceVerification(legacy),VerificationMode::Explicit).published);
    legacy=db.source(legacy.id);assert(legacy.type==SourceType::USB && legacy.label=="legacy manual" && db.totalFileCount()==1);
    // Known identity replacement is rejected even with an empty cache.
    db.forgetSource(legacy.id);SourceRecord foreign;foreign.root=root.string();foreign.identity="mac-volume:synthetic-other";
    foreign.mount_fingerprint=candidate.mount_fingerprint;foreign.id=db.saveSource(foreign);foreign=db.source(foreign.id);
    auto foreign_result=db.publishSourceVerification(observeSourceVerification(foreign),VerificationMode::Explicit);
    assert(!foreign_result.published && foreign_result.issue==ProbeIssue::IdentityChanged);
    assert(db.source(foreign.id).identity==foreign.identity && db.totalFileCount()==0);
    // Pure simulated evidence cannot authorize a missing candidate, malformed
    // fingerprints, canonical changes or an unstable before/after boundary.
    auto valid=observeSourceVerification(db.source(foreign.id));
    valid.expected.identity.clear();valid.expected.mount_fingerprint.clear();valid=observeSourceVerification(valid.expected);
    assert(stableSourceVerification(valid));
    for(int choice=0;choice<6;++choice) {
        auto bad=valid;
        if(choice==0)bad.candidate_after.update_allowed=false;
        if(choice==1)bad.initial_after.identity+="different";
        if(choice==2)bad.initial_after.canonical_root+="different";
        if(choice==3)bad.initial_after.boundary_fingerprint="bad";
        if(choice==4)bad.initial_after.mount_fingerprint+="bad";
        if(choice==5)bad.initial_after.issue=ProbeIssue::Unsupported;
        assert(!stableSourceVerification(bad));
    }
    fields=capturedSourceFingerprintFields(valid.initial_before.mount_fingerprint);auto altered=fields;altered[6]="1:1:0:0";
    assert(!sameSourceDirectoryInstance(framed(altered),framed(altered)));
    altered=fields;altered[6]="999999:"+fields[6].substr(fields[6].find(':')+1);
    assert(!sameSourceDirectoryInstance(framed(altered),framed(fields)));
    assert(!sameSourceDirectoryInstance(framed(fields)+"extra",framed(fields)));
    auto malformed=valid.expected;malformed.root="relative";assert(rejected([&]{observeSourceVerification(malformed);}));
    malformed.root=root.string()+std::string(1,'\0')+"bad";assert(rejected([&]{observeSourceVerification(malformed);}));
    // SDK-compatible signed device/birth evidence is simulated through the
    // production evaluator, not an assertion about an actual remount/NAS.
    MountEvidence mount;mount.mount_point=fields[0];mount.filesystem=fields[1];mount.volume_uuid=fields[2];
    mount.local=true;mount.internal=true;mount.mount_token=fields[4];
    RootEvidence signed_root;signed_root.canonical_root=fields[5];signed_root.directory_token="-2:99:-1:0";signed_root.actual_mount=mount;
    SourceVerificationEvidence signed_evidence;signed_evidence.expected=valid.expected;
    signed_evidence.initial_before=evaluateSourceEvidence(signed_evidence.expected,signed_root,{mount});
    auto signed_candidate=signed_evidence.expected;signed_candidate.identity=signed_evidence.initial_before.identity;
    signed_candidate.mount_fingerprint=signed_evidence.initial_before.mount_fingerprint;
    signed_evidence.candidate_before=evaluateSourceEvidence(signed_candidate,signed_root,{mount});
    signed_evidence.candidate_after=signed_evidence.candidate_before;signed_evidence.initial_after=signed_evidence.initial_before;
    assert(stableSourceVerification(signed_evidence) && "signed SDK directory evidence must permit explicit verification");
    assert(sameSourceDirectoryInstance(signed_candidate.mount_fingerprint,signed_candidate.mount_fingerprint));
    // Initial session state changes cannot be normalized into candidate success.
    auto state_changed=valid;state_changed.initial_before.status=SourceStatus::Accessible;
    state_changed.initial_before.issue=ProbeIssue::None;state_changed.initial_before.update_allowed=true;
    state_changed.initial_after.issue=ProbeIssue::ScopeChanged;
    assert(!stableSourceVerification(state_changed));
    // Real native observation sees local storage in a configured network slot.
    SourceRecord network;network.root=root.string();network.type=SourceType::NAS;network.identity="mac-share:smbfs:synthetic/share";
    network.id=db.saveSource(network);network=db.source(network.id);
    auto network_result=db.publishSourceVerification(observeSourceVerification(network),VerificationMode::Explicit);
    assert(!network_result.published && network_result.status==SourceStatus::Offline && network_result.issue==ProbeIssue::ExpectedNetworkMount);
    assert(db.source(network.id).identity==network.identity);
    // Real replacement during held sessions: no test production hook required.
    auto unstable_expected=valid.expected;unstable_expected=db.source(foreign.id);unstable_expected.identity.clear();unstable_expected.mount_fingerprint.clear();
    SourceVerificationEvidence unstable;unstable.expected=unstable_expected;
    auto initial_session=beginSourceProbe(unstable_expected);unstable.initial_before=initial_session.result();
    auto unstable_candidate=unstable_expected;unstable_candidate.identity=unstable.initial_before.identity;unstable_candidate.mount_fingerprint=unstable.initial_before.mount_fingerprint;
    auto held_candidate=beginSourceProbe(unstable_candidate);unstable.candidate_before=held_candidate.result();
    fs::rename(root,base/"changed-during-probe");fs::create_directory(root);
    unstable.candidate_after=held_candidate.finish();unstable.initial_after=initial_session.finish();
    assert(!stableSourceVerification(unstable));
    assert(rejected([&]{db.publishSourceVerification(valid,static_cast<VerificationMode>(9));}));
    fs::remove_all(base);std::cout<<"source verification tests passed\n";
}
