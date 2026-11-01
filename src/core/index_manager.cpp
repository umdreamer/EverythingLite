#include "core/index_manager.h"
#include "core/update_policy.h"
#include <unordered_set>
#include "core/path_utils.h"
#include "core/source_probe.h"

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include <stdexcept>
#include <iterator>
#include <optional>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace everything_lite {

namespace {
bool sameVolumeLegacyChild(const std::string& parent,const SourceRecord& child) {
#if !defined(_WIN32)
    struct stat parent_stat{},child_stat{};
    return (child.type==SourceType::Local || child.type==SourceType::Unknown) &&
        child.identity.empty() && child.mount_fingerprint.empty() &&
        lstat(parent.c_str(),&parent_stat)==0 && S_ISDIR(parent_stat.st_mode) &&
        lstat(child.root.c_str(),&child_stat)==0 && S_ISDIR(child_stat.st_mode) &&
        parent_stat.st_dev==child_stat.st_dev;
#else
    (void)parent;(void)child;return false;
#endif
}
}

IndexManager::IndexManager(std::string db_path) : database_(std::move(db_path)), scanner_(1000) {
    database_.initialize();
}

std::int64_t IndexManager::newGeneration() {
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

IndexStats IndexManager::rebuildSource(std::int64_t source_id,
                                      const ProgressCallback& progress,
                                      const std::atomic_bool* cancelled,const UpdatePolicy* update_policy) {
    const auto expected = database_.source(source_id);
    return rebuildSourceScope(expected, expected.root, {}, progress, cancelled,{},update_policy);
}

IndexStats IndexManager::rebuildSourceScope(const SourceRecord& expected, const std::string& scan_path,
                                           const std::string& subtree_relative,
                                           const ProgressCallback& progress,
                                           const std::atomic_bool* cancelled,
                                           const std::string& reconcile_target,const UpdatePolicy* update_policy) {
    std::optional<SourceOwnershipSnapshot> ownership;
    try { ownership.emplace(database_.captureSourceDescendants(expected)); }
    catch(const std::exception& error) { IndexStats stats; stats.error=error.what(); return stats; }
    const auto& policy=ownership->policy;
    const auto& descendants=ownership->descendants;
    // Classification changes only the displayed physical type. It never binds
    // a legacy cache to a new volume identity or grants snapshot authority.
    std::optional<ProbeSession> type_probe;
    if(expected.type==SourceType::Unknown && subtree_relative.empty() && (!update_policy || update_policy->includesPeriodic()))
        type_probe.emplace(beginSourceProbe(expected));
    ScanBoundary boundary; boundary.allow_root_symlink=scan_path==expected.root;
    boundary.allow_partial=true;
    boundary.required_path=reconcile_target;
    boundary.visibility_root=expected.root;
    bool cadence_deferred=false;
    std::unordered_set<std::string> observed_hidden;
    if(update_policy) {
        const auto markDeferred=[&](const auto& path,bool hidden) {
            if(hidden && !update_policy->includesPeriodic()) {
                auto ancestor=path;bool covered=false;
                for(;;) {
                    if(observed_hidden.count(ancestor)){covered=true;break;}
                    if(ancestor==expected.root)break;
                    const auto slash=ancestor.find_last_of('/');if(slash==std::string::npos)break;
                    ancestor=slash==0?"/":ancestor.substr(0,slash);
                    if(ancestor.size()<expected.root.size())break;
                }
                if(!covered)observed_hidden.insert(path);
            }
            if(!update_policy->allows(path,hidden) && update_policy->classify(path,hidden)==UpdateCadence::Periodic)cadence_deferred=true;
        };
        boundary.allow_record=[&,update_policy,markDeferred](const auto& path,bool hidden){markDeferred(path,hidden);return update_policy->allows(path,hidden);};
        boundary.descend_directory=[&,update_policy,markDeferred](const auto& path,bool hidden){markDeferred(path,hidden);return update_policy->shouldDescend(path,hidden);};
    }
    for(const auto& child:descendants) {
        if(pathIsWithin(scan_path,child.root)) {
            IndexStats stats; stats.error="scan scope belongs to nested source"; return stats;
        }
        boundary.prune_roots.push_back(child.root);
    }
    std::vector<FileRecord> snapshot;
    std::uint64_t reported = 0;
    auto stats = scanner_.scanTree(scan_path, [&](std::vector<FileRecord>&& batch) {
        if (cancelled && cancelled->load()) return false;
        reported += batch.size();
        for(auto& record:batch)
            if(reconcile_target.empty() || pathIsWithin(record.path,reconcile_target))snapshot.push_back(std::move(record));
        if (progress) progress(expected.root, reported);
        return !(cancelled && cancelled->load());
    }, cancelled, &policy, &boundary);
    stats.deferred=cadence_deferred;
    if(!reconcile_target.empty())for(const auto& protected_path:stats.protected_paths)
        if(pathIsWithin(reconcile_target,protected_path)) {
            stats.completed=false;stats.partial_complete=false;break;
        }
    const bool snapshot_ready=(stats.completed || stats.partial_complete) && !(cancelled && cancelled->load());
    if (snapshot_ready) {
        try {
            const std::vector<std::string> observed(observed_hidden.begin(),observed_hidden.end());
            const auto merged = database_.mergeSourceSnapshot(expected, snapshot, subtree_relative,
                &descendants, stats.partial_complete ? &stats.protected_paths : nullptr,update_policy,&observed);
            stats.deferred=stats.deferred || merged.deferred;
            stats.removed = merged.removed;
            stats.changed = merged.changed;
            stats.indexed = merged.indexed;
            if(type_probe && stats.completed) {
                const auto before=type_probe->result(), after=type_probe->finish();
                if((before.type==SourceType::Local || before.type==SourceType::NAS || before.type==SourceType::External) &&
                   before.type==after.type && !before.mount_fingerprint.empty() &&
                   before.mount_fingerprint==after.mount_fingerprint &&
                   before.canonical_root==after.canonical_root &&
                   before.status!=SourceStatus::Failed && before.status!=SourceStatus::Offline &&
                   after.status!=SourceStatus::Failed && after.status!=SourceStatus::Offline) {
                    try {
                        auto source=database_.source(expected.id);
                        if(source.type==SourceType::Unknown && source.root==expected.root) {
                            source.type=before.type;
                            database_.saveSource(source);
                            stats.changed=true;
                        }
                    } catch(const std::exception&) {
                        // A concurrent catalog edit wins; the completed index
                        // snapshot remains valid and the next rebuild can retry.
                    }
                }
            }
        } catch (const std::exception& error) {
            stats.completed = false; stats.partial_complete = false;
            stats.error = error.what(); stats.indexed = 0;
        }
    }
    if (!snapshot_ready) {
        stats.completed = false;
        stats.indexed = 0;
        if (cancelled && cancelled->load()) stats.cancelled = true;
        if (!stats.cancelled) {
            auto updated = expected;
            std::error_code ec;
            const auto status = std::filesystem::status(utf8ToPath(expected.root), ec);
            updated.status = (!std::filesystem::exists(status) && (!ec || ec == std::errc::no_such_file_or_directory))
                ? SourceStatus::Offline : SourceStatus::Failed;
            using namespace std::chrono;
            updated.last_check = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
            // A concurrent catalog update owns the new state; never overwrite it.
            try {
                database_.saveSource(updated);
                stats.changed = updated.status != expected.status;
            } catch (const std::exception&) {}
        }
    }
    return stats;
}

IndexStats IndexManager::rebuildRoot(const std::string& root,
                                     const ProgressCallback& progress,
                                     const std::atomic_bool* cancelled,const UpdatePolicy* update_policy) {
    const auto id = database_.ensureSourceForRoot(normalizePath(root));
    return rebuildSource(id, progress, cancelled,update_policy);
}

IndexStats IndexManager::rebuildRoots(const std::vector<std::string>& roots,
                                      const ProgressCallback& progress,
                                      const std::atomic_bool* cancelled,const UpdatePolicy* update_policy) {
    IndexStats total; total.completed = true;
    std::unordered_set<std::string> seen;
    std::vector<std::pair<std::string,std::int64_t>> configured;
    for (const auto& root : roots) {
        if (cancelled && cancelled->load()) { total.completed = false; total.cancelled = true; break; }
        const auto normalized = normalizePath(root);
        if (!seen.insert(normalized).second) continue;
        configured.emplace_back(normalized,database_.ensureSourceForRoot(normalized));
    }
    const auto explicitly_configured=configured;
    const auto sources=database_.sources();
    for(const auto& source:sources) {
        if(seen.count(source.root))continue;
        if(std::count_if(sources.begin(),sources.end(),[&](const auto& other){return other.root==source.root;})!=1)continue;
        for(const auto& parent:explicitly_configured) {
            if(pathIsWithin(source.root,parent.first) && sameVolumeLegacyChild(parent.first,source)) {
                seen.insert(source.root);configured.emplace_back(source.root,source.id);break;
            }
        }
    }
    // Register the complete ownership topology before any traversal. Children
    // establish their snapshots first, so one rebuild can retire old overlaps.
    std::stable_sort(configured.begin(),configured.end(),[](const auto& left,const auto& right) {
        return left.first.size()>right.first.size();
    });
    for(const auto& entry:configured) {
        if (cancelled && cancelled->load()) { total.completed = false; total.cancelled = true; break; }
        const auto already_scanned=total.scanned;
        const auto stats = rebuildSource(entry.second, progress ? ProgressCallback(
            [&](const std::string& root,std::uint64_t count){progress(root,already_scanned+count);}) : ProgressCallback{}, cancelled,update_policy);
        total.scanned += stats.scanned; total.indexed += stats.indexed;
        total.skipped += stats.skipped; total.removed += stats.removed;
        total.changed = total.changed || stats.changed;
        total.deferred = total.deferred || stats.deferred;
        total.completed = total.completed && stats.completed;
        total.cancelled = total.cancelled || stats.cancelled;
        if (!stats.error.empty()) {
            if(!total.error.empty())total.error+="\n";
            total.error+=stats.error;
        }
    }
    return total;
}

IndexStats IndexManager::applyPathChange(const std::string& root, const std::string& path,const UpdatePolicy* update_policy) {
    const auto normalized_root = normalizePath(root);
    auto scan_path = normalizePath(path);
    const auto requested_path=scan_path;
    if (!pathIsWithin(scan_path, normalized_root)) return {};
    SourceRecord expected;
    for (const auto& source : database_.sources()) {
        if (source.root == normalized_root) {
            if (expected.id) throw std::runtime_error("ambiguous source binding; use source id");
            expected = source;
        }
    }
    if (!expected.id) return rebuildRoot(normalized_root,{},nullptr,update_policy);
    // Only same-volume legacy local descendants can be routed without
    // replacing the separately bound identity of a removable/network source.
    try {
        const auto ownership=database_.captureSourceDescendants(expected);
        std::optional<SourceRecord> owner;
        for(const auto& child:ownership.descendants)if(pathIsWithin(scan_path,child.root)) {
            if(owner && owner->root==child.root) {
                IndexStats stats; stats.error="ambiguous nested source binding"; return stats;
            }
            if(!owner || child.root.size()>owner->root.size())owner=child;
        }
        if(owner) {
            if(sameVolumeLegacyChild(normalized_root,*owner))
                return applyPathChange(owner->root,scan_path,update_policy);
            IndexStats stats; stats.error="scan scope belongs to nested source requiring verification"; return stats;
        }
    } catch(const std::exception& error) { IndexStats stats; stats.error=error.what(); return stats; }
    std::error_code ec;
    const bool exists = std::filesystem::exists(utf8ToPath(scan_path), ec);
    if (ec) { IndexStats stats; stats.error = ec.message(); return stats; }
    if (!exists) {
        // A missing child alone cannot prove deletion: enumerate its nearest
        // existing parent completely before applying a bounded diff.
        while (scan_path != normalized_root) {
            scan_path = normalizePath(pathToUtf8(utf8ToPath(scan_path).parent_path()));
            const bool parent_exists = std::filesystem::exists(utf8ToPath(scan_path), ec);
            if (ec) { IndexStats stats; stats.error = ec.message(); return stats; }
            if (parent_exists) break;
        }
    }
    const auto target=(!exists && requested_path!=normalized_root)?requested_path:scan_path;
    const auto relative = target == normalized_root ? std::string{} :
        target.substr(normalized_root.size() + (normalized_root.back() == '/' ? 0 : 1));
    return rebuildSourceScope(expected, scan_path, relative, {}, nullptr,
                              target==scan_path?std::string{}:target,update_policy);
}

} // namespace everything_lite
