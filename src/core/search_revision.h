#pragma once

#include <cstdint>
#include <string>

struct sqlite3;

namespace everything_lite {

// data_version is comparable only on the same connection. Keep this monitor
// for the lifetime of one worker; use it only on that worker's thread.
// It never creates/initializes the database or holds a read transaction.
class SearchRevision final {
public:
    explicit SearchRevision(const std::string& path);
    ~SearchRevision();
    SearchRevision(const SearchRevision&) = delete;
    SearchRevision& operator=(const SearchRevision&) = delete;

    std::uint64_t read() const;

private:
    sqlite3* db_ = nullptr;
};

} // namespace everything_lite
