#pragma once

#include <QDialog>
#include <QStringList>

class QListWidget;

namespace everything_lite {

class RootSettingsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit RootSettingsDialog(const QStringList& roots, QWidget* parent = nullptr);
    QStringList roots() const;

private:
    QListWidget* list_ = nullptr;
    void addRoot();
    void removeSelected();
};

} // namespace everything_lite
