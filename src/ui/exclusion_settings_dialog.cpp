#include "ui/exclusion_settings_dialog.h"

#include <QDialogButtonBox>
#include <QCheckBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProgressBar>
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
    auto* hint=new QLabel(QStringLiteral("排除规则作用于索引和文件变更。默认只索引 .app 程序包本身；开启包内索引后会重建索引。其他规则放宽后需重建索引。"),this);
    hint->setWordWrap(true);layout->addWidget(hint);

    auto addPaths=[&](const QString& title,const std::vector<std::string>& initial,
                      bool directory,QListWidget*& list,const QString& object_name) {
        auto* group=new QGroupBox(title,this);auto* group_layout=new QVBoxLayout(group);
        list=new QListWidget(group);list->setObjectName(object_name);
        for(const auto& path:initial) addUnique(list,QString::fromUtf8(path.c_str()));
        group_layout->addWidget(list);
        auto* row=new QHBoxLayout();
        auto* input=new QLineEdit(group);
        input->setObjectName(directory?QStringLiteral("excludedDirectoryInput"):QStringLiteral("excludedFileInput"));
        input->setPlaceholderText(QStringLiteral("输入绝对路径，也可选择…"));
        auto* add=new QPushButton(QStringLiteral("添加"),group);
        auto* choose=new QPushButton(QStringLiteral("选择…"),group);
        auto* remove=new QPushButton(QStringLiteral("移除所选"),group);
        row->addWidget(input,1);row->addWidget(add);row->addWidget(choose);row->addWidget(remove);group_layout->addLayout(row);
        auto addTyped=[list,input]{addUnique(list,input->text());input->clear();};
        connect(add,&QPushButton::clicked,this,addTyped);
        connect(input,&QLineEdit::returnPressed,this,addTyped);
        connect(choose,&QPushButton::clicked,this,[this,list,directory]{
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
    app_contents_=new QCheckBox(QStringLiteral("索引 .app 程序包内部文件（默认关闭）"),this);
    app_contents_->setObjectName(QStringLiteral("indexAppBundleContents"));
    app_contents_->setChecked(rules.index_app_bundle_contents);
    layout->addWidget(app_contents_);
    error_=new QLabel(this);error_->setObjectName(QStringLiteral("exclusionError"));
    error_->setWordWrap(true);error_->setStyleSheet(QStringLiteral("color: #b00020;"));layout->addWidget(error_);
    saving_=new QLabel(QStringLiteral("正在保存排除规则并清理索引…"),this);
    saving_->setObjectName(QStringLiteral("exclusionSaving"));saving_->hide();layout->addWidget(saving_);
    progress_=new QProgressBar(this);progress_->setObjectName(QStringLiteral("exclusionProgressBar"));
    progress_->setRange(0,0);progress_->hide();layout->addWidget(progress_);
    buttons_=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,this);
    connect(buttons_,&QDialogButtonBox::accepted,this,&ExclusionSettingsDialog::accept);
    connect(buttons_,&QDialogButtonBox::rejected,this,&QDialog::reject);
    layout->addWidget(buttons_);
}

ExclusionRules ExclusionSettingsDialog::rules() const {
    ExclusionRules out;
    for(const auto& path:values(directories_)) out.directories.push_back(path.toUtf8().toStdString());
    for(const auto& path:values(files_)) out.files.push_back(path.toUtf8().toStdString());
    out.folder_filter=folder_filter_->toPlainText().trimmed().toUtf8().toStdString();
    out.file_filter=file_filter_->toPlainText().trimmed().toUtf8().toStdString();
    out.index_app_bundle_contents=app_contents_->isChecked();
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
    saving_->setVisible(saving);
    progress_->setVisible(saving);
    if(saving) {progress_->setRange(0,0);saving_->setText(QStringLiteral("正在保存排除规则并计算索引条数…"));}
    for(auto* group:findChildren<QGroupBox*>())group->setEnabled(!saving);
    folder_filter_->setEnabled(!saving);file_filter_->setEnabled(!saving);
    app_contents_->setEnabled(!saving);buttons_->setEnabled(!saving);
}

void ExclusionSettingsDialog::setSaveProgress(std::uint64_t checked,std::uint64_t total,int elapsed_seconds) {
    progress_->setRange(0,100);
    progress_->setValue(total ? static_cast<int>(std::min<std::uint64_t>(95,checked*95/total)) : 95);
    saving_->setText(total && checked<total
        ? QStringLiteral("正在检查索引：%1 / %2 项，已用 %3 秒").arg(checked).arg(total).arg(elapsed_seconds)
        : QStringLiteral("已检查 %1 / %2 项；正在清理索引，已用 %3 秒").arg(checked).arg(total).arg(elapsed_seconds));
}

void ExclusionSettingsDialog::finishSaved() {
    QDialog::accept();
}

} // namespace everything_lite
