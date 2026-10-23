#pragma once

#include "core/source_catalog.h"
#include <cstdint>
#include <string>

namespace everything_lite {

struct FileRecord {
    std::int64_t source_id = 0;
    std::string relative_path;
    CloudCopy cloud_copy = CloudCopy::Unknown;
    // Stored in the spare bit of the existing cloud_copy integer, so the
    // Finder-hidden filter adds no column to large existing indexes.
    bool finder_hidden = false;
    std::string path;
    std::string parent_path;
    std::string name;
    std::string extension;
    std::string root;
    std::uint64_t size = 0;
    std::int64_t modified_time = 0;
    bool is_directory = false;
    std::int64_t scan_generation = 0;
};

constexpr int kFinderHiddenStorageBit = 4;
inline int storedCloudFlags(const FileRecord& record) {
    return static_cast<int>(record.cloud_copy) | (record.finder_hidden ? kFinderHiddenStorageBit : 0);
}

struct SearchResult {
    FileRecord file;
    int rank = 0;
    std::string source_label;
    SourceType source_type = SourceType::Unknown;
    SourceStatus source_status = SourceStatus::Unverified;
    std::int64_t source_last_check = 0;
    std::int64_t source_last_success = 0;
};

struct IndexStats {
    bool completed = false;
    bool cancelled = false;
    std::string error;
    std::uint64_t scanned = 0;
    std::uint64_t indexed = 0;
    std::uint64_t skipped = 0;
    std::uint64_t removed = 0;
};

} // namespace everything_lite
