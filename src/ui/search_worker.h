#pragma once

#include "core/file_record.h"
#include "core/search_service.h"

#include <QObject>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace everything_lite {

struct BackgroundSearchResult {
    std::vector<SearchResult> results;
    std::string error;
    double elapsed_ms = 0.0;
};

// Dedicated background search executor. It owns a SearchService that is lazily
// created on the search thread, so SQLite initialization and every query stay
// off the GUI event loop. No cancellation is used: correctness is preserved,
// and MainWindow discards stale results by request id.
class SearchWorker final : public QObject {
public:
    explicit SearchWorker(std::string db_path);

    BackgroundSearchResult search(const std::string& query,
                                  std::size_t limit,
                                  std::size_t offset,
                                  const SearchOptions& options);

private:
    std::string db_path_;
    std::unique_ptr<SearchService> service_;
};

} // namespace everything_lite
