#pragma once

#include "core/file_record.h"
#include "core/search_service.h"
#include "core/search_revision.h"

#include <QObject>
#include <QHash>
#include <QString>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace everything_lite {

struct BackgroundSearchResult {
    std::vector<SearchResult> results;
    QHash<QString, qint64> opened_at;
    std::string error;
    double elapsed_ms = 0.0;
    bool cancelled = false;
    bool snapshot_changed = false;
    std::optional<std::uint64_t> revision;
};

// Dedicated background search executor. It owns a SearchService that is lazily
// created on the search thread, so SQLite initialization and every query stay
// off the GUI event loop. The caller owns each cancellation token across threads.
class SearchWorker final : public QObject {
public:
    explicit SearchWorker(std::string db_path, QString open_history_path = {});

    BackgroundSearchResult search(const std::string& query,
                                  std::size_t limit,
                                  std::size_t offset,
                                  const SearchOptions& options,
                                  const std::atomic_bool* cancellation = nullptr,
                                  std::optional<std::uint64_t> expected_revision = std::nullopt);

private:
    std::string db_path_;
    QString open_history_path_;
    std::unique_ptr<SearchService> service_;
    std::unique_ptr<SearchRevision> revision_;
};

} // namespace everything_lite
