#include "ui/search_worker.h"

#include "core/search_trace.h"

#include <chrono>
#include <exception>
#include <utility>

namespace everything_lite {

SearchWorker::SearchWorker(std::string db_path) : db_path_(std::move(db_path)) {}

BackgroundSearchResult SearchWorker::search(const std::string& query,
                                             std::size_t limit,
                                             std::size_t offset,
                                             const SearchOptions& options) {
    BackgroundSearchResult outcome;
    const auto started = std::chrono::steady_clock::now();
    try {
        // Construct the shared service here, on the worker thread. This moves
        // the one-time Database::initialize()/FTS checks off the UI thread too.
        if (!service_) {
            searchTrace("SearchWorker", "initializing SearchService on background thread");
            service_ = std::make_unique<SearchService>(db_path_);
        }
        outcome.results = service_->search(query, limit, offset, options);
    } catch (const std::exception& e) {
        outcome.error = e.what();
    } catch (...) {
        outcome.error = "unknown search error";
    }
    const auto finished = std::chrono::steady_clock::now();
    outcome.elapsed_ms = std::chrono::duration<double, std::milli>(finished - started).count();
    return outcome;
}

} // namespace everything_lite
