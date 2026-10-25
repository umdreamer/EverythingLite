#include "ui/index_update_journal.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace everything_lite;

class IndexUpdateJournalTest : public QObject {
    Q_OBJECT
private slots:
    void writesSeparateDaysAndReadsEvents() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto directory = QDir(temp.path()).filePath(QStringLiteral("logs"));
        IndexUpdateJournal journal(directory);
        IndexUpdateEvent first{QStringLiteral("开始"), QStringLiteral("/sample"),
                               QStringLiteral("/sample/a"), {}, 0,
                               QDateTime::fromString(QStringLiteral("2026-10-24T23:59:00+08:00"), Qt::ISODate)};
        auto second = first;
        second.kind = QStringLiteral("待更新");
        second.reason = QStringLiteral("扫描未完整完成");
        second.at = QDateTime::fromString(QStringLiteral("2026-10-25T00:01:00+08:00"), Qt::ISODate);
        QString error;
        QVERIFY2(journal.append(first, &error), qPrintable(error));
        QVERIFY2(journal.append(second, &error), qPrintable(error));
        QCOMPARE(journal.dates(), (QStringList{QStringLiteral("2026-10-25"), QStringLiteral("2026-10-24")}));
        const auto before = journal.read(QDate(2026, 10, 24));
        const auto after = journal.read(QDate(2026, 10, 25));
        QCOMPARE(before.size(), 1);
        QCOMPARE(after.size(), 1);
        QCOMPARE(before.front().path, first.path);
        QCOMPARE(after.front().reason, second.reason);
        QVERIFY(journal.containsPath(QDir(directory).filePath(QStringLiteral("index-update-2026-10-25.jsonl"))));
        QVERIFY(!journal.containsPath(QDir(temp.path()).filePath(QStringLiteral("other.jsonl"))));
    }

    void privatePermissionsAndNoSearchField() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        IndexUpdateJournal journal(QDir(temp.path()).filePath(QStringLiteral("logs")));
        const IndexUpdateEvent event{QStringLiteral("恢复"), QStringLiteral("/sample"),
                                     QStringLiteral("/sample/a"), {}, 1,
                                     QDateTime::fromString(QStringLiteral("2026-10-25T10:00:00+08:00"), Qt::ISODate)};
        QString error;
        QVERIFY2(journal.append(event, &error), qPrintable(error));
        const auto file_path = QDir(journal.directory()).filePath(QStringLiteral("index-update-2026-10-25.jsonl"));
        QFile file(file_path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto data = file.readAll();
        const auto json = QJsonDocument::fromJson(data.trimmed()).object();
        QCOMPARE(json.value(QStringLiteral("kind")).toString(), event.kind);
        QVERIFY(!json.contains(QStringLiteral("query")));
        QVERIFY(!json.contains(QStringLiteral("search")));
#ifdef Q_OS_UNIX
        const auto file_permissions = QFileInfo(file_path).permissions();
        const auto directory_permissions = QFileInfo(journal.directory()).permissions();
        QVERIFY(!(file_permissions & (QFileDevice::ReadGroup | QFileDevice::ReadOther |
                                       QFileDevice::WriteGroup | QFileDevice::WriteOther)));
        QVERIFY(!(directory_permissions & (QFileDevice::ReadGroup | QFileDevice::ReadOther |
                                            QFileDevice::WriteGroup | QFileDevice::WriteOther)));
#endif
    }
};

QTEST_GUILESS_MAIN(IndexUpdateJournalTest)
#include "test_index_update_journal.moc"
