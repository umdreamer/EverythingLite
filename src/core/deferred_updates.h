#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;
namespace everything_lite {
struct DeferredUpdate {
    std::string root;
    std::string path;
    std::int64_t due_ms=0;
    std::int64_t generation=0;
};

// Separate from the searchable index. Acknowledging an older generation must
// never erase a later event, including one received by another window.
class DeferredUpdates {
public:
    explicit DeferredUpdates(const std::string& path);
    ~DeferredUpdates();
    DeferredUpdates(const DeferredUpdates&)=delete;
    DeferredUpdates& operator=(const DeferredUpdates&)=delete;
    void enqueue(const std::string& root,const std::string& path,std::int64_t due_ms);
    std::vector<DeferredUpdate> due(std::int64_t now_ms) const;
    bool acknowledge(const DeferredUpdate& update);
    void postpone(const std::string& root,const std::string& path,std::int64_t due_ms);
private:
    sqlite3* db_=nullptr;
};
}
