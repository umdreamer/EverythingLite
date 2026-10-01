#pragma once

#include <cstdint>
#include <string>

namespace everything_lite {

struct FileRecord {
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

struct SearchResult {
    FileRecord file;
    int rank = 0;
};

struct IndexStats {
    std::uint64_t scanned = 0;
    std::uint64_t indexed = 0;
    std::uint64_t skipped = 0;
    std::uint64_t removed = 0;
};

} // namespace everything_lite
