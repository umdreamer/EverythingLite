#include "ui/desktop_actions.h"
#include <QDesktopServices>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>
#include <memory>
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif
namespace everything_lite {
DesktopActions::DesktopActions(QObject* parent) : QObject(parent) {}
bool DesktopActions::launch(const QString& program, const QStringList& arguments) {
    if (QProcess::startDetached(program, arguments))
        return true;
    emit feedback(QStringLiteral("无法启动应用：%1").arg(program));
    return false;
}
#ifdef Q_OS_MACOS
void DesktopActions::launchNative(const QString& program, const QStringList& arguments,
                                  std::function<bool()> is_current, std::function<void()> on_failure) {
    // Native launchers can fail after exec succeeds.
    // It owns its cleanup independently, so deleting the window cancels only
    // UI callbacks rather than killing an unrelated system application.
    auto* process = new QProcess;
    const auto reported = std::make_shared<bool>(false);
    auto report = [this, program, is_current, on_failure, reported](const QString& reason) {
        if (*reported)
            return;
        *reported = true;
        if (is_current && !is_current())
            return;
        emit feedback(QStringLiteral("应用请求失败：%1\n%2").arg(program, reason));
        if (on_failure)
            on_failure();
    };
    connect(process, &QProcess::errorOccurred, this, [process, report](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            report(process->errorString());
    });
    connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            process->deleteLater();
    });
    connect(process, &QProcess::finished, this, [process, report](int code, QProcess::ExitStatus status) {
        if (code != 0 || status != QProcess::NormalExit) {
            const auto reason = QString::fromUtf8(process->readAllStandardError().left(4096)).trimmed();
            report(reason.isEmpty() ? QStringLiteral("退出代码：%1").arg(code) : reason);
        }
    });
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    process->start(program, arguments);
}
#endif
void DesktopActions::open(const QString& path) {
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        emit feedback(QStringLiteral("系统未接受打开请求：%1").arg(path));
}
bool DesktopActions::openWithExecutable(const QString& path, const QString& executable) {
    const QFileInfo info(executable);
    if (!info.isFile() || !info.isExecutable()) {
        emit feedback(QStringLiteral("请选择具有执行权限的普通应用文件：%1").arg(executable));
        return false;
    }
    // Never pass a command line or .desktop Exec string to a shell.
    return launch(info.absoluteFilePath(), {path});
}
void DesktopActions::openWithApplication(const QString& path, const QString& application) {
#ifdef Q_OS_MACOS
    launchNative(QStringLiteral("/usr/bin/open"), {QStringLiteral("-a"), application, path});
#else
    openWithExecutable(path, application);
#endif
}
void DesktopActions::reveal(const QString& path) {
    const auto sequence = ++reveal_sequence_;
#ifdef Q_OS_MACOS
    launchNative(QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), path},
                 [this, sequence] { return sequence == reveal_sequence_; });
#elif defined(Q_OS_LINUX)
    auto message = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("/org/freedesktop/FileManager1"),
        QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("ShowItems"));
    message << QStringList{QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded)} << QString{};
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 1500), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, path, sequence](QDBusPendingCallWatcher* result) {
                const QDBusPendingReply<> reply = *result;
                result->deleteLater();
                if (sequence != reveal_sequence_ || !reply.isError())
                    return;
                const auto parent = QFileInfo(path).absolutePath();
                const bool accepted = QDesktopServices::openUrl(QUrl::fromLocalFile(parent));
                emit feedback(
                    accepted
                        ? QStringLiteral("文件管理器定位失败，已提交打开父目录的请求：%1").arg(parent)
                        : QStringLiteral("文件管理器定位失败，系统也未接受打开父目录的请求：%1").arg(parent));
            });
#else
    Q_UNUSED(sequence)
    open(QFileInfo(path).absolutePath());
#endif
}
void DesktopActions::preview(const QString& path) {
    const auto sequence = ++preview_sequence_;
#ifdef Q_OS_MACOS
    launchNative(
        QStringLiteral("/usr/bin/qlmanage"), {QStringLiteral("-p"), path},
        [this, sequence] { return sequence == preview_sequence_; },
        [this, path] { emit previewFallback(path); });
#elif defined(Q_OS_LINUX)
    // Ubuntu 24.04 Sushi 46 contract is ssb. An empty window handle is honest
    // on Wayland; a numeric Qt winId is not a valid exported Wayland handle.
    auto message = QDBusMessage::createMethodCall(
        QStringLiteral("org.gnome.NautilusPreviewer"), QStringLiteral("/org/gnome/NautilusPreviewer"),
        QStringLiteral("org.gnome.NautilusPreviewer2"), QStringLiteral("ShowFile"));
    message << QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded) << QString{} << false;
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 1500), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, path, sequence](QDBusPendingCallWatcher* result) {
                const QDBusPendingReply<> reply = *result;
                result->deleteLater();
                if (sequence != preview_sequence_ || !reply.isError())
                    return;
                emit feedback(
                    QStringLiteral("系统快速查看不可用，使用基础预览：%1").arg(reply.error().message()));
                emit previewFallback(path);
            });
#else
    Q_UNUSED(sequence)
    emit previewFallback(path);
#endif
}
} // namespace everything_lite
