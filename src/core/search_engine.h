#pragma once

#include "core/database.h"

#include <string>
#include <vector>

namespace everything_lite {

class SearchEngine {
public:
    explicit SearchEngine(std::string db_path);
    std::vector<SearchResult> search(const std::string& query, std::size_t limit = 500) const;

private:
    Database database_;
};

} // namespace everything_lite
