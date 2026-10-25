#pragma once
#include "core/file_record.h"
#include <QDateTime>
#include <QString>
namespace everything_lite {
// Presentation uses only the cached search row; never probes the filesystem.
inline QString sourceTypeText(SourceType type) {
    switch (type) {
        case SourceType::Local: return QStringLiteral("本地磁盘");
        case SourceType::NAS: return QStringLiteral("NAS");
        case SourceType::Cloud: return QStringLiteral("网盘");
        case SourceType::USB: return QStringLiteral("U盘");
        case SourceType::External: return QStringLiteral("移动硬盘/外置存储");
        case SourceType::Unknown: return QStringLiteral("本地磁盘");
    }
    return QStringLiteral("本地磁盘");
}
inline QString sourceText(const SearchResult& row) {
    const auto type = sourceTypeText(row.source_type);
    return row.source_label.empty() ? type : QStringLiteral("%1（%2）").arg(QString::fromUtf8(row.source_label.c_str()), type);
}
inline QString statusText(const SearchResult& row) {
    QString text;
    switch (row.source_status) {
        case SourceStatus::Accessible: text = QStringLiteral("上次可用"); break;
        case SourceStatus::Offline: text = QStringLiteral("离线缓存"); break;
        case SourceStatus::Unverified: text = QStringLiteral("未核验"); break;
        case SourceStatus::Failed: text = row.source_last_success > 0
            ? QStringLiteral("缓存结果（更新未完成）") : QStringLiteral("检查未完成"); break;
    }
    if (row.file.cloud_copy == CloudCopy::RemoteOnly) text += QStringLiteral(" · 仅云端（未下载）");
    else if (row.source_type == SourceType::Cloud) {
        text += row.file.cloud_copy == CloudCopy::LocalCopy ? QStringLiteral(" · 本地副本") : QStringLiteral(" · 副本未核验");
    }
    return text;
}
inline QString cachedTimestamp(std::int64_t time) {
    return time > 0 ? QDateTime::fromSecsSinceEpoch(time).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QStringLiteral("无记录");
}
inline QString resultTooltip(const SearchResult& row, const QString& app_status = {}) {
    auto plain = QStringLiteral("%1\n来源：%2\n状态：%3\n上次检查：%4\n上次可用：%5\n缓存信息不保证文件当前可访问。")
        .arg(QString::fromUtf8(row.file.path.c_str()), sourceText(row), statusText(row),
             cachedTimestamp(row.source_last_check), cachedTimestamp(row.source_last_success));
    if (!app_status.isEmpty()) plain += QStringLiteral("\n本应用记录：%1").arg(app_status);
    // Qt tooltips accept rich text. Escape the whole cached payload so legal
    // markup in a path/label stays literal and cannot load image resources.
    return QStringLiteral("<pre>%1</pre>").arg(plain.toHtmlEscaped());
}
inline QString cachedActionBlock(const SearchResult& row) {
    if (row.source_status == SourceStatus::Offline) return QStringLiteral("此项目来自离线缓存；请先恢复来源并同步。仍可复制路径。");
    if (row.file.cloud_copy == CloudCopy::RemoteOnly) return QStringLiteral("此项目仅在云端，未确认本地副本；请先在网盘应用中下载并同步。仍可复制路径。");
    return {};
}
} // namespace everything_lite
