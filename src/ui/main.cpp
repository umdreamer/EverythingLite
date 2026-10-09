#include "ui/main_window.h"
#include "ui/directory_sync_permissions.h"
#include "ui/directory_sync_setup_dialog.h"

#include "core/search_trace.h"

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>

#include <sstream>
#include <string>

int main(int argc, char* argv[]) {
    // Reserved helper modes dispatch before Qt, tracing, settings, or index startup.
    bool setup = false;
    bool probe = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i] ? argv[i] : "");
        setup = setup || arg == "--directory-sync-setup";
        probe = probe || arg == "--directory-sync-permission-probe";
    }
    if (probe) {
        if (setup || std::string(argv[1]) != "--directory-sync-permission-probe" ||
            (argc != 2 && !(argc == 3 && std::string(argv[2]) == "--request"))) return 2;
        return everything_lite::runDirectorySyncPermissionHelper(argc == 3);
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
