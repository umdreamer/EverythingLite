#include "ui/search_result_model.h"
#include "ui/result_presentation.h"

#include <QDateTime>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QString>

#include <algorithm>
#include <tuple>

namespace everything_lite {

SearchResultModel::SearchResultModel(QObject* parent) : QAbstractTableModel(parent) {}

int SearchResultModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(results_.size());
}

int SearchResultModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : 6;
}

QVariant SearchResultModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(results_.size())) return {};
    const auto& row = results_[static_cast<std::size_t>(index.row())];
    const auto& file = row.file;
    const auto app_bundle=file.is_directory && QString::fromUtf8(file.name.c_str()).endsWith(QStringLiteral(".app"),Qt::CaseInsensitive);

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case 0: return QString::fromUtf8(file.name.c_str())+(app_bundle?QStringLiteral("（程序包）"):QString{});
            case 1: return QString::fromUtf8(file.parent_path.c_str());
            case 2: return file.is_directory ? QStringLiteral("—") : formatSize(file.size);
            case 3: return file.modified_time > 0
                ? QDateTime::fromSecsSinceEpoch(file.modified_time).toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                : QString{};
            case 4: return sourceText(row);
            case 5: return statusFor(row);
            default: return {};
        }
    }
    if(role==Qt::DecorationRole && index.column()==0 && app_bundle) {
        const auto path=QString::fromUtf8(file.path.c_str());
        auto found=app_icons_.constFind(path);
        if(found!=app_icons_.cend())return *found;
        QFileIconProvider provider;
        auto icon=provider.icon(QFileInfo(path));
        if(icon.isNull())icon=QIcon::fromTheme(QStringLiteral("application-x-executable"),provider.icon(QFileIconProvider::Folder));
        if(app_icons_.size()>=256)app_icons_.clear();
        app_icons_.insert(path,icon);
        return icon;
    }
    if (role == Qt::TextAlignmentRole && (index.column() == 2 || index.column() == 3)) {
        return static_cast<int>(Qt::AlignVCenter | Qt::AlignRight);
    }
    if (role == Qt::ToolTipRole) return resultTooltip(row, statusFor(row));
    if (role == Qt::UserRole) {
        switch (index.column()) {
            case 0: return QString::fromUtf8(file.name.c_str()).toCaseFolded();
            case 1: return QString::fromUtf8(file.parent_path.c_str()).toCaseFolded();
            case 2: return QVariant::fromValue<qulonglong>(file.size);
            case 3: return QVariant::fromValue<qlonglong>(file.modified_time);
            case 4: return sourceText(row);
            case 5: return statusFor(row);
            default: return {};
        }
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
        case 4: return QStringLiteral("来源");
        case 5: return QStringLiteral("状态");
        default: return {};
    }
}

void SearchResultModel::sort(int column, Qt::SortOrder order) {
    if (column < 0 || column >= 6) return;
    sort_column_ = column;
    sort_order_ = order;
    emit layoutAboutToBeChanged();
    applySort();
    emit layoutChanged();
}

void SearchResultModel::setResults(std::vector<SearchResult> results, QHash<QString, qint64> open_times) {
    beginResetModel();
    results_.clear();
    membership_.clear();
    path_membership_.clear();
    open_times_ = std::move(open_times);
    verified_presence_.clear();
    appendUniqueResults(std::move(results));
    applySort();
    endResetModel();
}

void SearchResultModel::appendResults(std::vector<SearchResult> results, QHash<QString, qint64> open_times) {
    if (results.empty()) return;
    // Because users may have selected a local column sort, append and re-sort
    // the loaded window as one model update. Server-side global sorting can be
    // added later without changing the incremental-loading API.
    beginResetModel();
    for (auto it = open_times.cbegin(); it != open_times.cend(); ++it) open_times_.insert(it.key(), it.value());
    appendUniqueResults(std::move(results));
    applySort();
    endResetModel();
}

void SearchResultModel::clearResults() {
    beginResetModel();
    results_.clear();
    membership_.clear();
    path_membership_.clear();
    open_times_.clear();
    verified_presence_.clear();
    endResetModel();
}

QString SearchResultModel::statusFor(const SearchResult& row) const {
    if (row.source_status != SourceStatus::Accessible || row.file.cloud_copy == CloudCopy::RemoteOnly)
        return statusText(row);
    const auto path = QString::fromUtf8(row.file.path.c_str());
    if (verified_presence_.contains(path) && !verified_presence_.value(path)) {
        return row.source_type == SourceType::Local || row.source_type == SourceType::Unknown
            ? QStringLiteral("原位置不存在") : QStringLiteral("来源不可用或原位置未找到");
    }
    const auto opened = open_times_.value(path);
    return opened > 0
        ? QStringLiteral("本应用上次请求打开 %1").arg(QDateTime::fromSecsSinceEpoch(opened).toString(QStringLiteral("yyyy-MM-dd HH:mm")))
        : QStringLiteral("本应用无打开记录");
}

void SearchResultModel::notifyStatusChanged(const QString& path) {
    for (int row = 0; row < static_cast<int>(results_.size()); ++row) {
        if (QString::fromUtf8(results_[static_cast<std::size_t>(row)].file.path.c_str()) != path) continue;
        emit dataChanged(index(row, 5), index(row, 5), {Qt::DisplayRole, Qt::ToolTipRole, Qt::UserRole});
    }
}

void SearchResultModel::markOpenRequest(const QString& path, qint64 at) {
    if (path.isEmpty() || at <= 0) return;
    open_times_.insert(path, at);
    notifyStatusChanged(path);
}

void SearchResultModel::markPathMissing(const QString& path) {
    verified_presence_.insert(path, false);
    notifyStatusChanged(path);
}

void SearchResultModel::markPathPresent(const QString& path) {
    verified_presence_.insert(path, true);
    notifyStatusChanged(path);
}

void SearchResultModel::appendUniqueResults(std::vector<SearchResult> results) {
    auto priority=[](const SearchResult& row) {
        int status=0;
        switch(row.source_status) {
            case SourceStatus::Accessible: status=3; break;
            case SourceStatus::Unverified: status=2; break;
            case SourceStatus::Failed: status=1; break;
            case SourceStatus::Offline: break;
        }
        return std::make_tuple(status,row.file.cloud_copy!=CloudCopy::RemoteOnly,
                               row.source_last_success,row.file.source_id);
    };
    for (auto& row : results) {
        const auto& file = row.file;
        // Legacy/manual rows without a complete identity remain independent.
        if (file.source_id > 0 && !file.relative_path.empty()
            && !membership_.emplace(file.source_id, file.relative_path).second) continue;
        if(file.source_id>0 && !file.relative_path.empty() && !file.path.empty()) {
            const auto found=path_membership_.find(file.path);
            if(found!=path_membership_.end()) {
                if(priority(row)>priority(results_[found->second]))results_[found->second]=std::move(row);
                continue;
            }
            path_membership_.emplace(file.path,results_.size());
        }
        results_.push_back(std::move(row));
    }
}

QString SearchResultModel::pathAt(int row) const {
    if (row < 0 || row >= static_cast<int>(results_.size())) return {};
    return QString::fromUtf8(results_[static_cast<std::size_t>(row)].file.path.c_str());
}

QString SearchResultModel::nameAt(int row) const {
    if (row < 0 || row >= static_cast<int>(results_.size())) return {};
    return QString::fromUtf8(results_[static_cast<std::size_t>(row)].file.name.c_str());
}

bool SearchResultModel::isDirectoryAt(int row) const {
    if (row < 0 || row >= static_cast<int>(results_.size())) return false;
    return results_[static_cast<std::size_t>(row)].file.is_directory;
}

QString SearchResultModel::actionBlockAt(int row) const {
    if (row < 0 || row >= static_cast<int>(results_.size())) return {};
    return cachedActionBlock(results_[static_cast<std::size_t>(row)]);
}

void SearchResultModel::applySort() {
    if (sort_column_ < 0) return;
    const auto column = sort_column_;
    const auto order = sort_order_;
    std::stable_sort(results_.begin(), results_.end(), [this, column, order](const SearchResult& a, const SearchResult& b) {
        int cmp = 0;
        switch (column) {
            case 0: cmp = QString::fromUtf8(a.file.name.c_str()).compare(QString::fromUtf8(b.file.name.c_str()), Qt::CaseInsensitive); break;
            case 1: cmp = QString::fromUtf8(a.file.parent_path.c_str()).compare(QString::fromUtf8(b.file.parent_path.c_str()), Qt::CaseInsensitive); break;
            case 2: cmp = a.file.size < b.file.size ? -1 : (a.file.size > b.file.size ? 1 : 0); break;
            case 3: cmp = a.file.modified_time < b.file.modified_time ? -1 : (a.file.modified_time > b.file.modified_time ? 1 : 0); break;
            case 4: cmp = sourceText(a).compare(sourceText(b), Qt::CaseInsensitive); break;
            case 5: cmp = statusFor(a).compare(statusFor(b), Qt::CaseInsensitive); break;
            default: break;
        }
        if (cmp == 0) cmp = a.file.path.compare(b.file.path);
        return order == Qt::AscendingOrder ? cmp < 0 : cmp > 0;
    });
    path_membership_.clear();
    for(std::size_t i=0;i<results_.size();++i) {
        const auto& file=results_[i].file;
        if(file.source_id>0 && !file.relative_path.empty() && !file.path.empty())path_membership_.emplace(file.path,i);
    }
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
