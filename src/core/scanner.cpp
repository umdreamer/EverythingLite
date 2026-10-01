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
        if (!ec) {
            record.size = static_cast<std::uint64_t>(size);
        }
        ec.clear();
    }
    const auto modified = fs::last_write_time(path, ec);
    if (!ec) {
        record.modified_time = toUnixSeconds(modified);
    }
    return true;
}

bool FileScanner::readSingle(const std::string& path, FileRecord& record) {
    return makeRecord(utf8ToPath(path), record);
}

IndexStats FileScanner::scanTree(const std::string& root,
                                 const BatchCallback& callback,
                                 const std::atomic_bool* cancelled) const {
    IndexStats stats;
    std::vector<FileRecord> batch;
    batch.reserve(batch_size_);

    auto flush = [&]() -> bool {
        if (batch.empty()) {
            return true;
        }
        auto size = static_cast<std::uint64_t>(batch.size());
        if (!callback(std::move(batch))) {
            return false;
        }
        stats.indexed += size;
        batch.clear();
        batch.reserve(batch_size_);
        return true;
    };

    const auto root_path = utf8ToPath(root);
    std::error_code ec;
    if (!fs::exists(root_path, ec)) {
        return stats;
    }

    FileRecord root_record;
    ++stats.scanned;
    if (makeRecord(root_path, root_record)) {
        batch.push_back(std::move(root_record));
    } else {
        ++stats.skipped;
    }

    const auto options = fs::directory_options::skip_permission_denied;
    fs::recursive_directory_iterator it(root_path, options, ec), end;
    while (it != end) {
        if (cancelled && cancelled->load()) {
            break;
        }
        if (ec) {
            ++stats.skipped;
            ec.clear();
            it.increment(ec);
            continue;
        }

        ++stats.scanned;
        FileRecord record;
        if (makeRecord(it->path(), record)) {
            batch.push_back(std::move(record));
            if (batch.size() >= batch_size_ && !flush()) {
                break;
            }
        } else {
            ++stats.skipped;
        }
        it.increment(ec);
    }

    flush();
    return stats;
}

} // namespace everything_lite
