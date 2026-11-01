#include "core/search_service.h"

#include "core/search_query.h"
#include "core/path_utils.h"
#include "core/search_trace.h"
#include "core/database.h"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace everything_lite {

SearchService::SearchService(std::string db_path) : db_path_(db_path), engine_(std::move(db_path)) {}

std::vector<SearchResult> SearchService::search(const std::string& query,
                                                 std::size_t limit,
                                                 std::size_t offset,
                                                 const SearchOptions& options,
                                                 const std::atomic_bool* cancellation) const {
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
    if (options.scope == SearchItemScope::All && !options.match_path && !options.folder_scope &&
        options.include_hidden && options.source_roots.empty()) {
        auto results = engine_.search(query, limit, offset, cancellation);
        searchTrace("SearchService", "default branch result_count=" + std::to_string(results.size()));
        return results;
    }

    auto parsed = parseSearchQuery(query);
    for(const auto& root:options.source_roots)parsed.source_roots.push_back(normalizePath(root));
    if(!parsed.source_roots.empty()) {
        const auto configured=parsed.source_roots;
        for(const auto& source:Database(db_path_).sources()) {
            if(source.status==SourceStatus::Offline ||
               std::find(configured.begin(),configured.end(),source.root)!=configured.end())continue;
            if(!std::any_of(configured.begin(),configured.end(),[&](const auto& root){return pathIsWithin(source.root,root);}))continue;
            std::error_code ec;
            const bool present=std::filesystem::exists(utf8ToPath(source.root),ec);
            if(present || (ec && ec!=std::errc::no_such_file_or_directory && ec!=std::errc::not_a_directory))
                parsed.source_roots.push_back(source.root);
        }
    }
    if (options.folder_scope) {
        if(options.folder_scope->empty() || !utf8ToPath(*options.folder_scope).is_absolute())
            throw std::invalid_argument("folder scope must be an absolute directory path");
        parsed.folder_scope=normalizePath(*options.folder_scope);
    }

    // Explicit type:file/type:dir in the query wins over the GUI filter.
    if (!parsed.files_only && !parsed.directories_only) {
        if (options.scope == SearchItemScope::Files) parsed.files_only = true;
        if (options.scope == SearchItemScope::Folders) parsed.directories_only = true;
    }
    if (options.match_path) parsed.match_path = true;
    parsed.include_hidden = options.include_hidden;

    auto results = engine_.search(parsed, limit, offset, cancellation);
    searchTrace("SearchService", "filtered branch result_count=" + std::to_string(results.size()));
    return results;
}

} // namespace everything_lite
