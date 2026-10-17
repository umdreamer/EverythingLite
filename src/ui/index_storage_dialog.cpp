#include "ui/index_storage_dialog.h"
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>
#include <QLocale>
#include <memory>
namespace everything_lite {
namespace {
QString bytes(std::uint64_t value) {
    return QLocale().formattedDataSize(static_cast<qint64>(value))
        + QStringLiteral(" (%1 字节)").arg(value);
}
}
QString indexStorageSummary(const IndexStorage& v) {
    return QStringLiteral("主库：%1\n空闲页：%2（已包含在主库内）\nWAL：%3\nSHM：%4\n备份：%5（%6 份）\n总空间：%7")
        .arg(bytes(v.main_bytes),bytes(v.freelist_bytes),bytes(v.wal_bytes),bytes(v.shm_bytes),bytes(v.backup_bytes))
        .arg(v.backup_count).arg(bytes(v.total_bytes));
}
IndexStorageDialog::IndexStorageDialog(QString database_path,QWidget* parent)
    :QDialog(parent),database_path_(std::move(database_path)) {
    setObjectName(QStringLiteral("indexStorageDialog"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("索引存储")); resize(620,500);
    auto* layout=new QVBoxLayout(this);
    auto* explanation=new QLabel(QStringLiteral("优化格式会保留恢复备份，旧版本程序将不能写入新格式。仅整理空间保留当前格式。备份可能使总占用增加；不会删除真实文件。"),this);
    explanation->setWordWrap(true); layout->addWidget(explanation);
    report_=new QLabel(QStringLiteral("正在读取存储统计…"),this); report_->setObjectName(QStringLiteral("storageReport"));
    report_->setTextFormat(Qt::PlainText); report_->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(report_);
    outcome_=new QLabel(this);outcome_->setObjectName(QStringLiteral("storageOutcome"));outcome_->setTextFormat(Qt::PlainText);outcome_->setWordWrap(true);layout->addWidget(outcome_);
    progress_=new QProgressBar(this);progress_->setRange(0,0);progress_->hide();layout->addWidget(progress_);
    auto* backup_label=new QLabel(QStringLiteral("应用备份（精确命名的独立普通文件；列出不代表已验证可恢复）"),this);backup_label->setWordWrap(true);layout->addWidget(backup_label);
    backups_=new QListWidget(this);backups_->setObjectName(QStringLiteral("storageBackups"));backups_->setSelectionMode(QAbstractItemView::ExtendedSelection);layout->addWidget(backups_);
    auto* actions=new QDialogButtonBox(this);
    optimize_=actions->addButton(QStringLiteral("优化存储格式…"),QDialogButtonBox::ActionRole);optimize_->setObjectName(QStringLiteral("storageOptimize"));
    compact_=actions->addButton(QStringLiteral("仅整理空间…"),QDialogButtonBox::ActionRole);compact_->setObjectName(QStringLiteral("storageCompact"));
    cleanup_=actions->addButton(QStringLiteral("删除所选备份…"),QDialogButtonBox::ActionRole);cleanup_->setObjectName(QStringLiteral("storageCleanup"));
    refresh_=actions->addButton(QStringLiteral("刷新统计"),QDialogButtonBox::ActionRole);
    cancel_=actions->addButton(QStringLiteral("取消操作"),QDialogButtonBox::ActionRole);cancel_->setEnabled(false);
    auto* close=actions->addButton(QDialogButtonBox::Close);layout->addWidget(actions);
    connect(optimize_,&QPushButton::clicked,this,[this]{requestMaintenance(true);});
    connect(compact_,&QPushButton::clicked,this,[this]{requestMaintenance(false);});
    connect(refresh_,&QPushButton::clicked,this,[this]{refreshReport();});
    connect(cleanup_,&QPushButton::clicked,this,&IndexStorageDialog::requestCleanup);
    connect(cancel_,&QPushButton::clicked,this,[this]{if(cancellationRequested)cancellationRequested();cancel_->setEnabled(false);outcome_->setText(QStringLiteral("正在取消；已提交的格式迁移和已完成备份会保留。"));});
    connect(close,&QPushButton::clicked,this,&IndexStorageDialog::close);
    refreshReport();
}
IndexStorageDialog::~IndexStorageDialog() {
    if(report_thread_) {report_thread_->wait();delete report_thread_;report_thread_=nullptr;}
}
void IndexStorageDialog::refreshReport(bool update_summary) {
    if(report_thread_ || operation_running_)return;
    refresh_->setEnabled(false);optimize_->setEnabled(false);compact_->setEnabled(false);cleanup_->setEnabled(false);
    const auto generation=report_generation_;
    struct Result {IndexStorage storage;std::vector<IndexStorageBackup> backups;QString error;};
    auto result=std::make_shared<Result>();const auto path=database_path_.toUtf8().toStdString();
    report_thread_=QThread::create([result,path]{try{result->storage=readIndexStorage(path);result->backups=listIndexStorageBackups(path);}catch(const std::exception& e){result->error=QString::fromUtf8(e.what());}});
    connect(report_thread_,&QThread::finished,this,[this,result,generation,update_summary]{
        report_thread_->deleteLater();report_thread_=nullptr;refresh_->setEnabled(!operation_running_);optimize_->setEnabled(!operation_running_);compact_->setEnabled(!operation_running_);cleanup_->setEnabled(!operation_running_);
        if(generation!=report_generation_)return;
        backups_->clear();
        for(const auto& backup:result->backups){auto* item=new QListWidgetItem(QFileInfo(QString::fromUtf8(backup.path.c_str())).fileName()+QStringLiteral(" — ")+bytes(backup.bytes),backups_);item->setData(Qt::UserRole,QString::fromUtf8(backup.path.c_str()));}
        if(update_summary)report_->setText(result->error.isEmpty()?indexStorageSummary(result->storage):QStringLiteral("存储统计失败：%1").arg(result->error));
    });report_thread_->start();
}
void IndexStorageDialog::requestMaintenance(bool optimize) {
    if(operation_running_ || !maintenanceRequested)return;
    const auto explanation=optimize
        ?QStringLiteral("将先创建并验证恢复备份，再迁移存储格式并整理空间。旧版本程序不能写入新格式。迁移提交后取消整理，不会撤销格式迁移。继续？")
        :QStringLiteral("将先创建并验证恢复备份，再整理 SQLite 空闲页。备份会保留，总占用可能增加。继续？");
    if(QMessageBox::question(this,QStringLiteral("确认索引维护"),explanation)!=QMessageBox::Yes)return;
    maintenanceRequested(optimize);
}
void IndexStorageDialog::requestCleanup() {
    if(operation_running_ || report_thread_ || !cleanupRequested)return;
    std::vector<std::string> selected;QStringList names;
    for(auto* item:backups_->selectedItems()){const auto path=item->data(Qt::UserRole).toString();selected.push_back(path.toUtf8().toStdString());names.push_back(QFileInfo(path).fileName());}
    if(selected.empty()){outcome_->setText(QStringLiteral("请选择要删除的备份文件。"));return;}
    const auto message=QStringLiteral("仅删除以下 %1 份备份。删除前会验证当前索引及至少一份未选中的独立恢复备份；不会删除最后一份已验证恢复备份。已完成的删除不能撤销。\n\n%2\n\n继续？").arg(selected.size()).arg(names.join(QLatin1Char('\n')));
    if(QMessageBox::question(this,QStringLiteral("确认删除所选备份"),message)!=QMessageBox::Yes)return;
    cleanupRequested(selected);
}
void IndexStorageDialog::beginOperation() {
    setWindowTitle(QStringLiteral("索引存储"));
    ++report_generation_;operation_running_=true;backups_->setEnabled(false);cleanup_->setEnabled(false);optimize_->setEnabled(false);compact_->setEnabled(false);refresh_->setEnabled(false);cancel_->setEnabled(true);progress_->show();
    outcome_->setText(QStringLiteral("等待当前扫描或搜索结束… 文件变化和最新查询已保留。"));
}
void IndexStorageDialog::setPhase(IndexStoragePhase phase) {
    if(!operation_running_)return;
    QString text;
    switch(phase){case IndexStoragePhase::Backup:text=QStringLiteral("正在创建恢复备份…");break;
    case IndexStoragePhase::BackupVerified:text=QStringLiteral("备份已验证；正在建立来源凭证…");break;
    case IndexStoragePhase::Migrate:text=QStringLiteral("正在迁移存储格式…");break;
    case IndexStoragePhase::Vacuum:text=QStringLiteral("正在整理空间…");break;
    case IndexStoragePhase::Verify:text=QStringLiteral("正在验证数据库与备份…");break;}
    outcome_->setText(text);
}
void IndexStorageDialog::finishOperation(const QString& message,const IndexStorage* before,const IndexStorage* after) {
    operation_running_=false;backups_->setEnabled(true);progress_->hide();cancel_->setEnabled(false);optimize_->setEnabled(!report_thread_);compact_->setEnabled(!report_thread_);cleanup_->setEnabled(!report_thread_);refresh_->setEnabled(!report_thread_);
    outcome_->setText(message);
    if(before && after)report_->setText(QStringLiteral("操作前\n%1\n\n操作后\n%2").arg(indexStorageSummary(*before),indexStorageSummary(*after)));
    if(close_when_finished_)close();
    else refreshReport(false);
}
void IndexStorageDialog::reject(){if(operation_running_)close();else QDialog::reject();}
void IndexStorageDialog::closeEvent(QCloseEvent* event) {
    if(operation_running_){close_when_finished_=true;if(cancellationRequested)cancellationRequested();outcome_->setText(QStringLiteral("正在取消操作，完成后关闭窗口…"));event->ignore();return;}
    QDialog::closeEvent(event);
}
} // namespace everything_lite
