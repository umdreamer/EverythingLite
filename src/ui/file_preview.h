#pragma once
#include <QDialog>
#include <QImage>
#include <QString>
namespace everything_lite {
enum class PreviewKind { Metadata, Text, Image };
struct PreviewResult {
    PreviewKind kind = PreviewKind::Metadata;
    QString message;
    QString text;
    QImage image;
};
// Bounded disk/decoder work, called on a background thread by FilePreview.
PreviewResult loadFilePreview(const QString& path);
class FilePreview final : public QDialog {
  public:
    explicit FilePreview(const QString& path, QWidget* parent = nullptr);
};
} // namespace everything_lite
