#include "ui/file_preview.h"
#include <QDesktopServices>
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringDecoder>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <memory>
namespace everything_lite {
PreviewResult loadFilePreview(const QString& path) {
    PreviewResult result;
    const QFileInfo info(path);
    if (!info.exists()) {
        result.message = QStringLiteral("文件不存在");
        return result;
    }
    result.message =
        QStringLiteral("名称：%1\n大小：%2 字节\n路径：%3").arg(info.fileName()).arg(info.size()).arg(path);
    if (info.isDir()) {
        result.message += QStringLiteral("\n目录：不递归读取内容");
        return result;
    }
    if (!info.isFile()) {
        result.message += QStringLiteral("\n此文件类型不支持基础预览");
        return result;
    }
    // Inspect permission bits too: tests or elevated runs must not accidentally
    // claim a chmod-000 file is an ordinary readable document.
    const auto reads = QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther;
    if (!(info.permissions() & reads)) {
        result.message += QStringLiteral("\n文件不可读");
        return result;
    }
    if (info.size() == 0) {
        result.message += QStringLiteral("\n空文件");
        return result;
    }
    if (info.size() > 16 * 1024 * 1024) {
        result.message += QStringLiteral("\n文件过大，仅显示元数据");
        return result;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.message += QStringLiteral("\n文件不可读：%1").arg(file.errorString());
        return result;
    }
    auto bytes = file.read(16 * 1024 * 1024 + 1);
    if (bytes.size() > 16 * 1024 * 1024) {
        result.message += QStringLiteral("\n文件过大，仅显示元数据");
        return result;
    }
    if (file.error() != QFileDevice::NoError) {
        result.message += QStringLiteral("\n文件读取失败");
        return result;
    }
    if (bytes.isEmpty()) {
        result.message += QStringLiteral("\n空文件");
        return result;
    }
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    // The size/header checks happen off the GUI thread before image decoding.
    if (reader.canRead()) {
        const auto size = reader.size();
        const qint64 pixels = qint64(size.width()) * size.height();
        if (!size.isValid() || pixels > 16 * 1024 * 1024 || pixels * 16 > 64 * 1024 * 1024) {
            result.message += QStringLiteral("\n图片尺寸过大或无效，仅显示元数据");
            return result;
        }
        reader.setScaledSize(size.scaled(1200, 900, Qt::KeepAspectRatio));
        result.image = reader.read();
        if (!result.image.isNull()) {
            result.kind = PreviewKind::Image;
            return result;
        }
        result.message += QStringLiteral("\n图片解码失败，仅显示元数据");
        return result;
    }
    if (bytes.size() > 1024 * 1024) {
        result.message += QStringLiteral("\n文本文件过大，仅显示元数据");
        return result;
    }
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString text = decoder(bytes);
    if (decoder.hasError() || bytes.contains('\0')) {
        result.message += QStringLiteral("\n不是有效 UTF-8 文本，或为二进制文件");
        return result;
    }
    for (const auto ch : QString(text)) {
        if (ch.unicode() < 32 && ch != '\n' && ch != '\r' && ch != '\t') {
            result.message += QStringLiteral("\n此二进制类型不支持基础预览");
            return result;
        }
    }
    result.kind = PreviewKind::Text;
    result.text = QString(text).left(100000);
    if (QString(text).size() > 100000)
        result.message += QStringLiteral("\n仅呈现前 100000 个字符");
    return result;
}
FilePreview::FilePreview(const QString& path, QWidget* parent) : QDialog(parent) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("快速查看 — %1").arg(QFileInfo(path).fileName()));
    resize(800, 600);
    auto* layout = new QVBoxLayout(this);
    auto* info = new QLabel(QStringLiteral("正在加载受限预览…"), this);
    info->setTextFormat(Qt::PlainText);
    info->setWordWrap(true);
    layout->addWidget(info);
    auto* open = new QPushButton(QStringLiteral("用系统应用打开"), this);
    layout->addWidget(open);
    connect(open, &QPushButton::clicked, this, [this, path, info] {
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
            info->setText(QStringLiteral("系统未接受打开请求"));
    });
    auto result = std::make_shared<PreviewResult>();
    auto* worker = QThread::create([path, result] { *result = loadFilePreview(path); });
    // Receiver-bound finished callback is discarded if this dialog is closed;
    // the worker owns only a bounded result and can finish independently.
    connect(worker, &QThread::finished, this, [this, result, info, layout] {
        info->setText(result->message);
        if (result->kind == PreviewKind::Text) {
            auto* text = new QPlainTextEdit(this);
            text->setReadOnly(true);
            text->setPlainText(result->text);
            layout->insertWidget(1, text, 1);
        } else if (result->kind == PreviewKind::Image) {
            auto* image = new QLabel(this);
            image->setAlignment(Qt::AlignCenter);
            image->setPixmap(QPixmap::fromImage(result->image));
            layout->insertWidget(1, image, 1);
        }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}
} // namespace everything_lite
