#pragma once
#include "core/source_catalog.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace everything_lite {
// Input evidence: raw_remote_source is transient; never copy it to output or logs.
// mount_token is a current statfs observation, NOT a permanent or unique mount ID.
struct MountEvidence {
    std::string mount_point, filesystem, volume_uuid, raw_remote_source, mount_token;
    bool local = false;
    std::optional<bool> internal;
};
struct RootEvidence {
    std::string canonical_root, directory_token;
    MountEvidence actual_mount; // obtained from this directory's fd, not path selection
    // Native backend adds aliases only after equal device/inode/birth-time evidence.
    std::vector<std::string> equivalent_roots;
};
enum class ProbeIssue { None, Missing, CheckFailed, Unsupported, NeedsVerification, IdentityChanged, ScopeChanged, ExpectedNetworkMount };
struct ProbeResult {
    SourceStatus status = SourceStatus::Unverified;
    SourceType type = SourceType::Unknown;
    ProbeIssue issue = ProbeIssue::NeedsVerification;
    // Network identity is a sanitized logical share, never a server physical UUID.
    // Volume UUID is persistent SDK evidence, not a promise against cloned UUIDs.
    std::string identity, mount_fingerprint, canonical_root;
    std::vector<std::string> child_mount_boundaries;
    std::string boundary_fingerprint; // transient begin/end comparison, never permanent ID
    bool update_allowed = false;
};
// Empty means unsupported/ambiguous syntax. Credentials are discarded BEFORE
// identifiers/fingerprints are constructed. No mount options are accepted.
std::string sanitizeRemoteSource(const std::string& raw);
std::optional<std::size_t> selectMountForPath(const std::string& path, const std::vector<MountEvidence>& mounts);
ProbeResult evaluateSourceEvidence(const SourceRecord& saved, const RootEvidence& root, const std::vector<MountEvidence>& mounts);
class ProbeSession {
public:
    ProbeSession();
    ~ProbeSession();
    ProbeSession(ProbeSession&&) noexcept;
    ProbeSession& operator=(ProbeSession&&) noexcept;
    ProbeSession(const ProbeSession&) = delete;
    ProbeSession& operator=(const ProbeSession&) = delete;
    const ProbeResult& result() const;
    // Single-use: verifies held fd AND the current root path/mount, then closes fd.
    // This is a pre/post guard, not an atomic filesystem snapshot. A Scanner must
    // verify actual mount membership when entering directories and prune nested
    // configured sources too. Lexical boundaries alone miss firmlink aliases.
    // Root-open/metadata evidence does not establish directory enumeration
    // permission. Scanner must reject enumeration/read failures before success.
    // Avoid metadata probes for every ordinary file; directory entry guards are
    // the later Scanner integration contract. No persistent GUI-held descriptor.
    ProbeResult finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend ProbeSession beginSourceProbe(const SourceRecord& source);
};
ProbeSession beginSourceProbe(const SourceRecord& source);
} // namespace everything_lite
