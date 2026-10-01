#include "ui/root_settings_dialog.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace everything_lite {

RootSettingsDialog::RootSettingsDialog(const QStringList& roots, QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("索引目录"));
    resize(620, 360);

    auto* layout = new QVBoxLayout(this);
    auto* hint = new QLabel(QStringLiteral("只索引需要被 Everything Lite 搜索的目录。目录可随时修改，修改后建议重建索引。"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    list_ = new QListWidget(this);
    list_->addItems(roots);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    layout->addWidget(list_, 1);

    auto* actionRow = new QHBoxLayout();
    auto* addButton = new QPushButton(QStringLiteral("添加目录…"), this);
    auto* removeButton = new QPushButton(QStringLiteral("移除"), this);
    actionRow->addWidget(addButton);
    actionRow->addWidget(removeButton);
    actionRow->addStretch();
    layout->addLayout(actionRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);

    connect(addButton, &QPushButton::clicked, this, &RootSettingsDialog::addRoot);
    connect(removeButton, &QPushButton::clicked, this, &RootSettingsDialog::removeSelected);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

QStringList RootSettingsDialog::roots() const {
    QStringList out;
    for (int i = 0; i < list_->count(); ++i) out << list_->item(i)->text();
    out.removeDuplicates();
    return out;
}

void RootSettingsDialog::addRoot() {
    const auto path = QFileDialog::getExistingDirectory(this, QStringLiteral("选择索引目录"));
    if (path.isEmpty()) return;
    const auto matches = list_->findItems(path, Qt::MatchExactly);
    if (matches.isEmpty()) list_->addItem(path);
}

void RootSettingsDialog::removeSelected() {
    const auto selected = list_->selectedItems();
    for (auto* item : selected) delete list_->takeItem(list_->row(item));
}

} // namespace everything_lite
