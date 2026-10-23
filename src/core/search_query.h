#pragma once

#include "core/source_catalog.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace everything_lite {

struct SearchQuery {
    std::optional<std::int64_t> source_id;
    std::optional<SourceStatus> source_status;
    std::vector<std::string> terms;
    std::optional<std::string> extension;
    std::optional<std::string> path_term;
    // Absolute folder whose indexed descendants are searched. The folder
    // itself is outside this scope.
    std::optional<std::string> folder_scope;
    std::optional<std::uint64_t> min_size;
    std::optional<std::uint64_t> max_size;
    std::optional<std::int64_t> modified_after;
    bool files_only = false;
    bool directories_only = false;
    // Mirrors Everything's "Match Path" option. When false, plain terms match
    // only the basename. When true, plain terms match the full path.
    bool match_path = false;
    bool include_hidden = true;
};

SearchQuery parseSearchQuery(const std::string& query, std::int64_t now_seconds = 0);

} // namespace everything_lite
