#pragma once

#include "core/exclusion_policy.h"
#include <QDialog>
#include <functional>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QCheckBox;
class QDialogButtonBox;

namespace everything_lite {

class ExclusionSettingsDialog final : public QDialog {
public:
    explicit ExclusionSettingsDialog(const ExclusionRules& rules, QWidget* parent = nullptr);
    ExclusionRules rules() const;
    void accept() override;
    void showError(const QString& message);
    void setSaving(bool saving);
    void setSaveProgress(std::uint64_t checked,std::uint64_t total,int elapsed_seconds);
    void finishSaved();
    std::function<void(ExclusionRules)> saveRequested;

private:
    QListWidget* directories_ = nullptr;
    QListWidget* files_ = nullptr;
    QPlainTextEdit* folder_filter_ = nullptr;
    QPlainTextEdit* file_filter_ = nullptr;
    QLabel* error_ = nullptr;
    QLabel* saving_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QCheckBox* app_contents_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
};

} // namespace everything_lite
