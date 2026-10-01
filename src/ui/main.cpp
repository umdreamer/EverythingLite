#include "ui/main_window.h"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("CICHI"));
    QCoreApplication::setApplicationName(QStringLiteral("Everything Lite"));
    QCoreApplication::setApplicationVersion(QStringLiteral(EVERYTHING_LITE_VERSION));

    everything_lite::MainWindow window;
    window.show();
    return app.exec();
}
