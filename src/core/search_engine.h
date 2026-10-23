#pragma once

#include "core/database.h"
#include "core/search_query.h"

#include <atomic>
#include <string>
#include <vector>

namespace everything_lite {

class SearchEngine {
public:
    explicit SearchEngine(std::string db_path);

    std::vector<SearchResult> search(const std::string& query,
                                     std::size_t limit = 500,
                                     std::size_t offset = 0,
                                     const std::atomic_bool* cancel = nullptr) const;
    std::vector<SearchResult> search(const SearchQuery& query,
                                     std::size_t limit = 500,
                                     std::size_t offset = 0,
                                     const std::atomic_bool* cancel = nullptr) const;
    std::vector<SearchResult> searchReliable(const std::string& query,
                                             std::size_t limit = 500,
                                             std::size_t offset = 0) const;
    std::vector<SearchResult> searchReliable(const SearchQuery& query,
                                             std::size_t limit = 500,
                                             std::size_t offset = 0) const;
    std::vector<SearchResult> searchCorrect(const std::string& query,
                                            std::size_t limit = 500,
                                            std::size_t offset = 0) const;
    std::vector<SearchResult> searchCorrect(const SearchQuery& query,
                                            std::size_t limit = 500,
                                            std::size_t offset = 0) const;

private:
    Database database_;
};

} // namespace everything_lite
