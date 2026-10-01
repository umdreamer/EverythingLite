#pragma once

#include "core/database.h"
#include "core/scanner.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace everything_lite {

class IndexManager {
public:
    using ProgressCallback = std::function<void(const std::string& root, std::uint64_t scanned)>;

    explicit IndexManager(std::string db_path);

    IndexStats rebuildRoot(const std::string& root,
                           const ProgressCallback& progress = {},
                           const std::atomic_bool* cancelled = nullptr);

    IndexStats rebuildRoots(const std::vector<std::string>& roots,
                            const ProgressCallback& progress = {},
                            const std::atomic_bool* cancelled = nullptr);

    IndexStats applyPathChange(const std::string& root, const std::string& path);

    Database& database() { return database_; }
    const Database& database() const { return database_; }

private:
    Database database_;
    FileScanner scanner_;

    static std::int64_t newGeneration();
};

} // namespace everything_lite
