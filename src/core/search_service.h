#pragma once

#include "core/file_record.h"
#include "core/search_engine.h"

#include <cstddef>
#include <string>
#include <vector>

namespace everything_lite {

enum class SearchItemScope {
    All = 0,
    Files = 1,
    Folders = 2,
};

struct SearchOptions {
    SearchItemScope scope = SearchItemScope::All;
    bool match_path = false;
};

// The single application-level search entry point used by both CLI and GUI.
// v0.4.6 intentionally keeps all search semantics here so the UI cannot
// silently diverge from command-line results.
class SearchService {
public:
    explicit SearchService(std::string db_path);

    std::vector<SearchResult> search(const std::string& query,
                                     std::size_t limit = 500,
                                     std::size_t offset = 0,
                                     const SearchOptions& options = {}) const;

private:
    SearchEngine engine_;
};

} // namespace everything_lite
