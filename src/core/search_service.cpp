#include "core/search_service.h"

#include "core/search_query.h"
#include "core/search_trace.h"

#include <sstream>

namespace everything_lite {

SearchService::SearchService(std::string db_path) : db_path_(db_path), engine_(std::move(db_path)) {}

std::vector<SearchResult> SearchService::search(const std::string& query,
                                                 std::size_t limit,
                                                 std::size_t offset,
                                                 const SearchOptions& options) const {
    {
        std::ostringstream trace;
        trace << "db=\"" << db_path_ << "\""
              << " query=\"" << query << "\""
              << " utf8_hex=[" << bytesToHex(query) << "]"
              << " limit=" << limit
              << " offset=" << offset
              << " scope=" << static_cast<int>(options.scope)
              << " match_path=" << (options.match_path ? 1 : 0);
        searchTrace("SearchService", trace.str());
    }

    // The common/default case deliberately calls the exact same SearchEngine
    // string overload used by the CLI historically. This is the reference
    // behavior for GUI/CLI consistency.
    if (options.scope == SearchItemScope::All && !options.match_path) {
        auto results = engine_.search(query, limit, offset, nullptr);
        searchTrace("SearchService", "default branch result_count=" + std::to_string(results.size()));
        return results;
    }

    auto parsed = parseSearchQuery(query);

    // Explicit type:file/type:dir in the query wins over the GUI filter.
    if (!parsed.files_only && !parsed.directories_only) {
        if (options.scope == SearchItemScope::Files) parsed.files_only = true;
        if (options.scope == SearchItemScope::Folders) parsed.directories_only = true;
    }
    if (options.match_path) parsed.match_path = true;

    auto results = engine_.search(parsed, limit, offset, nullptr);
    searchTrace("SearchService", "filtered branch result_count=" + std::to_string(results.size()));
    return results;
}

} // namespace everything_lite
