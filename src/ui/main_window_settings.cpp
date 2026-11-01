#include "ui/main_window.h"
#include "ui/settings_dialog.h"
#include "ui/root_settings_dialog.h"
#include "ui/exclusion_settings_dialog.h"
#include "ui/index_storage_dialog.h"
#include "ui/index_update_journal.h"
#include "ui/open_history_store.h"
#include "ui/app_settings.h"
#include "core/database.h"
#include "core/path_utils.h"
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
namespace everything_lite {
namespace {
std::string utf8(const QString& text) {const auto bytes=text.toUtf8();return {bytes.constData(),static_cast<std::size_t>(bytes.size())};}
QString text(const std::string& value) {return QString::fromUtf8(value.c_str());}
QString sourceType(SourceType type) {
    switch(type) {
    case SourceType::NAS:return QStringLiteral("NAS");
    case SourceType::Cloud:return QStringLiteral("网盘");
    case SourceType::USB:return QStringLiteral("U盘");
    case SourceType::External:return QStringLiteral("移动硬盘");
    default:return QStringLiteral("本地磁盘");
    }
}
QPlainTextEdit* readOnly(QWidget* parent,const QString& name,const QString& content) {
    auto* edit=new QPlainTextEdit(parent);edit->setObjectName(name);edit->setReadOnly(true);edit->setPlainText(content);return edit;
}
}
QWidget* MainWindow::makeRootSettingsPage(QWidget* parent,bool updates) {
    auto* page=new QWidget(parent);auto* layout=new QVBoxLayout(page);
    auto* editor=new RootSettingsDialog(roots_,page,periodic_minutes_,update_rules_);
    editor->setObjectName(updates?QStringLiteral("updateSettingsEditor"):QStringLiteral("rootSettingsEditor"));
    editor->setSectionsVisible(!updates,updates);editor->setProperty("rootSettingsBaseline",roots_);settings_dialog_->embedDialog(editor,page);
    if(auto* buttons=editor->findChild<QDialogButtonBox*>())if(auto* save=buttons->button(QDialogButtonBox::Ok))save->setText(QStringLiteral("保存"));
    layout->addWidget(editor);
    auto* outcome=new QLabel(page);outcome->setObjectName(updates?QStringLiteral("updateSettingsOutcome"):QStringLiteral("rootSettingsOutcome"));outcome->setWordWrap(true);layout->addWidget(outcome);
    editor->saveRequested=[this,editor,updates,outcome] {
        if(storage_maintenance_){outcome->setText(QStringLiteral("索引维护进行中；完成后可保存。"));return;}
        if(!updates && editor->property("rootSettingsBaseline").toStringList()!=roots_) {
            outcome->setText(QStringLiteral("索引目录已在其他操作中变化；请关闭并重新打开设置后再保存。"));return;
        }
        applyRootConfiguration(updates?roots_:editor->roots(),updates?editor->periodicMinutes():periodic_minutes_,updates?editor->updateRules():update_rules_);
        editor->setProperty("rootSettingsBaseline",roots_);
        outcome->setText(QStringLiteral("已保存；正在同步来源清单。新增目录的扫描进度见程序状态。"));
    };
    if(updates) {
        auto* refresh=new QPushButton(QStringLiteral("刷新待更新范围"),page);layout->addWidget(refresh);
        auto* pending=readOnly(page,QStringLiteral("periodicPendingText"),deferredStatusLines().join(QLatin1Char('\n')));layout->addWidget(pending,1);
        connect(refresh,&QPushButton::clicked,page,[this,pending]{pending->setPlainText(deferredStatusLines().join(QLatin1Char('\n')));});
        connect(this,&MainWindow::catalogSynchronized,page,[outcome](const QString& error){outcome->setText(error.isEmpty()?QStringLiteral("更新策略已保存，来源清单已同步。"):QStringLiteral("策略已保存，来源清单尚未同步：")+error);});
        return page;
    }
    layout->addWidget(new QLabel(QStringLiteral("活动目录、历史来源与离线缓存"),page));
    auto* tree=new QTreeWidget(page);tree->setObjectName(QStringLiteral("sourceSettingsTree"));
    tree->setHeaderLabels({QStringLiteral("目录 / 来源"),QStringLiteral("类型"),QStringLiteral("状态")});
    tree->header()->setSectionResizeMode(0,QHeaderView::Stretch);tree->setMinimumHeight(180);layout->addWidget(tree,1);
    auto refreshSources=[this,tree] {
        tree->clear();
        try {
            Database db(utf8(db_path_));const auto active=db.configuredRoots();const auto sources=db.sources();
            QHash<QString,QTreeWidgetItem*> configured;
            for(const auto& root:roots_) {
                const bool registered=std::find(active.begin(),active.end(),utf8(root))!=active.end();
                auto* item=new QTreeWidgetItem(tree,{root,QStringLiteral("本地磁盘"),registered?QStringLiteral("活动索引目录"):QStringLiteral("等待来源同步")});item->setToolTip(0,root);configured.insert(root,item);
            }
            QTreeWidgetItem* retained=nullptr;
            for(const auto& source:sources) {
                const auto root=text(source.root);
                if(configured.contains(root)) {
                    auto* item=configured.value(root);item->setText(1,sourceType(source.type));
                    if(source.status==SourceStatus::Offline)item->setText(2,QStringLiteral("活动目录；离线缓存"));
                    continue;
                }
                QString owner;
                for(const auto& configuredRoot:roots_)if(pathIsWithin(source.root,utf8(configuredRoot)) && configuredRoot.size()>owner.size())owner=configuredRoot;
                auto* parentItem=configured.value(owner,nullptr);
                if(!parentItem) {
                    if(!retained)retained=new QTreeWidgetItem(tree,{QStringLiteral("未启用的历史来源缓存"),{},QStringLiteral("保留，不自动加入目录")});
                    parentItem=retained;
                }
                auto* cached=new QTreeWidgetItem(parentItem,{root,sourceType(source.type),source.status==SourceStatus::Offline?QStringLiteral("离线缓存"):owner.isEmpty()?QStringLiteral("未启用"):QStringLiteral("归属上级索引目录")});cached->setToolTip(0,root);
            }
        }catch(const std::exception& error) {new QTreeWidgetItem(tree,{QStringLiteral("来源清单尚未读取"),{},QString::fromUtf8(error.what())});}
    };
    auto* refresh=new QPushButton(QStringLiteral("刷新来源清单"),page);layout->addWidget(refresh);
    connect(refresh,&QPushButton::clicked,page,refreshSources);
    connect(this,&MainWindow::catalogSynchronized,page,[outcome,refreshSources](const QString& error){outcome->setText(error.isEmpty()?QStringLiteral("来源清单已同步。新增目录扫描在后台执行；详情见程序状态。"):QStringLiteral("来源清单尚未同步：")+error);refreshSources();});
    refreshSources();return page;
}
QWidget* MainWindow::makeStateSettingsPage(QWidget* parent) {
    auto* page=new QWidget(parent);auto* layout=new QVBoxLayout(page);
    auto* task=new QLabel(page);task->setObjectName(QStringLiteral("settingsTaskStatus"));task->setWordWrap(true);layout->addWidget(task);
    auto updateTask=[this,task] {if(task->isVisible()) {const auto value=QStringLiteral("%1：%2").arg(storage_maintenance_?QStringLiteral("存储维护"):worker_thread_?QStringLiteral("后台任务"):QStringLiteral("当前状态"),status_label_->text());if(task->text()!=value)task->setText(value);}};
    auto* timer=new QTimer(page);timer->setInterval(1000);connect(timer,&QTimer::timeout,page,updateTask);timer->start();
    auto* refresh=new QPushButton(QStringLiteral("刷新状态"),page);layout->addWidget(refresh);
    auto* tabs=new QTabWidget(page);tabs->setObjectName(QStringLiteral("stateTabs"));layout->addWidget(tabs,1);
    auto* status=readOnly(tabs,QStringLiteral("indexStatusText"),indexStatusText());tabs->addTab(status,QStringLiteral("索引与程序状态"));
    auto* overview=readOnly(tabs,QStringLiteral("settingsOverviewText"),settingsOverviewText());tabs->addTab(overview,QStringLiteral("配置详情与数据位置"));
    auto* diagnosis=readOnly(tabs,QStringLiteral("searchDiagnosticsText"),searchDiagnosticsText());tabs->addTab(diagnosis,QStringLiteral("最近搜索诊断"));
    connect(refresh,&QPushButton::clicked,page,[this,status,overview,diagnosis]{status->setPlainText(indexStatusText());overview->setPlainText(settingsOverviewText());diagnosis->setPlainText(searchDiagnosticsText());});
    auto* rebuild=new QPushButton(QStringLiteral("重建索引…"),page);layout->addWidget(rebuild);
    connect(rebuild,&QPushButton::clicked,this,&MainWindow::rebuildIndex);return page;
}
void MainWindow::showSettingsPage(const QString& id) {
    const bool was_visible=settings_dialog_ && settings_dialog_->isVisible();
    if(!settings_dialog_) {
        auto* dialog=new SettingsDialog(this);settings_dialog_=dialog;
        connect(dialog,&SettingsDialog::pageActivated,this,[this](const QString& page,bool newly_created) {
            if(newly_created)return;
            if(page==QStringLiteral("logs"))refreshIndexUpdateLogDialog(true);
            if(page==QStringLiteral("state")) {
                if(auto* edit=settings_dialog_->findChild<QPlainTextEdit*>("indexStatusText"))edit->setPlainText(indexStatusText());
                if(auto* edit=settings_dialog_->findChild<QPlainTextEdit*>("settingsOverviewText"))edit->setPlainText(settingsOverviewText());
                if(auto* edit=settings_dialog_->findChild<QPlainTextEdit*>("searchDiagnosticsText"))edit->setPlainText(searchDiagnosticsText());
            }
        });
        dialog->addPage(QStringLiteral("roots"),QStringLiteral("索引目录与来源"),[this](QWidget* parent){return makeRootSettingsPage(parent,false);});
        dialog->addPage(QStringLiteral("updates"),QStringLiteral("更新策略"),[this](QWidget* parent){return makeRootSettingsPage(parent,true);});
        dialog->addPage(QStringLiteral("excludes"),QStringLiteral("排除规则"),[this,dialog](QWidget* parent){
            auto* page=new QWidget(parent);auto* layout=new QVBoxLayout(page);
            auto* hint=new QLabel(QStringLiteral("排除规则尚未载入。索引任务完成后可点击加载；保存进度在本页显示。"),page);hint->setWordWrap(true);layout->addWidget(hint);
            auto* load=new QPushButton(QStringLiteral("加载排除规则"),page);layout->addWidget(load);
            auto loadEditor=[this,dialog,page,layout,hint,load] {
                if(worker_thread_ || storage_maintenance_)return;
                showExclusionSettings();if(!exclusion_editor_)return;
                auto* editor=exclusion_editor_.data();editor->setEmbedded(true);dialog->embedDialog(editor,page);layout->addWidget(editor,1);editor->show();hint->hide();load->hide();
            };
            connect(load,&QPushButton::clicked,page,loadEditor);loadEditor();return page;
        });
        dialog->addPage(QStringLiteral("storage"),QStringLiteral("数据与存储"),[this,dialog](QWidget* parent){
            auto* page=new QWidget(parent);auto* layout=new QVBoxLayout(page);
            auto* paths=readOnly(page,QStringLiteral("dataLocationText"),QStringLiteral("索引数据库：%1\n偏好设置：%2\n打开记录：%3\n索引日志：%4\n周期队列：%5")
                .arg(db_path_,makeAppSettings().fileName(),OpenHistoryStore::defaultPath(),IndexUpdateJournal::defaultDirectory(),db_path_+QStringLiteral(".deferred.sqlite")));
            paths->setMaximumHeight(160);layout->addWidget(paths);
            showIndexStorage();if(storage_dialog_) {dialog->embedDialog(storage_dialog_,page);layout->addWidget(storage_dialog_,1);storage_dialog_->show();}
            return page;
        });
        dialog->addPage(QStringLiteral("state"),QStringLiteral("程序状态"),[this](QWidget* parent){return makeStateSettingsPage(parent);});
        dialog->addPage(QStringLiteral("logs"),QStringLiteral("日志与保留设置"),[this,dialog](QWidget* parent){
            auto* page=new QWidget(parent);auto* layout=new QVBoxLayout(page);showIndexUpdateLog();
            if(index_update_log_dialog_) {dialog->embedDialog(index_update_log_dialog_,page);layout->addWidget(index_update_log_dialog_,1);index_update_log_dialog_->show();}
            return page;
        });
    }
    settings_dialog_->selectPage(id);
    if(!was_visible) {
        for(auto* editor:settings_dialog_->findChildren<RootSettingsDialog*>())editor->reload(roots_,periodic_minutes_,update_rules_);
        if(exclusion_editor_ && !exclusion_editor_->isSaving()) {
            try {exclusion_editor_->reloadRules(Database(utf8(db_path_)).exclusionPolicy().rules());}
            catch(const std::exception& error){exclusion_editor_->showError(QString::fromUtf8(error.what()));}
        }
    }
    settings_dialog_->show();settings_dialog_->raise();settings_dialog_->activateWindow();
}
}
