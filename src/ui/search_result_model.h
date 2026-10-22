#pragma once

#include "core/file_record.h"

#include <QAbstractTableModel>

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

    void setResults(std::vector<SearchResult> results);
    void appendResults(std::vector<SearchResult> results);
    void clearResults();
    QString pathAt(int row) const;
    QString nameAt(int row) const;
    bool isDirectoryAt(int row) const;
    QString actionBlockAt(int row) const;

private:
    std::vector<SearchResult> results_;
    std::set<std::pair<std::int64_t, std::string>> membership_;
    std::unordered_map<std::string, std::size_t> path_membership_;
    int sort_column_ = -1;
    Qt::SortOrder sort_order_ = Qt::AscendingOrder;

    void appendUniqueResults(std::vector<SearchResult> results);
    void applySort();
    static QString formatSize(std::uint64_t bytes);
};

} // namespace everything_lite
