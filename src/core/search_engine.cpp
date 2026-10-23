#include "core/search_engine.h"

#include <algorithm>

namespace everything_lite {

namespace {

bool hasNonAscii(const std::string& text) {
    return std::any_of(text.begin(), text.end(), [](unsigned char c) { return c >= 0x80; });
}

bool queryHasNonAscii(const SearchQuery& query) {
    for (const auto& term : query.terms) {
        if (hasNonAscii(term)) return true;
    }
    if (query.path_term && hasNonAscii(*query.path_term)) return true;
    if (query.extension && hasNonAscii(*query.extension)) return true;
    return false;
}

} // namespace

SearchEngine::SearchEngine(std::string db_path) : database_(std::move(db_path)) {
    database_.initialize();
}

std::vector<SearchResult> SearchEngine::search(const std::string& query,
                                                std::size_t limit,
                                                std::size_t offset,
                                                const std::atomic_bool* cancel) const {
    return database_.search(query, limit, offset, cancel);
}

std::vector<SearchResult> SearchEngine::search(const SearchQuery& query,
                                                std::size_t limit,
                                                std::size_t offset,
                                                const std::atomic_bool* cancel) const {
    return database_.search(query, limit, offset, cancel);
}

std::vector<SearchResult> SearchEngine::searchReliable(const std::string& query,
                                                        std::size_t limit,
                                                        std::size_t offset) const {
    return database_.searchReliable(query, limit, offset);
}

std::vector<SearchResult> SearchEngine::searchReliable(const SearchQuery& query,
                                                        std::size_t limit,
                                                        std::size_t offset) const {
    return database_.searchReliable(query, limit, offset);
}


std::vector<SearchResult> SearchEngine::searchCorrect(const std::string& query,
                                                       std::size_t limit,
                                                       std::size_t offset) const {
    return searchCorrect(parseSearchQuery(query), limit, offset);
}

std::vector<SearchResult> SearchEngine::searchCorrect(const SearchQuery& query,
                                                       std::size_t limit,
                                                       std::size_t offset) const {
    // Correctness gate: a zero-result answer is trusted only after both
    // independent search paths agree. For non-ASCII text, use the canonical
    // files-table/instr path first; for ASCII text, keep FTS first for speed.
    if (queryHasNonAscii(query)) {
        auto reliable = database_.searchReliable(query, limit, offset);
        if (!reliable.empty() || (query.terms.empty() && !query.path_term)) return reliable;
        return database_.search(query, limit, offset, nullptr);
    }

    auto fast = database_.search(query, limit, offset, nullptr);
    if (!fast.empty() || (query.terms.empty() && !query.path_term)) return fast;
    return database_.searchReliable(query, limit, offset);
}

} // namespace everything_lite
