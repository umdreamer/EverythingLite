#include "core/search_service.h"

#include "core/search_query.h"

namespace everything_lite {

SearchService::SearchService(std::string db_path) : engine_(std::move(db_path)) {}

std::vector<SearchResult> SearchService::search(const std::string& query,
                                                 std::size_t limit,
                                                 std::size_t offset,
                                                 const SearchOptions& options) const {
    // The common/default case deliberately calls the exact same SearchEngine
    // string overload used by the CLI historically. This is the reference
    // behavior for GUI/CLI consistency.
    if (options.scope == SearchItemScope::All && !options.match_path) {
        return engine_.search(query, limit, offset, nullptr);
    }

    auto parsed = parseSearchQuery(query);

    // Explicit type:file/type:dir in the query wins over the GUI filter.
    if (!parsed.files_only && !parsed.directories_only) {
        if (options.scope == SearchItemScope::Files) parsed.files_only = true;
        if (options.scope == SearchItemScope::Folders) parsed.directories_only = true;
    }
    if (options.match_path) parsed.match_path = true;

    return engine_.search(parsed, limit, offset, nullptr);
}

} // namespace everything_lite
