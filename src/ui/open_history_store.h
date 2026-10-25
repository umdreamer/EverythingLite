#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

namespace everything_lite {

// Stores only opening requests accepted by this application. The database is
// independent of the rebuildable search index and is never queried by a view.
class OpenHistoryStore final {
public:
    explicit OpenHistoryStore(QString database_path);

    static QString defaultPath();
    const QString& path() const { return database_path_; }
    bool record(const QString& path, qint64 requested_at, QString* error = nullptr) const;
    QHash<QString, qint64> lookupMany(const QStringList& paths) const;

private:
    QString database_path_;
};

} // namespace everything_lite
