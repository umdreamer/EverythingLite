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
                                             const std::atomic_bool* cancellation,
                                             std::optional<std::uint64_t> expected_revision) {
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
        // Initialization may migrate/write the index, so open the persistent
        // read-only monitor only after the service has initialized it.
        if (!revision_) revision_ = std::make_unique<SearchRevision>(db_path_);
        const auto before = revision_->read();
        if ((offset > 0 && !expected_revision) || (expected_revision && *expected_revision != before)) {
            outcome.snapshot_changed = true;
        } else {
            outcome.results = service_->search(query, limit, offset, options, cancellation);
            if (revision_->read() != before) {
                outcome.results.clear();
                outcome.snapshot_changed = true;
            } else {
                outcome.revision = before;
            }
        }
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
