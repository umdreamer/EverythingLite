#pragma once
#include <QDialog>
#include <QPointer>
#include <QStringList>
#include <functional>
class QUrl;
class QLabel;
class QPushButton;
class QProcess;
namespace everything_lite {
class DirectorySyncSetupDialog : public QDialog {
public:
    struct HelperConfig {
        QString program;
        QStringList arguments;
        int readTimeoutMs = 15000;
        int requestTimeoutMs = 300000;
    };
    struct GuidanceActions {
        QString applicationFilePath;
        std::function<bool(const QUrl&)> openSettings;
        std::function<bool(const QString&)> revealApplication;
    };
    explicit DirectorySyncSetupDialog(QWidget* parent = nullptr);
    // Explicit injection for isolated tests; production uses this executable only.
    explicit DirectorySyncSetupDialog(HelperConfig helper, QWidget* parent = nullptr);
    // Inject external UI actions and a synthetic executable path for isolated tests.
    DirectorySyncSetupDialog(HelperConfig helper, GuidanceActions guidance, QWidget* parent = nullptr);
    ~DirectorySyncSetupDialog() override;
    void done(int result) override;
private:
    void startProbe(bool request);
    void cancelProbe();
    HelperConfig helper_;
    GuidanceActions guidance_;
    QLabel* status_ = nullptr;
    QPushButton* check_ = nullptr;
    QPushButton* request_ = nullptr;
    QPointer<QProcess> active_;
    unsigned long generation_ = 0;
    bool closed_ = false;
};
}
