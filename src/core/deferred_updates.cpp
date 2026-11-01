#include "core/deferred_updates.h"
#include "core/path_utils.h"
#include <sqlite3.h>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace everything_lite {
namespace {
using Statement=std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)>;
Statement prepare(sqlite3* db,const char* sql) {
    sqlite3_stmt* stmt=nullptr;
    if(sqlite3_prepare_v2(db,sql,-1,&stmt,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db));
    return Statement(stmt,sqlite3_finalize);
}
void bindValue(sqlite3* db,sqlite3_stmt* stmt,int n,const std::string& value) {
    if(sqlite3_bind_text(stmt,n,value.data(),static_cast<int>(value.size()),SQLITE_TRANSIENT)!=SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}
std::string column(sqlite3_stmt* stmt,int n) {
    const auto* value=sqlite3_column_text(stmt,n);
    return value ? std::string(reinterpret_cast<const char*>(value),sqlite3_column_bytes(stmt,n)) : std::string{};
}
}
DeferredUpdates::DeferredUpdates(const std::string& path) {
    const auto requested=utf8ToPath(path);
    const auto target=pathToUtf8(std::filesystem::canonical(requested.parent_path())/requested.filename());
#if !defined(_WIN32)
    const int fd=open(target.c_str(),O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)throw std::runtime_error("unable to open deferred update queue");
    close(fd);
#endif
    const int flags=SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX
#ifdef SQLITE_OPEN_NOFOLLOW
        |SQLITE_OPEN_NOFOLLOW
#endif
        ;
    if(sqlite3_open_v2(target.c_str(),&db_,flags,nullptr)!=SQLITE_OK) {
        const std::string message=db_?sqlite3_errmsg(db_):"unable to open deferred update queue";
        if(db_)sqlite3_close(db_);db_=nullptr;throw std::runtime_error(message);
    }
    sqlite3_busy_timeout(db_,1000);
    if(sqlite3_exec(db_,"PRAGMA journal_mode=WAL; CREATE TABLE IF NOT EXISTS pending(root TEXT NOT NULL,path TEXT NOT NULL,due_ms INTEGER NOT NULL,generation INTEGER NOT NULL,PRIMARY KEY(root,path)) WITHOUT ROWID; CREATE TABLE IF NOT EXISTS event_sequence(id INTEGER PRIMARY KEY AUTOINCREMENT);",nullptr,nullptr,nullptr)!=SQLITE_OK) {
        const std::string message=sqlite3_errmsg(db_);sqlite3_close(db_);db_=nullptr;throw std::runtime_error(message);
    }
}
DeferredUpdates::~DeferredUpdates(){if(db_)sqlite3_close(db_);}
void DeferredUpdates::enqueue(const std::string& root,const std::string& path,std::int64_t due_ms) {
    if(root.find('\0')!=std::string::npos || path.find('\0')!=std::string::npos ||
       !utf8ToPath(root).is_absolute() || !utf8ToPath(path).is_absolute() ||
       normalizePath(root)!=root || normalizePath(path)!=path || !pathIsWithin(path,root) || due_ms<0)
        throw std::invalid_argument("invalid deferred update scope");
    if(sqlite3_exec(db_,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db_));
    try {
        std::vector<std::pair<std::string,std::int64_t>> existing;
        auto list=prepare(db_,"SELECT path,due_ms FROM pending WHERE root=?");bindValue(db_,list.get(),1,root);int rc;
        while((rc=sqlite3_step(list.get()))==SQLITE_ROW)existing.emplace_back(column(list.get(),0),sqlite3_column_int64(list.get(),1));
        if(rc!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db_));
        std::string scope=path;auto deadline=due_ms;
        for(const auto& row:existing)if(pathIsWithin(scope,row.first))scope=row.first;
        if(existing.size()>=400)scope=root;
        auto remove=prepare(db_,"DELETE FROM pending WHERE root=? AND path=?");
        for(const auto& row:existing)if(pathIsWithin(row.first,scope)) {
            deadline=std::min(deadline,row.second);sqlite3_reset(remove.get());sqlite3_clear_bindings(remove.get());
            bindValue(db_,remove.get(),1,root);bindValue(db_,remove.get(),2,row.first);
            if(sqlite3_step(remove.get())!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db_));
        }
        if(sqlite3_exec(db_,"INSERT INTO event_sequence DEFAULT VALUES",nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db_));
        const auto generation=sqlite3_last_insert_rowid(db_);
        if(sqlite3_exec(db_,"DELETE FROM event_sequence",nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db_));
        auto stmt=prepare(db_,"INSERT INTO pending VALUES(?,?,?,?)");
        bindValue(db_,stmt.get(),1,root);bindValue(db_,stmt.get(),2,scope);sqlite3_bind_int64(stmt.get(),3,deadline);sqlite3_bind_int64(stmt.get(),4,generation);
        if(sqlite3_step(stmt.get())!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db_));
        if(sqlite3_exec(db_,"COMMIT",nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db_));
    } catch(...) {sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
}
std::vector<DeferredUpdate> DeferredUpdates::due(std::int64_t now_ms) const {
    auto stmt=prepare(db_,"SELECT root,path,due_ms,generation FROM pending WHERE due_ms<=? ORDER BY due_ms,root,path");
    sqlite3_bind_int64(stmt.get(),1,now_ms);std::vector<DeferredUpdate> result;int rc;
    while((rc=sqlite3_step(stmt.get()))==SQLITE_ROW)
        result.push_back({column(stmt.get(),0),column(stmt.get(),1),sqlite3_column_int64(stmt.get(),2),sqlite3_column_int64(stmt.get(),3)});
    if(rc!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db_));return result;
}
bool DeferredUpdates::acknowledge(const DeferredUpdate& value) {
    auto stmt=prepare(db_,"DELETE FROM pending WHERE root=? AND path=? AND generation=?");
    bindValue(db_,stmt.get(),1,value.root);bindValue(db_,stmt.get(),2,value.path);sqlite3_bind_int64(stmt.get(),3,value.generation);
    if(sqlite3_step(stmt.get())!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db_));return sqlite3_changes(db_)!=0;
}
void DeferredUpdates::postpone(const std::string& root,const std::string& path,std::int64_t due_ms) {
    if(sqlite3_exec(db_,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db_));
    try {
        auto list=prepare(db_,"SELECT path FROM pending WHERE root=?");bindValue(db_,list.get(),1,root);
        std::vector<std::string> matching;int rc;
        while((rc=sqlite3_step(list.get()))==SQLITE_ROW) {
            auto current=column(list.get(),0);
            if(pathIsWithin(path,current) || pathIsWithin(current,path))matching.push_back(std::move(current));
        }
        if(rc!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db_));
        auto stmt=prepare(db_,"UPDATE pending SET due_ms=MAX(due_ms,?) WHERE root=? AND path=?");
        for(const auto& current:matching) {
            sqlite3_reset(stmt.get());sqlite3_clear_bindings(stmt.get());sqlite3_bind_int64(stmt.get(),1,due_ms);
            bindValue(db_,stmt.get(),2,root);bindValue(db_,stmt.get(),3,current);
            if(sqlite3_step(stmt.get())!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db_));
        }
        if(sqlite3_exec(db_,"COMMIT",nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db_));
    } catch(...) {sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
}
}
