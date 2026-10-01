#pragma once

#include "core/file_record.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <filesystem>
#include <string>
#include <vector>

namespace everything_lite {

class FileScanner {
public:
    using BatchCallback = std::function<bool(std::vector<FileRecord>&&)>;

    explicit FileScanner(std::size_t batch_size = 1000);

    IndexStats scanTree(const std::string& root,
                        const BatchCallback& callback,
                        const std::atomic_bool* cancelled = nullptr) const;

    static bool readSingle(const std::string& path, FileRecord& record);

private:
    std::size_t batch_size_;
    static bool makeRecord(const std::filesystem::path& path, FileRecord& record);
};

} // namespace everything_lite
