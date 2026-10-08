#pragma once
#include "core/source_probe.h"
namespace everything_lite {
enum class VerificationMode { Automatic, Explicit };
// Trusted native observations, passed by value across the later helper boundary.
// This is pre/post evidence, not an atomic filesystem snapshot.
struct SourceVerificationEvidence {
    SourceRecord expected;
    ProbeResult initial_before, candidate_before, candidate_after, initial_after;
};
struct SourceVerificationResult {
    bool published = false;
    SourceStatus status = SourceStatus::Unverified;
    ProbeIssue issue = ProbeIssue::NeedsVerification;
    std::string error;
};
SourceVerificationEvidence observeSourceVerification(const SourceRecord& expected);
bool stableSourceVerification(const SourceVerificationEvidence& evidence);
bool sameSourceDirectoryInstance(const std::string& old_fingerprint, const std::string& new_fingerprint);
} // namespace everything_lite
