#include "core/index_manager.h"
#include "core/path_utils.h"

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <unordered_set>

namespace everything_lite {

IndexManager::IndexManager(std::string db_path) : database_(std::move(db_path)), scanner_(1000) {
    database_.initialize();
}

std::int64_t IndexManager::newGeneration() {
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

IndexStats IndexManager::rebuildRoot(const std::string& root,
                                     const ProgressCallback& progress,
                                     const std::atomic_bool* cancelled) {
    const auto normalized_root = normalizePath(root);
    const auto generation = newGeneration();
    database_.beginRootScan(normalized_root, generation);

    std::uint64_t reported = 0;
    auto stats = scanner_.scanTree(normalized_root, [&](std::vector<FileRecord>&& batch) {
        if (cancelled && cancelled->load()) return false;
        database_.upsertBatch(batch, normalized_root, generation);
        reported += batch.size();
        if (progress) progress(normalized_root, reported);
        return true;
    }, cancelled);

    if (!(cancelled && cancelled->load())) {
        stats.removed = database_.deleteStaleForRoot(normalized_root, generation);
        database_.completeRootScan(normalized_root, generation, stats.indexed);
        // v0.3 one-time migration: prepare the trigram basename index after a
        // complete scan. Once ready, database triggers keep it synchronized.
        if (database_.nameSearchIndexAvailable() && !database_.nameSearchIndexReady()) {
            database_.rebuildNameSearchIndex();
        }
    }
    return stats;
}

IndexStats IndexManager::rebuildRoots(const std::vector<std::string>& roots,
                                      const ProgressCallback& progress,
                                      const std::atomic_bool* cancelled) {
    IndexStats total;
    std::vector<std::string> candidates;
    candidates.reserve(roots.size());
    for (const auto& root : roots) candidates.push_back(normalizePath(root));
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.size() != b.size()) return a.size() < b.size();
        return a < b;
    });
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    std::vector<std::string> normalized;
    normalized.reserve(candidates.size());
    for (const auto& r : candidates) {
        bool covered = false;
        for (const auto& existing : normalized) {
            if (pathIsWithin(r, existing)) {
                covered = true;
                break;
            }
        }
        if (!covered) normalized.push_back(r);
    }

    for (const auto& old_root : database_.roots()) {
        if (std::find(normalized.begin(), normalized.end(), old_root) == normalized.end()) {
            total.removed += database_.deleteRoot(old_root);
        }
    }

    for (const auto& root : normalized) {
        if (cancelled && cancelled->load()) break;
        const auto stats = rebuildRoot(root, progress, cancelled);
        total.scanned += stats.scanned;
        total.indexed += stats.indexed;
        total.skipped += stats.skipped;
        total.removed += stats.removed;
    }
    return total;
}

IndexStats IndexManager::applyPathChange(const std::string& root, const std::string& path) {
    IndexStats stats;
    const auto normalized_root = normalizePath(root);
    const auto normalized_path = normalizePath(path);
    if (!pathIsWithin(normalized_path, normalized_root)) return stats;

    std::error_code ec;
    const auto fs_path = utf8ToPath(normalized_path);
    if (!std::filesystem::exists(fs_path, ec)) {
        stats.removed = database_.deletePathAndDescendants(normalized_path);
        return stats;
    }

    auto generation = database_.currentGeneration(normalized_root);
    if (generation == 0) {
        generation = newGeneration();
        database_.beginRootScan(normalized_root, generation);
    }

    if (std::filesystem::is_directory(fs_path, ec)) {
        stats.removed = database_.deletePathAndDescendants(normalized_path);
        auto scan_stats = scanner_.scanTree(normalized_path, [&](std::vector<FileRecord>&& batch) {
            database_.upsertBatch(batch, normalized_root, generation);
            return true;
        });
        stats.scanned += scan_stats.scanned;
        stats.indexed += scan_stats.indexed;
        stats.skipped += scan_stats.skipped;
    } else {
        FileRecord record;
        ++stats.scanned;
        if (FileScanner::readSingle(normalized_path, record)) {
            database_.upsertBatch({record}, normalized_root, generation);
            ++stats.indexed;
        } else {
            ++stats.skipped;
        }
    }
    return stats;
}

} // namespace everything_lite
