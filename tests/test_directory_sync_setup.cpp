#include "ui/directory_sync_setup_dialog.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest>
using namespace everything_lite;
class DirectorySyncSetupTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    QString script;
    QString log;
    DirectorySyncSetupDialog::HelperConfig config(const QString& mode, int timeout = 2000) {
        return {QStringLiteral("/usr/bin/python3"), {script, mode, log}, timeout, timeout};
    }
    QLabel* status(DirectorySyncSetupDialog& d) { return d.findChild<QLabel*>("permissionStatus"); }
    QPushButton* button(DirectorySyncSetupDialog& d, const char* name) { return d.findChild<QPushButton*>(name); }
    QList<QByteArray> calls() {
        QFile f(log); if (!f.open(QIODevice::ReadOnly)) return {};
        auto lines=f.readAll().trimmed().split('\n'); return lines;
    }
private slots:
    void initTestCase() {
        QVERIFY(dir.isValid()); script=dir.filePath("helper.py"); log=dir.filePath("calls.jsonl");
        QFile f(script); QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"PY(import json, signal, sys, time
mode, log = sys.argv[1:3]
with open(log, 'a') as f: f.write(json.dumps(sys.argv[3:])+'\n')
request = '--request' in sys.argv[3:]
if mode in ('slow', 'late'):
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    time.sleep(0.5 if mode == 'slow' else 0.12)
if mode == 'exit':
    print('private diagnostic', file=sys.stderr)
    sys.exit(7)
if mode == 'oversized':
    print('x'*70000)
    sys.exit(0)
v = dict(protocol=1, accessibility=True, finder_status=mode if mode in ('granted','consent-required','denied','unavailable','error') else 'granted', finder_error=0, request=request)
if mode == 'malformed': print('not JSON'); sys.exit(0)
if mode == 'wrong-version': v['protocol'] = 2
if mode == 'no-accessibility': v['accessibility'] = False
if mode == 'wrong-type': v['accessibility'] = 1
if mode == 'wrong-enum': v['finder_status'] = 'other'
if mode == 'wrong-request': v['request'] = not request
if mode == 'wrong-integer': v['finder_error'] = 0.5
if mode == 'missing': del v['finder_error']
if mode == 'extra': v['directory'] = 'private directory'
print(json.dumps(v))
)PY");
    }
    void init() { QFile::remove(log); }
    void status_data() {
        QTest::addColumn<QString>("mode"); QTest::addColumn<QString>("message");
        QTest::newRow("granted") << "granted" << "Finder：已授权";
        QTest::newRow("consent-required") << "consent-required" << "Finder：需要系统授权";
        QTest::newRow("denied") << "denied" << "Finder：未获授权";
        QTest::newRow("unavailable") << "unavailable" << "Finder：暂不可用";
        QTest::newRow("error") << "error" << "Finder：无法检查";
    }
    void status() {
        QFETCH(QString, mode); QFETCH(QString, message);
        DirectorySyncSetupDialog d(config(mode));
        QCOMPARE(d.windowTitle(), QStringLiteral("Everything Lite — 目录联动权限"));
        QVERIFY(status(d));
        QTRY_VERIFY(status(d)->text().contains(message));
        QVERIFY(status(d)->text().contains("辅助功能：已授权"));
        QVERIFY(d.findChild<QLabel*>("permissionExplanation")->text().contains("不等于"));
    }
    void noAccessibility() {
        DirectorySyncSetupDialog d(config("no-accessibility")); QVERIFY(status(d));
        QTRY_VERIFY(status(d)->text().contains("辅助功能：未获授权"));
    }
    void argumentsAndRepeatedClick() {
        DirectorySyncSetupDialog d(config("granted")); QVERIFY(status(d));
        QTRY_VERIFY(status(d)->text().contains("Finder：已授权"));
        QCOMPARE(calls().size(), 1);
        QCOMPARE(QJsonDocument::fromJson(calls()[0]).array(), QJsonArray{"--directory-sync-permission-probe"});
        auto* request=button(d,"requestPermissions"); QVERIFY(request); request->click(); request->click();
        QTRY_COMPARE(calls().size(), 2);
        QTRY_VERIFY(request->isEnabled());
        QCOMPARE(QJsonDocument::fromJson(calls()[1]).array(), (QJsonArray{"--directory-sync-permission-probe","--request"}));
    }
    void failure_data() {
        QTest::addColumn<QString>("mode");
        for(const auto* mode : {"malformed","wrong-version","wrong-type","wrong-enum","wrong-request","wrong-integer","missing","extra","exit","oversized"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void failure() {
        QFETCH(QString, mode); DirectorySyncSetupDialog d(config(mode)); QVERIFY(status(d));
        QTRY_COMPARE(status(d)->text(),QStringLiteral("权限检查失败。请重试。"));
        QVERIFY(button(d,"checkPermissions")->isEnabled());
    }
    void timeoutKeepsChildExclusive() {
        DirectorySyncSetupDialog d(config("slow",60)); QVERIFY(status(d));
        auto* check=button(d,"checkPermissions"); QVERIFY(check);
        QTRY_COMPARE(calls().size(),1);
        QTRY_COMPARE(status(d)->text(),QStringLiteral("权限检查超时。请重试。"));
        QVERIFY(!check->isEnabled()); check->click(); QCOMPARE(calls().size(),1);
        QTRY_VERIFY(check->isEnabled());
    }
    void closeAndLateResult() {
        auto* d=new DirectorySyncSetupDialog(config("late"));
        QTRY_COMPARE(calls().size(),1);
        QPointer<DirectorySyncSetupDialog> guard(d);
        QElapsedTimer elapsed; elapsed.start(); d->close(); delete d;
        QVERIFY(elapsed.elapsed()<100); QVERIFY(guard.isNull());
        // Process the late completion without accessing a destroyed dialog.
        QTest::qWait(400);
    }
    void failedStart() {
        auto h=config("granted");h.program=dir.filePath("missing-program");
        DirectorySyncSetupDialog d(h); QVERIFY(status(d));
        QTRY_COMPARE(status(d)->text(),QStringLiteral("权限检查失败。请重试。"));
        QVERIFY(button(d,"checkPermissions")->isEnabled());
    }
};
QTEST_MAIN(DirectorySyncSetupTest)
#include "test_directory_sync_setup.moc"
