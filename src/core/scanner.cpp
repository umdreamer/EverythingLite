#include "core/scanner.h"
#include "core/path_utils.h"

#include <chrono>
#include <filesystem>

namespace everything_lite {
namespace fs = std::filesystem;

namespace {
std::int64_t toUnixSeconds(const fs::file_time_type& value) {
    using namespace std::chrono;
    const auto system_time = time_point_cast<system_clock::duration>(
        value - fs::file_time_type::clock::now() + system_clock::now());
    return duration_cast<seconds>(system_time.time_since_epoch()).count();
}
}

FileScanner::FileScanner(std::size_t batch_size) : batch_size_(batch_size) {}

bool FileScanner::makeRecord(const fs::path& path, FileRecord& record) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec) {
        return false;
    }

    record.path = normalizePath(pathToUtf8(path));
    record.name = pathToUtf8(path.filename());
    if (record.name.empty()) {
        record.name = record.path;
    }
    record.parent_path = normalizePath(pathToUtf8(path.parent_path()));
    auto ext = pathToUtf8(path.extension());
    if (!ext.empty() && ext.front() == '.') {
        ext.erase(ext.begin());
    }
    record.extension = ext;
    record.is_directory = fs::is_directory(status);
    record.size = 0;
    if (fs::is_regular_file(status)) {
        const auto size = fs::file_size(path, ec);
        if (ec) return false;
        record.size = static_cast<std::uint64_t>(size);
    }
    const auto modified = fs::last_write_time(path, ec);
    if (ec) {
        // A symlink may intentionally have no target; its own metadata remains
        // enumerable, while other metadata failures invalidate the snapshot.
        if (!fs::is_symlink(status)) return false;
        record.modified_time = 0;
    } else record.modified_time = toUnixSeconds(modified);
    return true;
}

bool FileScanner::readSingle(const std::string& path, FileRecord& record, const ExclusionPolicy* policy) {
    return makeRecord(utf8ToPath(path), record) && (!policy || !policy->excludes(record.path,record.is_directory));
}

IndexStats FileScanner::scanTree(const std::string& root,
                                 const BatchCallback& callback,
                                 const std::atomic_bool* cancelled,
                                 const ExclusionPolicy* policy) const {
    IndexStats stats;
    std::vector<FileRecord> batch;
    batch.reserve(batch_size_);
    auto isCancelled = [&]() { return cancelled && cancelled->load(); };
    auto flush = [&]() -> bool {
        if (isCancelled()) { stats.cancelled = true; return false; }
        if (batch.empty()) return true;
        const auto size = batch.size();
        try {
            if (!callback(std::move(batch))) { stats.cancelled = true; return false; }
        } catch (const std::exception& error) {
            stats.error = error.what(); return false;
        } catch (...) {
            stats.error = "scan callback failed"; return false;
        }
        stats.indexed += size;
        batch.clear(); batch.reserve(batch_size_);
        return true;
    };
    if (isCancelled()) { stats.cancelled = true; return stats; }
    // Ancestor exclusions apply even to event scopes inside an excluded parent.
    const auto parent=normalizePath(pathToUtf8(utf8ToPath(root).parent_path()));
    if(policy && policy->excludes(parent,true)) { stats.completed=true; return stats; }
    const auto root_path = utf8ToPath(root);
    std::error_code ec;
    const auto status = fs::status(root_path, ec);
    if (ec || !fs::exists(status)) {
        stats.error = ec ? ec.message() : "root does not exist";
        return stats;
    }
    if(policy && policy->excludes(root,fs::is_directory(status))) { stats.completed=true; return stats; }
    auto append = [&](const fs::path& path,bool is_directory) {
        if(policy && policy->excludes(pathToUtf8(path),is_directory)) { ++stats.skipped; return true; }
        ++stats.scanned;
        FileRecord record;
        if (!makeRecord(path, record)) {
            ++stats.skipped; stats.error = "unable to read file metadata";
            return false;
        }
        if(policy && policy->excludes(record.path,record.is_directory)) { ++stats.skipped; return true; }
        batch.push_back(std::move(record));
        return batch.size() < batch_size_ || flush();
    };
    if (!append(root_path,fs::is_directory(status))) return stats;
    if (fs::is_directory(status)) {
        // Never skip permission errors: an incomplete enumeration cannot be
        // used to infer deletions from a previously complete snapshot.
        fs::recursive_directory_iterator it(root_path, fs::directory_options::none, ec), end;
        if (ec) { stats.error = ec.message(); return stats; }
        while (it != end) {
            if (isCancelled()) { stats.cancelled = true; return stats; }
            const auto path=pathToUtf8(it->path());
            const auto entry_status=it->symlink_status(ec);
            if(ec) { stats.error=ec.message(); return stats; }
            const bool is_directory=fs::is_directory(entry_status);
            if(is_directory && policy && policy->excludes(path,true)) {
                it.disable_recursion_pending(); ++stats.skipped;
            } else if (!append(it->path(),is_directory)) return stats;
            it.increment(ec);
            if (ec) { stats.error = ec.message(); return stats; }
        }
    }
    if (!flush()) return stats;
    if (isCancelled()) { stats.cancelled = true; return stats; }
    stats.completed = true;
    return stats;
}

} // namespace everything_lite
