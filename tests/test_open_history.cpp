#include "ui/open_history_store.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

using namespace everything_lite;

class OpenHistoryTest : public QObject {
    Q_OBJECT
private slots:
    void acceptedRequestPersistsAcrossInstances() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto database = temp.filePath(QStringLiteral("history.db"));
        const auto path = QStringLiteral("/synthetic/中文 空格.txt");
        {
            OpenHistoryStore store(database);
            QCOMPARE(store.lookupMany({path}).size(), 0);
            QVERIFY(!QFile::exists(database));
            QString error;
            QVERIFY2(store.record(path, 1700000000, &error), qPrintable(error));
        }
        OpenHistoryStore reopened(database);
        const auto first = reopened.lookupMany({path, QStringLiteral("/synthetic/other.txt")});
        QCOMPARE(first.size(), 1);
        QCOMPARE(first.value(path), qint64(1700000000));
        QVERIFY(reopened.record(path, 1700000100));
        QCOMPARE(reopened.lookupMany({path}).value(path), qint64(1700000100));
#ifdef Q_OS_UNIX
        const auto perms = QFileInfo(database).permissions();
        QVERIFY(!(perms & (QFileDevice::ReadGroup | QFileDevice::ReadOther |
                           QFileDevice::WriteGroup | QFileDevice::WriteOther)));
#endif
    }

    void batchLookupReturnsOnlyKnownPaths() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        OpenHistoryStore store(temp.filePath(QStringLiteral("history.db")));
        QStringList paths;
        for (int i = 0; i < 1000; ++i) paths << QStringLiteral("/synthetic/%1.txt").arg(i);
        QVERIFY(store.record(paths.at(0), 1));
        QVERIFY(store.record(paths.at(999), 2));
        const auto found = store.lookupMany(paths);
        QCOMPARE(found.size(), 2);
        QCOMPARE(found.value(paths.at(0)), qint64(1));
        QCOMPARE(found.value(paths.at(999)), qint64(2));
    }
};

QTEST_GUILESS_MAIN(OpenHistoryTest)
#include "test_open_history.moc"
