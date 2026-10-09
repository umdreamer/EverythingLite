#include "ui/directory_sync_setup_dialog.h"
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
namespace everything_lite {
namespace {
constexpr qint64 outputBudget = 64 * 1024;
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
    return QStringLiteral("辅助功能：%1\nFinder：%2").arg(
        object.value("accessibility").toBool() ? QStringLiteral("已授权") : QStringLiteral("未获授权"), finderText);
}
} // namespace
DirectorySyncSetupDialog::DirectorySyncSetupDialog(QWidget* parent)
    : DirectorySyncSetupDialog(HelperConfig{QCoreApplication::applicationFilePath(), {}, 15000, 300000}, parent) {}
DirectorySyncSetupDialog::DirectorySyncSetupDialog(HelperConfig helper, QWidget* parent)
    : QDialog(parent), helper_(std::move(helper)) {
    setWindowTitle(QStringLiteral("Everything Lite — 目录联动权限"));
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(QStringLiteral("原生目录联动正在实现。获得权限不等于已启用目录联动。\n"
        "检查权限不会弹出授权提示。请求系统授权后，请按系统提示操作。\n"
        "辅助功能权限可在系统设置的“隐私与安全性 → 辅助功能”中开启；更改后可再次检查。"), this);
    explanation->setObjectName("permissionExplanation"); explanation->setWordWrap(true); layout->addWidget(explanation);
    status_ = new QLabel(QStringLiteral("正在检查权限……"), this);
    status_->setObjectName("permissionStatus"); status_->setWordWrap(true); layout->addWidget(status_);
    auto* row = new QHBoxLayout;
    check_ = new QPushButton(QStringLiteral("检查权限"), this); check_->setObjectName("checkPermissions");
    request_ = new QPushButton(QStringLiteral("请求系统授权"), this); request_->setObjectName("requestPermissions");
    auto* close = new QPushButton(QStringLiteral("关闭"), this);
    row->addWidget(check_);row->addWidget(request_);row->addWidget(close);layout->addLayout(row);
    connect(check_, &QPushButton::clicked, this, [this] { startProbe(false); });
    connect(request_, &QPushButton::clicked, this, [this] { startProbe(true); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    check_->setEnabled(false); request_->setEnabled(false);
    resize(520, 220);
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
