#include "ui/main_window.h"
#include "ui/directory_sync_permissions.h"
#ifdef __APPLE__
#include "ui/directory_sync_native_probe.h"
#endif
#include "ui/directory_sync_setup_dialog.h"

#include "core/search_trace.h"

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QTimer>

#include <sstream>
#include <string>

int main(int argc, char* argv[]) {
    // Validate reserved helpers before tracing, settings, or index startup.
    // The permission helper remains entirely before Qt initialization.
    bool setup = false;
    bool probe = false;
    bool nativeProbe = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i] ? argv[i] : "");
        nativeProbe = nativeProbe || arg == "--directory-sync-native-probe";
        setup = setup || arg == "--directory-sync-setup";
        probe = probe || arg == "--directory-sync-permission-probe";
    }
    if (probe) {
        if (setup || nativeProbe || std::string(argv[1]) != "--directory-sync-permission-probe" ||
            (argc != 2 && !(argc == 3 && std::string(argv[2]) == "--request"))) return 2;
        return everything_lite::runDirectorySyncPermissionHelper(argc == 3);
    }
    if (nativeProbe) {
        if (setup || std::string(argv[1]) != "--directory-sync-native-probe" ||
            (argc != 2 && !(argc == 6 && std::string(argv[2]) == "--fixture" &&
                           std::string(argv[4]) == "--directory"))) return 2;
#ifdef __APPLE__
        const bool fixtureMode = argc == 6;
        if (fixtureMode && (std::string(argv[3]).rfind("--", 0) == 0 ||
                            std::string(argv[5]).rfind("--", 0) == 0)) return 2;
        const std::string fixturePid = fixtureMode ? argv[3] : "";
        const std::string fixtureDirectory = fixtureMode ? argv[5] : "";
        QApplication app(argc, argv);
        QTimer::singleShot(0, &app, [&app, fixtureMode, fixturePid, fixtureDirectory] {
            app.exit(everything_lite::runDirectorySyncNativeProbe(fixtureMode, fixturePid, fixtureDirectory));
        });
        return app.exec();
#else
        return 2;
#endif
    }
    if (setup) {
        if (argc != 2 || std::string(argv[1]) != "--directory-sync-setup") return 2;
        QApplication app(argc, argv);
        everything_lite::DirectorySyncSetupDialog window;
        window.show();
        return app.exec();
    }

    // --debug-search is intentionally handled before QApplication so all core
    // constructors can emit trace lines from the very beginning of startup.
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i] ? argv[i] : "");
        if (arg == "--debug-search" || arg == "--trace-search") {
            qputenv("EVERYTHING_LITE_SEARCH_TRACE", QByteArrayLiteral("1"));
        }
    }

    QApplication app(argc, argv);
#ifdef Q_OS_LINUX
    QGuiApplication::setDesktopFileName(QStringLiteral("org.everythinglite.EverythingLite"));
#endif
    QCoreApplication::setOrganizationName(QStringLiteral("CICHI"));
    QCoreApplication::setApplicationName(QStringLiteral("Everything Lite"));
    QCoreApplication::setApplicationVersion(QStringLiteral(EVERYTHING_LITE_VERSION));

    if (everything_lite::searchTraceEnabled()) {
        std::ostringstream t;
        t << "START version=" << EVERYTHING_LITE_VERSION
          << " pid=" << QCoreApplication::applicationPid()
          << " executable=\"" << QCoreApplication::applicationFilePath().toStdString() << "\""
          << " cwd=\"" << QDir::currentPath().toStdString() << "\"";
        everything_lite::searchTrace("GUI", t.str());
    }

    everything_lite::MainWindow window;
    window.show();
    return app.exec();
}
