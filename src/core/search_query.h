#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace everything_lite {

struct SearchQuery {
    std::vector<std::string> terms;
    std::optional<std::string> extension;
    std::optional<std::string> path_term;
    std::optional<std::uint64_t> min_size;
    std::optional<std::uint64_t> max_size;
    std::optional<std::int64_t> modified_after;
    bool files_only = false;
    bool directories_only = false;
};

SearchQuery parseSearchQuery(const std::string& query, std::int64_t now_seconds = 0);

} // namespace everything_lite
