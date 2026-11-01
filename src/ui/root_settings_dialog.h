#pragma once

#include <QDialog>
#include <QStringList>
#include "core/update_policy.h"

class QListWidget;
class QSpinBox;
class QTableWidget;

namespace everything_lite {

class RootSettingsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit RootSettingsDialog(const QStringList& roots, QWidget* parent = nullptr,
                                int periodic_minutes = 15, const std::vector<UpdateRule>& rules = {});
    QStringList roots() const;
    int periodicMinutes() const;
    std::vector<UpdateRule> updateRules() const;

private:
    QListWidget* list_ = nullptr;
    QSpinBox* periodic_minutes_ = nullptr;
    QTableWidget* rules_ = nullptr;
    void addRoot();
    void removeSelected();
    void appendRule(const QString& path, UpdateCadence cadence);
};

} // namespace everything_lite
