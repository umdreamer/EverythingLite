#include "ui/search_result_model.h"

#include <QDateTime>

#include <utility>

namespace everything_lite {

SearchResultModel::SearchResultModel(QObject* parent) : QAbstractTableModel(parent) {}

int SearchResultModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(results_.size());
}

int SearchResultModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : 4;
}

QVariant SearchResultModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(results_.size())) return {};
    const auto& file = results_[static_cast<std::size_t>(index.row())].file;

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case 0: return QString::fromUtf8(file.name.c_str());
            case 1: return QString::fromUtf8(file.parent_path.c_str());
            case 2: return file.is_directory ? QStringLiteral("—") : formatSize(file.size);
            case 3: return file.modified_time > 0
                ? QDateTime::fromSecsSinceEpoch(file.modified_time).toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                : QString{};
            default: return {};
        }
    }
    if (role == Qt::TextAlignmentRole && (index.column() == 2 || index.column() == 3)) {
        return static_cast<int>(Qt::AlignVCenter | Qt::AlignRight);
    }
    if (role == Qt::ToolTipRole) {
        return QString::fromUtf8(file.path.c_str());
    }
    return {};
}

QVariant SearchResultModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
        case 0: return QStringLiteral("名称");
        case 1: return QStringLiteral("所在位置");
        case 2: return QStringLiteral("大小");
        case 3: return QStringLiteral("修改时间");
        default: return {};
    }
}

void SearchResultModel::setResults(std::vector<SearchResult> results) {
    beginResetModel();
    results_ = std::move(results);
    endResetModel();
}

QString SearchResultModel::pathAt(int row) const {
    if (row < 0 || row >= static_cast<int>(results_.size())) return {};
    return QString::fromUtf8(results_[static_cast<std::size_t>(row)].file.path.c_str());
}

bool SearchResultModel::isDirectoryAt(int row) const {
    if (row < 0 || row >= static_cast<int>(results_.size())) return false;
    return results_[static_cast<std::size_t>(row)].file.is_directory;
}

QString SearchResultModel::formatSize(std::uint64_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0
        ? QStringLiteral("%1 B").arg(bytes)
        : QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(QString::fromLatin1(units[unit]));
}

} // namespace everything_lite
