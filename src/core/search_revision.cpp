#include "core/search_revision.h"

#include <sqlite3.h>

#include <memory>
#include <stdexcept>

namespace everything_lite {

SearchRevision::SearchRevision(const std::string& path) {
    const int rc = sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        const std::string error = db_ ? sqlite3_errmsg(db_) : "cannot open search revision monitor";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(error);
    }
    sqlite3_busy_timeout(db_, 5000);
}

SearchRevision::~SearchRevision() {
    if (db_) sqlite3_close(db_);
}

std::uint64_t SearchRevision::read() const {
    sqlite3_stmt* raw = nullptr;
    const int rc = sqlite3_prepare_v2(db_, "PRAGMA data_version", -1, &raw, nullptr);
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    if (rc != SQLITE_OK || sqlite3_step(statement.get()) != SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(db_));
    const auto revision = static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0));
    if (sqlite3_step(statement.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    return revision;
}

} // namespace everything_lite
