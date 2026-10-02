#pragma once
#include <QObject>
#include <QString>
#include <cstdint>
#include <functional>
namespace everything_lite {
// Native desktop requests stay asynchronous; success means request acceptance,
// not confirmation that another application's window actually appeared.
class DesktopActions final : public QObject {
    Q_OBJECT
  public:
    explicit DesktopActions(QObject* parent = nullptr);
    void open(const QString& path);
    bool openWithExecutable(const QString& path, const QString& executable);
    void openWithApplication(const QString& path, const QString& application);
    void reveal(const QString& path);
    void preview(const QString& path);
  signals:
    void feedback(const QString& message);
    void previewFallback(const QString& path);

  private:
    std::uint64_t reveal_sequence_ = 0;
    std::uint64_t preview_sequence_ = 0;
    bool launch(const QString& program, const QStringList& arguments);
#ifdef Q_OS_MACOS
    void launchNative(const QString& program, const QStringList& arguments,
                      std::function<bool()> is_current = {}, std::function<void()> on_failure = {});
#endif
};
} // namespace everything_lite
