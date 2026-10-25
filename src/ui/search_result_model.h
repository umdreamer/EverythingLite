#pragma once

#include "core/file_record.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QIcon>

#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace everything_lite {

class SearchResultModel final : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit SearchResultModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    void setResults(std::vector<SearchResult> results, QHash<QString, qint64> open_times = {});
    void appendResults(std::vector<SearchResult> results, QHash<QString, qint64> open_times = {});
    void clearResults();
    void markOpenRequest(const QString& path, qint64 at);
    void markPathMissing(const QString& path);
    void markPathPresent(const QString& path);
    QString pathAt(int row) const;
    QString nameAt(int row) const;
    bool isDirectoryAt(int row) const;
    bool canVerifyAt(int row) const;
    QString actionBlockAt(int row) const;

private:
    mutable QHash<QString,QIcon> app_icons_;
    QHash<QString, qint64> open_times_;
    QHash<QString, bool> verified_presence_;
    std::vector<SearchResult> results_;
    std::set<std::pair<std::int64_t, std::string>> membership_;
    std::unordered_map<std::string, std::size_t> path_membership_;
    int sort_column_ = -1;
    Qt::SortOrder sort_order_ = Qt::AscendingOrder;

    void appendUniqueResults(std::vector<SearchResult> results);
    void applySort();
    QString statusFor(const SearchResult& row) const;
    void notifyStatusChanged(const QString& path);
    static QString formatSize(std::uint64_t bytes);
};

} // namespace everything_lite
