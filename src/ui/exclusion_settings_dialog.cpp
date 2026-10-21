#include "ui/exclusion_settings_dialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace everything_lite {
namespace {
void addUnique(QListWidget* list, const QString& path) {
    if (!path.isEmpty() && list->findItems(path,Qt::MatchExactly).isEmpty()) list->addItem(path);
}
QStringList values(const QListWidget* list) {
    QStringList out;
    for (int i=0;i<list->count();++i) out << list->item(i)->text();
    return out;
}
}

ExclusionSettingsDialog::ExclusionSettingsDialog(const ExclusionRules& rules,QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("exclusionSettingsDialog"));
    setWindowTitle(QStringLiteral("排除规则"));
    resize(680,660);
    auto* layout=new QVBoxLayout(this);
    auto* hint=new QLabel(QStringLiteral("排除规则作用于索引和文件变更。放宽规则后需重建索引，才能恢复此前排除的记录。"),this);
    hint->setWordWrap(true);layout->addWidget(hint);

    auto addPaths=[&](const QString& title,const std::vector<std::string>& initial,
                      bool directory,QListWidget*& list,const QString& object_name) {
        auto* group=new QGroupBox(title,this);auto* group_layout=new QVBoxLayout(group);
        list=new QListWidget(group);list->setObjectName(object_name);
        for(const auto& path:initial) addUnique(list,QString::fromUtf8(path.c_str()));
        group_layout->addWidget(list);
        auto* row=new QHBoxLayout();
        auto* add=new QPushButton(directory?QStringLiteral("添加目录…"):QStringLiteral("添加文件…"),group);
        auto* remove=new QPushButton(QStringLiteral("移除所选"),group);
        row->addWidget(add);row->addWidget(remove);row->addStretch();group_layout->addLayout(row);
        connect(add,&QPushButton::clicked,this,[this,list,directory]{
            const auto path=directory
                ? QFileDialog::getExistingDirectory(this,QStringLiteral("选择排除目录"))
                : QFileDialog::getOpenFileName(this,QStringLiteral("选择排除文件"));
            addUnique(list,path);
        });
        connect(remove,&QPushButton::clicked,this,[list]{
            qDeleteAll(list->selectedItems());
        });
        layout->addWidget(group,1);
    };
    addPaths(QStringLiteral("排除目录（精确路径及全部后代）"),rules.directories,true,directories_,QStringLiteral("excludedDirectories"));
    addPaths(QStringLiteral("排除文件（精确路径）"),rules.files,false,files_,QStringLiteral("excludedFiles"));

    auto addFilter=[&](const QString& label,const std::string& value,QPlainTextEdit*& edit,const QString& name) {
        layout->addWidget(new QLabel(label,this));
        edit=new QPlainTextEdit(this);edit->setObjectName(name);edit->setFixedHeight(56);
        edit->setPlaceholderText(QStringLiteral("*.tmp;regex:^draft_[0-9]+\\.txt$"));
        edit->setPlainText(QString::fromUtf8(value.c_str()));layout->addWidget(edit);
    };
    addFilter(QStringLiteral("排除文件夹过滤器（分号分隔，可用 *、**、?、regex:）"),rules.folder_filter,folder_filter_,QStringLiteral("folderFilterEdit"));
    addFilter(QStringLiteral("排除文件过滤器（分号分隔，可用 *、**、?、regex:）"),rules.file_filter,file_filter_,QStringLiteral("fileFilterEdit"));
    error_=new QLabel(this);error_->setObjectName(QStringLiteral("exclusionError"));
    error_->setWordWrap(true);error_->setStyleSheet(QStringLiteral("color: #b00020;"));layout->addWidget(error_);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,this);
    connect(buttons,&QDialogButtonBox::accepted,this,&ExclusionSettingsDialog::accept);
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    layout->addWidget(buttons);
}

ExclusionRules ExclusionSettingsDialog::rules() const {
    ExclusionRules out;
    for(const auto& path:values(directories_)) out.directories.push_back(path.toUtf8().toStdString());
    for(const auto& path:values(files_)) out.files.push_back(path.toUtf8().toStdString());
    out.folder_filter=folder_filter_->toPlainText().trimmed().toUtf8().toStdString();
    out.file_filter=file_filter_->toPlainText().trimmed().toUtf8().toStdString();
    return out;
}

void ExclusionSettingsDialog::accept() {
    try {
        ExclusionPolicy validated(rules());
        error_->clear();
        if(saveRequested) saveRequested(rules());
        else QDialog::accept();
    } catch(const std::exception& error) {
        error_->setText(QStringLiteral("规则无效：%1").arg(QString::fromUtf8(error.what())));
    }
}

void ExclusionSettingsDialog::showError(const QString& message) {
    error_->setText(message);
}

void ExclusionSettingsDialog::setSaving(bool saving) {
    setEnabled(!saving);
}

void ExclusionSettingsDialog::finishSaved() {
    QDialog::accept();
}

} // namespace everything_lite
