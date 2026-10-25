#include "ui/open_history_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <sqlite3.h>
#include <utility>

namespace everything_lite {
namespace {
struct DatabaseCloser {
    sqlite3* handle = nullptr;
    ~DatabaseCloser() { if (handle) sqlite3_close(handle); }
};

struct StatementCloser {
    sqlite3_stmt* handle = nullptr;
    ~StatementCloser() { if (handle) sqlite3_finalize(handle); }
};

void setError(QString* target, const QString& message) {
    if (target) *target = message;
}
} // namespace

OpenHistoryStore::OpenHistoryStore(QString database_path)
    : database_path_(QDir::cleanPath(std::move(database_path))) {}

QString OpenHistoryStore::defaultPath() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("open-history.db"));
}

bool OpenHistoryStore::record(const QString& path, qint64 requested_at, QString* error) const {
    if (path.isEmpty() || requested_at <= 0 || !QDir::isAbsolutePath(database_path_)) {
        setError(error, QStringLiteral("打开记录路径或时间无效"));
        return false;
    }
    const auto parent = QFileInfo(database_path_).absolutePath();
    if (!QDir().mkpath(parent) || QFileInfo(parent).isSymLink()
        || !QFile::setPermissions(parent, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)
        || QFileInfo(database_path_).isSymLink()) {
        setError(error, QStringLiteral("无法建立安全的打开记录目录"));
        return false;
    }
    const auto native = QFile::encodeName(database_path_);
    DatabaseCloser database;
    if (sqlite3_open_v2(native.constData(), &database.handle,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        setError(error, QStringLiteral("无法打开本应用的打开记录数据库"));
        return false;
    }
    sqlite3_busy_timeout(database.handle, 100);
    if (!QFile::setPermissions(database_path_, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        setError(error, QStringLiteral("无法限制打开记录文件权限"));
        return false;
    }
    if (sqlite3_exec(database.handle,
            "CREATE TABLE IF NOT EXISTS open_history (path TEXT PRIMARY KEY, requested_at INTEGER NOT NULL)",
            nullptr, nullptr, nullptr) != SQLITE_OK) {
        setError(error, QString::fromUtf8(sqlite3_errmsg(database.handle)));
        return false;
    }
    StatementCloser statement;
    if (sqlite3_prepare_v2(database.handle,
            "INSERT INTO open_history(path, requested_at) VALUES(?1, ?2) "
            "ON CONFLICT(path) DO UPDATE SET requested_at=excluded.requested_at",
            -1, &statement.handle, nullptr) != SQLITE_OK) {
        setError(error, QString::fromUtf8(sqlite3_errmsg(database.handle)));
        return false;
    }
    const auto bytes = path.toUtf8();
    sqlite3_bind_text(statement.handle, 1, bytes.constData(), bytes.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement.handle, 2, requested_at);
    if (sqlite3_step(statement.handle) != SQLITE_DONE) {
        setError(error, QString::fromUtf8(sqlite3_errmsg(database.handle)));
        return false;
    }
    if (error) error->clear();
    return true;
}

QHash<QString, qint64> OpenHistoryStore::lookupMany(const QStringList& paths) const {
    QHash<QString, qint64> result;
    if (paths.isEmpty()) return result;
    const QFileInfo info(database_path_);
    if (info.isSymLink() || !info.isFile()) return result;
    const auto native = QFile::encodeName(database_path_);
    DatabaseCloser database;
    if (sqlite3_open_v2(native.constData(), &database.handle, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        return result;
    sqlite3_busy_timeout(database.handle, 100);
    StatementCloser statement;
    if (sqlite3_prepare_v2(database.handle,
            "SELECT requested_at FROM open_history WHERE path=?1", -1, &statement.handle, nullptr) != SQLITE_OK)
        return result;
    for (const auto& path : paths) {
        const auto bytes = path.toUtf8();
        sqlite3_bind_text(statement.handle, 1, bytes.constData(), bytes.size(), SQLITE_TRANSIENT);
        if (sqlite3_step(statement.handle) == SQLITE_ROW)
            result.insert(path, sqlite3_column_int64(statement.handle, 0));
        sqlite3_reset(statement.handle);
        sqlite3_clear_bindings(statement.handle);
    }
    return result;
}

} // namespace everything_lite
