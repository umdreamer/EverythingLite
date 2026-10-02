#include "ui/desktop_actions.h"
#include "ui/file_preview.h"
#include <QApplication>
#include <QDesktopServices>
#include <QFile>
#include <QDir>
#include <QImage>
#include <QPlainTextEdit>
#include <QLabel>
#include <QSignalSpy>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QTimer>
class MockDesktop : public QDBusVirtualObject {
    Q_OBJECT
  public:
    QString signature;
    QString interface;
    QString member;
    QList<QVariant> args;
    int calls = 0;
    int delay = 0;
    bool fail = false;
    QString introspect(const QString&) const override {
        return {};
    }
    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override {
        ++calls;
        signature = message.signature();
        interface = message.interface();
        member = message.member();
        args = message.arguments();
        auto reply =
            fail ? message.createErrorReply("org.test.Failure", "synthetic failure") : message.createReply();
        QTimer::singleShot(delay, this, [connection, reply] { connection.send(reply); });
        return true;
    }
};
#endif
using namespace everything_lite;
class UrlSink : public QObject {
    Q_OBJECT
  public:
    QList<QUrl> urls;
  public slots:
    void accept(const QUrl& url) {
        urls << url;
    }
};
class DesktopTest : public QObject {
    Q_OBJECT
  private slots:
    void previewLimits() {
        QTemporaryDir dir;
        auto write = [&](const QString& name, const QByteArray& data) {
            QFile f(dir.filePath(name));
            if (!f.open(QIODevice::WriteOnly))
                return QString{};
            f.write(data);
            return f.fileName();
        };
        QCOMPARE(loadFilePreview(write("text.txt", QStringLiteral("中文\nHello").toUtf8())).kind,
                 PreviewKind::Text);
        QVERIFY(loadFilePreview(write("empty.txt", {})).message.contains(QStringLiteral("空")));
        QCOMPARE(loadFilePreview(write("binary.bin", QByteArray("\0\xff", 2))).kind, PreviewKind::Metadata);
        QCOMPARE(loadFilePreview(write("invalid.txt", QByteArray("\xc3\x28", 2))).kind,
                 PreviewKind::Metadata);
        QCOMPARE(loadFilePreview(write("truncated.txt", QByteArray::fromHex("e7a0"))).kind,
                 PreviewKind::Metadata);
        QVERIFY(loadFilePreview(write("large.txt", QByteArray(1024 * 1024 + 1, 'a')))
                    .message.contains(QStringLiteral("过大")));
        QVERIFY(loadFilePreview(dir.filePath("missing")).message.contains(QStringLiteral("不存在")));
        QCOMPARE(loadFilePreview(dir.path()).kind, PreviewKind::Metadata);
        QImage image(16, 16, QImage::Format_RGB32);
        image.fill(Qt::green);
        QVERIFY(image.save(dir.filePath("small.png")));
        QCOMPARE(loadFilePreview(dir.filePath("small.png")).kind, PreviewKind::Image);
        // A PNG header claiming huge dimensions must be rejected before decoding.
        QFile f(dir.filePath("small.png"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        auto png = f.readAll();
        f.close();
        png.replace(16, 8, QByteArray::fromHex("0001000000010000"));
        quint32 crc = 0xffffffffu;
        for (const auto byte : png.mid(12, 17)) {
            crc ^= static_cast<unsigned char>(byte);
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
        }
        crc ^= 0xffffffffu;
        for (int byte = 0; byte < 4; ++byte)
            png[29 + byte] = char(crc >> (24 - byte * 8));
        QCOMPARE(loadFilePreview(write("huge.png", png)).kind, PreviewKind::Metadata);
        auto unreadable = write("unreadable.txt", "sample");
        QFile::setPermissions(unreadable, {});
        QVERIFY(loadFilePreview(unreadable).message.contains(QStringLiteral("不可读")));
    }
    void previewDialogAndOpenFailure() {
        QTemporaryDir dir;
        const auto path = dir.filePath("<b>report.txt");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QStringLiteral("中文 sample").toUtf8());
        file.close();
        auto* preview = new FilePreview(path);
        preview->show();
        QTRY_VERIFY(preview->findChild<QPlainTextEdit*>());
        auto* metadata = preview->findChild<QLabel*>();
        QVERIFY(metadata);
        QCOMPARE(metadata->textFormat(), Qt::PlainText);
        QVERIFY(metadata->text().contains(QStringLiteral("<b>report.txt")));
        QCOMPARE(preview->findChild<QPlainTextEdit*>()->toPlainText(), QStringLiteral("中文 sample"));
        delete preview;
        auto* closedDuringLoad = new FilePreview(path);
        delete closedDuringLoad;
        QTest::qWait(100);
        DesktopActions actions;
        QSignalSpy feedback(&actions, &DesktopActions::feedback);
        actions.open(QString{});
        QCOMPARE(feedback.size(), 1);
        QVERIFY(feedback[0][0].toString().contains(QStringLiteral("未接受")));
    }
    void executableUsesLiteralArgument() {
        QTemporaryDir dir;
        const auto output = dir.filePath("argv");
        const auto path = dir.filePath(QStringLiteral("中文 空格#?%\"\n$() ` ;.txt"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        qputenv("EL_TEST_RECORD_ARGV", output.toUtf8());
        DesktopActions actions;
        QVERIFY(actions.openWithExecutable(path, QCoreApplication::applicationFilePath()));
        qunsetenv("EL_TEST_RECORD_ARGV");
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(output), 3000);
        QFile result(output);
        QVERIFY(result.open(QIODevice::ReadOnly));
        QCOMPARE(result.readAll(), path.toUtf8());
        QSignalSpy feedback(&actions, &DesktopActions::feedback);
        QVERIFY(!actions.openWithExecutable(path, path));
        QCOMPARE(feedback.size(), 1);
    }
#ifdef Q_OS_MACOS
    void nativeLauncherFailureIsVisible() {
        QTemporaryDir dir;
        const auto path = dir.filePath("synthetic.txt");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        DesktopActions actions;
        QSignalSpy feedback(&actions, &DesktopActions::feedback);
        actions.openWithApplication(path, QStringLiteral("EverythingLiteSyntheticMissingApplication"));
        QTRY_COMPARE_WITH_TIMEOUT(feedback.size(), 1, 3000);
    }
#endif
#ifdef Q_OS_LINUX
    void dbusContractAndFallback() {
        auto bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "mock-desktop");
        QVERIFY(bus.isConnected());
        MockDesktop service;
        QVERIFY(bus.registerService("org.freedesktop.FileManager1"));
        QVERIFY(bus.registerVirtualObject("/org/freedesktop/FileManager1", &service));
        QVERIFY(bus.registerService("org.gnome.NautilusPreviewer"));
        QVERIFY(bus.registerVirtualObject("/org/gnome/NautilusPreviewer", &service));
        UrlSink sink;
        QDesktopServices::setUrlHandler("file", &sink, "accept");
        DesktopActions actions;
        QSignalSpy feedback(&actions, &DesktopActions::feedback);
        QSignalSpy fallback(&actions, &DesktopActions::previewFallback);
        const QString path = QStringLiteral("/tmp/中文 空格#?%\"\n$() ` ;.txt");
        const auto uri = QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
        actions.reveal(path);
        QTRY_COMPARE(service.calls, 1);
        QCOMPARE(service.signature, QStringLiteral("ass"));
        QCOMPARE(service.interface, QStringLiteral("org.freedesktop.FileManager1"));
        QCOMPARE(service.member, QStringLiteral("ShowItems"));
        QCOMPARE(qdbus_cast<QStringList>(service.args[0]), QStringList{uri});
        QCOMPARE(service.args[1].toString(), QString{});
        actions.preview(path);
        QTRY_COMPARE(service.calls, 2);
        QCOMPARE(service.signature, QStringLiteral("ssb"));
        QCOMPARE(service.interface, QStringLiteral("org.gnome.NautilusPreviewer2"));
        QCOMPARE(service.member, QStringLiteral("ShowFile"));
        QCOMPARE(service.args[0].toString(), uri);
        QCOMPARE(service.args[1].toString(), QString{});
        QCOMPARE(service.args[2].toBool(), false);
        service.fail = true;
        actions.reveal(path);
        QTRY_COMPARE(sink.urls.size(), 1);
        QCOMPARE(sink.urls[0], QUrl::fromLocalFile("/tmp"));
        QVERIFY(!feedback.isEmpty());
        actions.preview(path);
        QTRY_COMPARE(fallback.size(), 1);
        QCOMPARE(fallback[0][0].toString(), path);
        service.fail = false;
        service.delay = 4000;
        actions.preview(path);
        QTRY_COMPARE_WITH_TIMEOUT(fallback.size(), 2, 3500);
        QTest::qWait(2600);
        QCOMPARE(fallback.size(), 2);
        // Superseding a delayed failure prevents its fallback from affecting the new selection.
        service.fail = true;
        service.delay = 100;
        int handled = service.calls;
        actions.preview(path);
        QTRY_COMPARE(service.calls, handled + 1);
        service.fail = false;
        service.delay = 0;
        actions.preview(path + "new");
        QTest::qWait(200);
        QCOMPARE(fallback.size(), 2);
        service.fail = true;
        service.delay = 100;
        handled = service.calls;
        actions.reveal(path);
        QTRY_COMPARE(service.calls, handled + 1);
        service.fail = false;
        service.delay = 0;
        actions.reveal(path + "new");
        QTest::qWait(200);
        QCOMPARE(sink.urls.size(), 1);
        service.fail = true;
        service.delay = 100;
        auto* destroyed = new DesktopActions;
        handled = service.calls;
        destroyed->preview(path);
        QTRY_COMPARE(service.calls, handled + 1);
        delete destroyed;
        QTest::qWait(200);
        QCOMPARE(fallback.size(), 2);
        bus.unregisterService("org.gnome.NautilusPreviewer");
        actions.preview(path);
        QTRY_COMPARE(fallback.size(), 3);
        QDesktopServices::unsetUrlHandler("file");
        bus.unregisterObject("/org/freedesktop/FileManager1");
        bus.unregisterObject("/org/gnome/NautilusPreviewer");
        bus.unregisterService("org.freedesktop.FileManager1");
        bus.unregisterService("org.gnome.NautilusPreviewer");
    }
#endif
};
int main(int argc, char** argv) {
    if (argc == 2 && qEnvironmentVariableIsSet("EL_TEST_RECORD_ARGV")) {
        QSaveFile out(qEnvironmentVariable("EL_TEST_RECORD_ARGV"));
        if (!out.open(QIODevice::WriteOnly))
            return 2;
        out.write(argv[1]);
        return out.commit() ? 0 : 2;
    }
    QTemporaryDir sandbox;
    if (!sandbox.isValid())
        return 2;
    qputenv("HOME", sandbox.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", (sandbox.path() + "/config").toUtf8());
    qputenv("XDG_DATA_HOME", (sandbox.path() + "/data").toUtf8());
    qputenv("XDG_CACHE_HOME", (sandbox.path() + "/cache").toUtf8());
    const auto runtime = sandbox.path() + "/runtime";
    QDir().mkpath(runtime);
    QFile::setPermissions(runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    qputenv("XDG_RUNTIME_DIR", runtime.toUtf8());
    QApplication app(argc, argv);
    DesktopTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_desktop_actions.moc"
