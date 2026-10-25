#pragma once

#include <QDate>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

namespace everything_lite {

struct IndexUpdateEvent {
    QString kind;
    QString root;
    QString path;
    QString reason;
    int attempt = 0;
    QDateTime at;
};

class IndexUpdateJournal final {
public:
    explicit IndexUpdateJournal(QString directory);

    static QString defaultDirectory();
    const QString& directory() const { return directory_; }
    bool append(const IndexUpdateEvent& event, QString* error = nullptr) const;
    QStringList dates() const;
    QVector<IndexUpdateEvent> read(const QDate& date) const;
    bool containsPath(const QString& path) const;
    bool removeDate(const QDate& date, QString* error = nullptr) const;
    int prune(int retention_days, const QDate& today, QString* error = nullptr) const;

private:
    QString directory_;
    QString filePath(const QDate& date) const;
};

} // namespace everything_lite
