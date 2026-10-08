#pragma once

#include "core/file_record.h"
#include "core/exclusion_policy.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <filesystem>
#include <string>
#include <vector>

namespace everything_lite {

// Optional directory contract; prune roots apply before metadata/open/enumeration.
// Enter guard receives a held directory fd on POSIX; other backends reject
// configured boundaries rather than reporting unguarded success.
// Metadata guard is for deterministic failure regression only; SourceScan never
// installs one and cannot inject/override native volume identity checks.
enum class DirectoryDecision { Enter, Prune, Error };
struct ScanBoundary {
    std::vector<std::string> prune_roots;
    std::function<DirectoryDecision(const std::string&, int)> enter_directory;
    std::function<bool(const std::string&)> metadata_guard;
    // No boundary keeps legacy bound-root traversal; a configured subtree
    // follows a root link only when the caller explicitly authorizes it.
    bool allow_root_symlink = false;
};
class FileScanner {
public:
    using BatchCallback = std::function<bool(std::vector<FileRecord>&&)>;

    explicit FileScanner(std::size_t batch_size = 1000);

    IndexStats scanTree(const std::string& root,
                        const BatchCallback& callback,
                        const std::atomic_bool* cancelled = nullptr,
                        const ExclusionPolicy* policy = nullptr,
                        const ScanBoundary* boundary = nullptr) const;

    static bool readSingle(const std::string& path, FileRecord& record,
                           const ExclusionPolicy* policy = nullptr);

private:
    std::size_t batch_size_;
    static bool makeRecord(const std::filesystem::path& path, FileRecord& record);
};

} // namespace everything_lite
