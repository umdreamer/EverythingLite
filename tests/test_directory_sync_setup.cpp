#include "ui/directory_sync_setup_dialog.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QUrl>
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
        QVERIFY(status(d)->text().contains("Finder：已授权"));
        QVERIFY(status(d)->text().contains("需在系统设置为当前应用开启"));
    }
    void guidanceButtonsExist() {
        DirectorySyncSetupDialog d(config("no-accessibility"));
        QVERIFY(button(d, "openAccessibilitySettings"));
        QVERIFY(button(d, "revealCurrentApplication"));
        const auto explanation=d.findChild<QLabel*>("permissionExplanation")->text();
        QVERIFY(explanation.contains("Finder 允许弹窗"));
        QVERIFY(explanation.contains("辅助功能需单独"));
    }
    void guidanceActionsOnlyOnClick() {
        const auto bundle = dir.filePath("Synthetic.app");
        const auto executable = bundle + "/Contents/MacOS/Synthetic";
        QVERIFY(QDir().mkpath(QFileInfo(executable).absolutePath()));
        QFile f(executable); QVERIFY(f.open(QIODevice::WriteOnly)); f.close();
        int settingsCalls=0, revealCalls=0;
        QUrl openedUrl; QString revealedBundle;
        DirectorySyncSetupDialog::GuidanceActions actions{
            executable,
            [&](const QUrl& url) { ++settingsCalls; openedUrl=url; return true; },
            [&](const QString& path) { ++revealCalls; revealedBundle=path; return true; }};
        DirectorySyncSetupDialog d(config("no-accessibility"), actions);
        QTRY_VERIFY(status(d)->text().contains("Finder：已授权"));
        QCOMPARE(settingsCalls, 0); QCOMPARE(revealCalls, 0);
        button(d, "checkPermissions")->click();
        QTRY_COMPARE(calls().size(), 2);
        QTRY_VERIFY(button(d, "checkPermissions")->isEnabled());
        QCOMPARE(settingsCalls, 0); QCOMPARE(revealCalls, 0);
        button(d, "openAccessibilitySettings")->click();
        QCOMPARE(settingsCalls, 1); QCOMPARE(revealCalls, 0);
        QCOMPARE(openedUrl, QUrl("x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"));
        button(d, "revealCurrentApplication")->click();
        QCOMPARE(settingsCalls, 1); QCOMPARE(revealCalls, 1);
        QCOMPARE(revealedBundle, QFileInfo(bundle).canonicalFilePath());
        QCOMPARE(calls().size(), 2); // Guidance buttons do not request system consent.
        button(d, "requestPermissions")->click();
        QTRY_COMPARE(calls().size(), 3);
        QTRY_VERIFY(button(d, "requestPermissions")->isEnabled());
        QCOMPARE(settingsCalls, 1); QCOMPARE(revealCalls, 1);
        d.reject();
        button(d, "openAccessibilitySettings")->click();
        button(d, "revealCurrentApplication")->click();
        QCOMPARE(settingsCalls, 1); QCOMPARE(revealCalls, 1);
    }
    void guidanceActionFailures() {
        const auto executable=dir.filePath("Failure.app/Contents/MacOS/Synthetic");
        QVERIFY(QDir().mkpath(QFileInfo(executable).absolutePath()));
        QFile f(executable); QVERIFY(f.open(QIODevice::WriteOnly)); f.close();
        DirectorySyncSetupDialog::GuidanceActions actions{
            executable, [](const QUrl&) { return false; }, [](const QString&) { return false; }};
        DirectorySyncSetupDialog d(config("no-accessibility"), actions);
        QTRY_VERIFY(button(d, "checkPermissions")->isEnabled());
        button(d, "openAccessibilitySettings")->click();
        QCOMPARE(status(d)->text(), QStringLiteral("无法打开辅助功能设置。请手动进入系统设置 → 隐私与安全性 → 辅助功能。"));
        button(d, "revealCurrentApplication")->click();
        QCOMPARE(status(d)->text(), QStringLiteral("无法显示当前应用。请在系统设置中手动选择当前应用。"));
        DirectorySyncSetupDialog unsupported(config("granted"), {executable, {}, {}});
        QTRY_VERIFY(button(unsupported, "checkPermissions")->isEnabled());
        button(unsupported, "revealCurrentApplication")->click();
        QCOMPARE(status(unsupported)->text(), QStringLiteral("无法定位当前应用包。请在系统设置中手动选择当前应用。"));
    }
    void invalidBundle_data() {
        QTest::addColumn<QString>("relativeExecutable");
        QTest::newRow("plain-executable") << "Plain";
        QTest::newRow("misleading-ancestor") << "Ancestor.app/Other/Plain";
        QTest::newRow("no-app-extension") << "NotBundle/Contents/MacOS/Plain";
        QTest::newRow("empty-app-name") << ".app/Contents/MacOS/Plain";
    }
    void invalidBundle() {
        QFETCH(QString, relativeExecutable);
        const auto executable=dir.filePath(relativeExecutable);
        QVERIFY(QDir().mkpath(QFileInfo(executable).absolutePath()));
        QFile f(executable); QVERIFY(f.open(QIODevice::WriteOnly)); f.close();
        int revealCalls=0;
        DirectorySyncSetupDialog::GuidanceActions actions{
            executable, [](const QUrl&) { return false; }, [&](const QString&) { ++revealCalls; return true; }};
        DirectorySyncSetupDialog d(config("granted"), actions);
        QTRY_VERIFY(button(d, "checkPermissions")->isEnabled());
        button(d, "revealCurrentApplication")->click();
        QCOMPARE(revealCalls, 0);
        QCOMPARE(status(d)->text(), QStringLiteral("无法定位当前应用包。请在系统设置中手动选择当前应用。"));
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
