#include "ui/main_window.h"

#include "core/search_trace.h"

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>

#include <sstream>
#include <string>

int main(int argc, char* argv[]) {
    // --debug-search is intentionally handled before QApplication so all core
    // constructors can emit trace lines from the very beginning of startup.
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i] ? argv[i] : "");
        if (arg == "--debug-search" || arg == "--trace-search") {
            qputenv("EVERYTHING_LITE_SEARCH_TRACE", QByteArrayLiteral("1"));
        }
    }

    QApplication app(argc, argv);
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
