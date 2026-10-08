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
                                             const SearchOptions& options,
                                             const std::atomic_bool* cancellation) {
    BackgroundSearchResult outcome;
    const auto started = std::chrono::steady_clock::now();
    try {
        if (cancellation && cancellation->load(std::memory_order_relaxed)) {
            outcome.cancelled = true;
            outcome.error = "search cancelled";
            return outcome;
        }
        // Construct the shared service here, on the worker thread. This moves
        // the one-time Database::initialize()/FTS checks off the UI thread too.
        if (!service_) {
            searchTrace("SearchWorker", "initializing SearchService on background thread");
            service_ = std::make_unique<SearchService>(db_path_);
        }
        outcome.results = service_->search(query, limit, offset, options, cancellation);
    } catch (const std::exception& e) {
        outcome.error = e.what();
    } catch (...) {
        outcome.error = "unknown search error";
    }
    outcome.cancelled = cancellation && cancellation->load(std::memory_order_relaxed);
    const auto finished = std::chrono::steady_clock::now();
    outcome.elapsed_ms = std::chrono::duration<double, std::milli>(finished - started).count();
    return outcome;
}

} // namespace everything_lite
