#pragma once

#include "core/file_record.h"

#include <QAbstractTableModel>
#include <QVector>

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

    void setResults(std::vector<SearchResult> results);
    QString pathAt(int row) const;
    bool isDirectoryAt(int row) const;

private:
    std::vector<SearchResult> results_;
    static QString formatSize(std::uint64_t bytes);
};

} // namespace everything_lite
