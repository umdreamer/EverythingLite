#include "core/search_engine.h"

namespace everything_lite {

SearchEngine::SearchEngine(std::string db_path) : database_(std::move(db_path)) {
    database_.initialize();
}

std::vector<SearchResult> SearchEngine::search(const std::string& query,
                                                std::size_t limit,
                                                std::size_t offset) const {
    return database_.search(query, limit, offset);
}

std::vector<SearchResult> SearchEngine::search(const SearchQuery& query,
                                                std::size_t limit,
                                                std::size_t offset) const {
    return database_.search(query, limit, offset);
}

} // namespace everything_lite
