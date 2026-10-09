#include "core/source_verification.h"
#include "core/scan_request.h"
#include "core/path_utils.h"
#include <array>
#include <charconv>
#include <stdexcept>
namespace everything_lite {
namespace {
bool eligible(const ProbeResult& p) {
    return (p.status==SourceStatus::Accessible || p.status==SourceStatus::Unverified) &&
        (p.issue==ProbeIssue::None || p.issue==ProbeIssue::NeedsVerification || p.issue==ProbeIssue::ScopeChanged) &&
        !p.identity.empty() && !p.mount_fingerprint.empty() && !p.canonical_root.empty();
}
bool same(const ProbeResult& a,const ProbeResult& b) {
    return a.identity==b.identity && a.mount_fingerprint==b.mount_fingerprint &&
        a.canonical_root==b.canonical_root && a.child_mount_boundaries==b.child_mount_boundaries &&
        a.boundary_fingerprint==b.boundary_fingerprint && a.type==b.type;
}
std::array<std::string,4> directoryParts(const std::string& text) {
    std::array<std::string,4> parts{};std::size_t start=0;
    for(std::size_t i=0;i<parts.size();++i) {
        const auto end=text.find(':',start);
        if((i<3 && end==std::string::npos) || (i==3 && end!=std::string::npos))throw std::invalid_argument("invalid directory token");
        const auto stop=end==std::string::npos?text.size():end;parts[i]=text.substr(start,stop-start);
        const auto& value=parts[i];const bool negative=!value.empty() && value[0]=='-';
        const auto digits=negative?1u:0u;
        if(value.size()<=digits || value[digits]<'0' || value[digits]>'9' ||
           (value.size()>digits+1 && value[digits]=='0') || (negative && value=="-0"))throw std::invalid_argument("invalid directory number");
        // macOS dev_t and birth-time seconds are signed; inode and nanoseconds
        // are nonnegative. Do not conflate signed SDK time with saved timestamps.
        if(i==0 || i==2) {
            std::int64_t number=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),number);
            if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size())throw std::invalid_argument("invalid signed directory number");
        } else {
            std::uint64_t number=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),number);
            if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() ||
               (i==1 && number==0) || (i==3 && number>=1000000000))throw std::invalid_argument("invalid directory instance");
        }
        start=stop+1;
    }
    return parts;
}
void validObservation(const ProbeResult& p) {
    const auto f=capturedSourceFingerprintFields(p.mount_fingerprint);
    if(f[5]!=p.canonical_root || static_cast<int>(p.type)<0 || static_cast<int>(p.type)>static_cast<int>(SourceType::Unknown))
        throw std::invalid_argument("inconsistent source observation");
    const bool local=f[3]=="local";
    // Match SourceProbe's physical classification. Cloud/USB remain valid
    // manual catalog labels but cannot be supplied as native probe evidence.
    const bool supported_network=!local && (f[1]=="smbfs" || f[1]=="nfs" || f[1]=="afpfs" || f[1]=="webdav");
    if((local && p.type!=SourceType::Local && p.type!=SourceType::External && p.type!=SourceType::Unknown) ||
       (!local && (!supported_network || p.type!=SourceType::NAS)))
        throw std::invalid_argument("inconsistent physical source classification");
    const auto identity=local ? (f[2].empty()?std::string{}:"mac-volume:"+f[2]) : "mac-share:"+f[1]+":"+f[3];
    if(identity.empty() || identity!=p.identity) throw std::invalid_argument("invalid permanent source identity");
    directoryParts(f[6]);
    if(capturedChildMountRoots(p.boundary_fingerprint,p.canonical_root)!=p.child_mount_boundaries)
        throw std::invalid_argument("inconsistent child mount observation");
}
}
SourceVerificationEvidence observeSourceVerification(const SourceRecord& expected) {
    validateSource(expected);
    if(expected.id<=0 || expected.root.front()!='/' || expected.root.find('\0')!=std::string::npos || normalizePath(expected.root)!=expected.root)
        throw std::invalid_argument("verification requires normalized absolute source root");
    SourceVerificationEvidence e;e.expected=expected;
    auto initial=beginSourceProbe(expected);e.initial_before=initial.result();
    if(eligible(e.initial_before)) {
        auto candidate=expected;candidate.identity=e.initial_before.identity;candidate.mount_fingerprint=e.initial_before.mount_fingerprint;
        auto session=beginSourceProbe(candidate);e.candidate_before=session.result();e.candidate_after=session.finish();
    }
    e.initial_after=initial.finish();return e;
}
bool stableSourceVerification(const SourceVerificationEvidence& e) {
    try {
        if(!eligible(e.initial_before) || !eligible(e.initial_after) || !same(e.initial_before,e.initial_after) ||
           e.initial_before.status!=e.initial_after.status || e.initial_before.issue!=e.initial_after.issue ||
           e.initial_before.update_allowed!=e.initial_after.update_allowed ||
           e.candidate_before.status!=SourceStatus::Accessible || e.candidate_after.status!=SourceStatus::Accessible ||
           e.candidate_before.issue!=ProbeIssue::None || e.candidate_after.issue!=ProbeIssue::None ||
           !e.candidate_before.update_allowed || !e.candidate_after.update_allowed ||
           !same(e.initial_before,e.candidate_before) || !same(e.initial_before,e.candidate_after)) return false;
        validObservation(e.initial_before);validObservation(e.initial_after);validObservation(e.candidate_before);validObservation(e.candidate_after);
        return true;
    } catch(const std::exception&) { return false; }
}
bool sameSourceDirectoryInstance(const std::string& old_fingerprint,const std::string& new_fingerprint) {
    try {
        const auto old=capturedSourceFingerprintFields(old_fingerprint), current=capturedSourceFingerprintFields(new_fingerprint);
        // UUID does not establish directory sameness. Requiring the actual device,
        // inode and nonzero birth-time proof is deliberately conservative for NAS
        // and remounts where the device number changes. Explicit verification is
        // necessary when those observations cannot establish the same instance.
        for(const auto i:{0,1,2,3,5}) if(old[i]!=current[i])return false;
        const auto a=directoryParts(old[6]),b=directoryParts(current[6]);
        return a==b && (a[2]!="0" || a[3]!="0");
    } catch(const std::exception&) {return false;}
}
} // namespace everything_lite
