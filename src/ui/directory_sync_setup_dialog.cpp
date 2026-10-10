#include "ui/directory_sync_setup_dialog.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
namespace everything_lite {
namespace {
constexpr qint64 outputBudget = 64 * 1024;
DirectorySyncSetupDialog::GuidanceActions productionGuidance() {
    DirectorySyncSetupDialog::GuidanceActions actions;
    actions.applicationFilePath = QCoreApplication::applicationFilePath();
#ifdef Q_OS_MAC
    actions.openSettings = [](const QUrl& url) { return QDesktopServices::openUrl(url); };
    actions.revealApplication = [](const QString& bundle) {
        return QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), bundle});
    };
#endif
    return actions;
}
QString applicationBundle(const QString& executablePath) {
    const QFileInfo executable(executablePath);
    if (!executable.isFile()) return {};
    const auto canonicalPath = executable.canonicalFilePath();
    if (canonicalPath.isEmpty()) return {};
    auto directory = QFileInfo(canonicalPath).dir();
    if (directory.dirName() != QStringLiteral("MacOS") || !directory.cdUp() ||
        directory.dirName() != QStringLiteral("Contents") || !directory.cdUp()) return {};
    const auto name = directory.dirName();
    if (name.size() <= 4 || !name.endsWith(QStringLiteral(".app"))) return {};
    return directory.absolutePath();
}
struct ProbeOutput {
    QByteArray stdoutData;
    qint64 bytes = 0;
    bool failed = false;
    bool timedOut = false;
    bool completed = false;
};
void stopInBackground(QProcess* process) {
    if (!process || process->state() == QProcess::NotRunning || process->property("stopping").toBool()) return;
    process->setProperty("stopping", true);
    process->terminate();
    QTimer::singleShot(250, process, [process] {
        if (process->state() != QProcess::NotRunning) process->kill();
    });
}
QString permissionText(const QByteArray& data, bool request) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return {};
    const auto object = document.object();
    if (object.size() != 5 || !object.value("protocol").isDouble() || object.value("protocol").toDouble() != 1 ||
        !object.value("accessibility").isBool() || !object.value("request").isBool() ||
        object.value("request").toBool() != request || !object.value("finder_status").isString() ||
        !object.value("finder_error").isDouble()) return {};
    const auto nativeError = object.value("finder_error").toDouble();
    if (!std::isfinite(nativeError) || std::floor(nativeError) != nativeError ||
        nativeError < std::numeric_limits<std::int32_t>::min() ||
        nativeError > std::numeric_limits<std::int32_t>::max()) return {};
    const auto finder = object.value("finder_status").toString();
    QString finderText;
    if (finder == "granted") finderText = QStringLiteral("已授权");
    else if (finder == "consent-required") finderText = QStringLiteral("需要系统授权");
    else if (finder == "denied") finderText = QStringLiteral("未获授权");
    else if (finder == "unavailable") finderText = QStringLiteral("暂不可用");
    else if (finder == "error") finderText = QStringLiteral("无法检查");
    else return {};
    const bool accessibility = object.value("accessibility").toBool();
    auto text = QStringLiteral("辅助功能：%1\nFinder：%2").arg(
        accessibility ? QStringLiteral("已授权") : QStringLiteral("未获授权"), finderText);
    if (!accessibility) text += QStringLiteral("\n辅助功能需在系统设置为当前应用开启；Finder 允许弹窗不会开启辅助功能。开启后请再次检查。");
    return text;
}
} // namespace
DirectorySyncSetupDialog::DirectorySyncSetupDialog(QWidget* parent)
    : DirectorySyncSetupDialog(HelperConfig{QCoreApplication::applicationFilePath(), {}, 15000, 300000}, parent) {}
DirectorySyncSetupDialog::DirectorySyncSetupDialog(HelperConfig helper, QWidget* parent)
    : DirectorySyncSetupDialog(std::move(helper), productionGuidance(), parent) {}
DirectorySyncSetupDialog::DirectorySyncSetupDialog(HelperConfig helper, GuidanceActions guidance, QWidget* parent)
    : QDialog(parent), helper_(std::move(helper)), guidance_(std::move(guidance)) {
    setWindowTitle(QStringLiteral("Everything Lite — 目录联动权限"));
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(QStringLiteral("原生目录联动正在实现。获得权限不等于已启用目录联动。\n"
        "检查权限不会弹出授权提示。请求系统授权时，请处理 Finder 允许弹窗。\n"
        "辅助功能需单独在系统设置的“隐私与安全性 → 辅助功能”中为当前应用开启。\n"
        "可点击“显示当前应用”确认本次应用；开启后请再次检查。"), this);
    explanation->setObjectName("permissionExplanation"); explanation->setWordWrap(true); layout->addWidget(explanation);
    status_ = new QLabel(QStringLiteral("正在检查权限……"), this);
    status_->setObjectName("permissionStatus"); status_->setWordWrap(true); layout->addWidget(status_);
    auto* guidanceRow = new QHBoxLayout;
    auto* settings = new QPushButton(QStringLiteral("打开辅助功能设置"), this);
    settings->setObjectName("openAccessibilitySettings");
    auto* reveal = new QPushButton(QStringLiteral("显示当前应用"), this);
    reveal->setObjectName("revealCurrentApplication");
    guidanceRow->addWidget(settings); guidanceRow->addWidget(reveal); layout->addLayout(guidanceRow);
    connect(settings, &QPushButton::clicked, this, [this] {
        if (closed_) return;
        const QUrl url(QStringLiteral("x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"));
        if (!guidance_.openSettings || !guidance_.openSettings(url))
            status_->setText(QStringLiteral("无法打开辅助功能设置。请手动进入系统设置 → 隐私与安全性 → 辅助功能。"));
    });
    connect(reveal, &QPushButton::clicked, this, [this] {
        if (closed_) return;
        const auto bundle = applicationBundle(guidance_.applicationFilePath);
        if (bundle.isEmpty() || !guidance_.revealApplication) {
            status_->setText(QStringLiteral("无法定位当前应用包。请在系统设置中手动选择当前应用。"));
            return;
        }
        if (!guidance_.revealApplication(bundle))
            status_->setText(QStringLiteral("无法显示当前应用。请在系统设置中手动选择当前应用。"));
    });
    auto* row = new QHBoxLayout;
    check_ = new QPushButton(QStringLiteral("检查权限"), this); check_->setObjectName("checkPermissions");
    request_ = new QPushButton(QStringLiteral("请求系统授权"), this); request_->setObjectName("requestPermissions");
    auto* close = new QPushButton(QStringLiteral("关闭"), this);
    row->addWidget(check_);row->addWidget(request_);row->addWidget(close);layout->addLayout(row);
    connect(check_, &QPushButton::clicked, this, [this] { startProbe(false); });
    connect(request_, &QPushButton::clicked, this, [this] { startProbe(true); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    check_->setEnabled(false); request_->setEnabled(false);
    resize(560, 300);
    QTimer::singleShot(0, this, [this] { startProbe(false); });
}
DirectorySyncSetupDialog::~DirectorySyncSetupDialog() { cancelProbe(); }
void DirectorySyncSetupDialog::done(int result) { closed_ = true; cancelProbe(); QDialog::done(result); }
void DirectorySyncSetupDialog::cancelProbe() { ++generation_; stopInBackground(active_); }
void DirectorySyncSetupDialog::startProbe(bool request) {
    if (closed_ || active_) return;
    const auto generation = ++generation_;
    auto* process = new QProcess; // No dialog parent: closing must never wait for a child.
    active_ = process;
    const QPointer<DirectorySyncSetupDialog> guard(this);
    auto output = std::make_shared<ProbeOutput>();
    auto* timer = new QTimer(process); timer->setSingleShot(true);
    check_->setEnabled(false);request_->setEnabled(false);
    status_->setText(request ? QStringLiteral("等待系统授权。请处理系统提示……") : QStringLiteral("正在检查权限……"));
    auto readOutput = [process, output] {
        if (output->bytes > outputBudget) return;
        for (const auto channel : {QProcess::StandardOutput, QProcess::StandardError}) {
            process->setReadChannel(channel);
            const auto chunk = process->read(outputBudget - output->bytes + 1);
            output->bytes += chunk.size();
            if (output->bytes > outputBudget) {
                output->failed = true; process->kill(); return;
            }
            if (channel == QProcess::StandardOutput) output->stdoutData += chunk;
        }
    };
    connect(process, &QProcess::readyReadStandardOutput, process, readOutput);
    connect(process, &QProcess::readyReadStandardError, process, readOutput);
    auto finish = [guard, generation, process, output, timer, request, readOutput](int code, QProcess::ExitStatus exit) {
        if (output->completed) return;
        output->completed = true; timer->stop(); readOutput();
        if (guard && !guard->closed_ && guard->generation_ == generation) {
            guard->active_.clear(); guard->check_->setEnabled(true);guard->request_->setEnabled(true);
            auto text = !output->failed && exit == QProcess::NormalExit && code == 0
                ? permissionText(output->stdoutData, request) : QString{};
            if (output->timedOut) text = QStringLiteral("权限检查超时。请重试。");
            else if (text.isEmpty()) text = QStringLiteral("权限检查失败。请重试。");
            guard->status_->setText(text);
        }
        process->deleteLater();
    };
    connect(process, qOverload<int,QProcess::ExitStatus>(&QProcess::finished), process, finish);
    connect(process, &QProcess::errorOccurred, process, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish(-1, QProcess::CrashExit);
    });
    connect(timer, &QTimer::timeout, process, [guard, generation, process, output] {
        output->timedOut = true; output->failed = true;
        if (guard && !guard->closed_ && guard->generation_ == generation)
            guard->status_->setText(QStringLiteral("权限检查超时。请重试。"));
        stopInBackground(process);
    });
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, process, [process] {
        if (process->state() != QProcess::NotRunning) process->kill();
    });
    connect(process, &QProcess::started, process, [process] {
        if (process->property("stopping").toBool()) process->kill();
    });
    QStringList arguments = helper_.arguments;
    arguments << QStringLiteral("--directory-sync-permission-probe");
    if (request) arguments << QStringLiteral("--request");
    process->start(helper_.program, arguments, QIODevice::ReadOnly);
    timer->start(qMax(1, request ? helper_.requestTimeoutMs : helper_.readTimeoutMs));
}
} // namespace everything_lite
