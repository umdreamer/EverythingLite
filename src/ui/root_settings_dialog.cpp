#include "ui/root_settings_dialog.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QComboBox>
#include <QDir>
#include <QHeaderView>
#include <QMessageBox>
#include <QSpinBox>
#include <QTableWidget>
#include "core/path_utils.h"

namespace everything_lite {

RootSettingsDialog::RootSettingsDialog(const QStringList& roots, QWidget* parent,int periodic_minutes,
                                       const std::vector<UpdateRule>& rules) : QDialog(parent) {
    setWindowTitle(QStringLiteral("索引目录"));
    resize(700, 620);

    auto* outer = new QVBoxLayout(this);
    root_section_=new QWidget(this);auto* layout=new QVBoxLayout(root_section_);layout->setContentsMargins(0,0,0,0);
    outer->addWidget(root_section_,1);
    auto* hint = new QLabel(QStringLiteral("只索引需要被 Everything Lite 搜索的目录。保存后自动同步来源并索引新增目录；移除目录会保留历史缓存。"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    list_ = new QListWidget(this);list_->setObjectName(QStringLiteral("indexRootsList"));
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

    update_section_=new QWidget(this);layout=new QVBoxLayout(update_section_);layout->setContentsMargins(0,0,0,0);
    outer->addWidget(update_section_,1);
    auto* intervalRow=new QHBoxLayout();
    intervalRow->addWidget(new QLabel(QStringLiteral("隐藏及程序数据目录更新间隔："),this));
    periodic_minutes_=new QSpinBox(this);periodic_minutes_->setObjectName(QStringLiteral("periodicMinutes"));
    periodic_minutes_->setRange(1,10080);periodic_minutes_->setSuffix(QStringLiteral(" 分钟"));periodic_minutes_->setValue(periodic_minutes);
    intervalRow->addWidget(periodic_minutes_);intervalRow->addStretch();layout->addLayout(intervalRow);
    layout->addWidget(new QLabel(QStringLiteral("目录更新覆盖规则（最具体的路径优先）："),this));
    rules_=new QTableWidget(0,2,this);rules_->setObjectName(QStringLiteral("updateRules"));
    rules_->setHorizontalHeaderLabels({QStringLiteral("目录路径（可直接输入）"),QStringLiteral("更新方式")});
    rules_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);
    rules_->horizontalHeader()->setSectionResizeMode(1,QHeaderView::ResizeToContents);
    rules_->setSelectionBehavior(QAbstractItemView::SelectRows);layout->addWidget(rules_,1);
    for(const auto& rule:rules)appendRule(QString::fromUtf8(rule.path.c_str()),rule.cadence);
    auto* ruleButtons=new QHBoxLayout();
    auto* addRule=new QPushButton(QStringLiteral("添加规则"),this);
    auto* chooseRule=new QPushButton(QStringLiteral("选择目录…"),this);
    auto* removeRule=new QPushButton(QStringLiteral("移除规则"),this);
    ruleButtons->addWidget(addRule);ruleButtons->addWidget(chooseRule);ruleButtons->addWidget(removeRule);ruleButtons->addStretch();layout->addLayout(ruleButtons);
    connect(addRule,&QPushButton::clicked,this,[this]{appendRule({},UpdateCadence::Realtime);});
    connect(chooseRule,&QPushButton::clicked,this,[this]{const auto path=QFileDialog::getExistingDirectory(this,QStringLiteral("选择规则目录"));if(!path.isEmpty())appendRule(path,UpdateCadence::Realtime);});
    connect(removeRule,&QPushButton::clicked,this,[this]{const auto rows=rules_->selectionModel()->selectedRows();for(auto it=rows.crbegin();it!=rows.crend();++it)rules_->removeRow(it->row());});

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    outer->addWidget(buttons);

    connect(addButton, &QPushButton::clicked, this, &RootSettingsDialog::addRoot);
    connect(removeButton, &QPushButton::clicked, this, &RootSettingsDialog::removeSelected);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        try { UpdatePolicy(pathToUtf8(utf8ToPath(QDir::homePath().toUtf8().toStdString())),updateRules(),{}); if(saveRequested)saveRequested();else accept(); }
        catch(const std::exception&) { QMessageBox::warning(this,QStringLiteral("更新规则无效"),QStringLiteral("请为每条规则填写不同的绝对目录路径。")); }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void RootSettingsDialog::reload(const QStringList& roots,int interval,const std::vector<UpdateRule>& rules) {
    list_->clear();list_->addItems(roots);periodic_minutes_->setValue(interval);rules_->setRowCount(0);
    for(const auto& rule:rules)appendRule(QString::fromUtf8(rule.path.c_str()),rule.cadence);
    setProperty("rootSettingsBaseline",roots);
}

void RootSettingsDialog::setSectionsVisible(bool roots,bool updates) {
    root_section_->setVisible(roots);update_section_->setVisible(updates);
}

void RootSettingsDialog::appendRule(const QString& path,UpdateCadence cadence) {
    const int row=rules_->rowCount();rules_->insertRow(row);rules_->setItem(row,0,new QTableWidgetItem(path));
    auto* mode=new QComboBox(rules_);mode->addItem(QStringLiteral("实时"),0);mode->addItem(QStringLiteral("周期"),1);
    mode->setCurrentIndex(cadence==UpdateCadence::Periodic?1:0);rules_->setCellWidget(row,1,mode);
}
int RootSettingsDialog::periodicMinutes() const {return periodic_minutes_->value();}
std::vector<UpdateRule> RootSettingsDialog::updateRules() const {
    std::vector<UpdateRule> result;
    for(int row=0;row<rules_->rowCount();++row) {
        const auto* item=rules_->item(row,0);const auto path=item?QDir::cleanPath(item->text()):QString{};
        const auto* mode=qobject_cast<QComboBox*>(rules_->cellWidget(row,1));
        result.push_back({path.toUtf8().toStdString(),mode && mode->currentIndex()==1?UpdateCadence::Periodic:UpdateCadence::Realtime});
    }
    return result;
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
