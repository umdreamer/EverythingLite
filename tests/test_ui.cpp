#include "ui/main_window.h"
#include "ui/app_settings.h"
#include "ui/search_result_model.h"
#include "ui/result_presentation.h"
#include "ui/search_worker.h"
#include "ui/index_storage_dialog.h"
#include "ui/exclusion_settings_dialog.h"
#include "core/index_manager.h"
#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDesktopServices>
#include <QUrl>
#include <QStatusBar>
#include <QClipboard>
#include <QHeaderView>
#include <QStandardItemModel>
#include <QTextDocument>
#include <QAbstractTextDocumentLayout>
#include <algorithm>
#include <atomic>
#include <optional>
#include <sqlite3.h>
#include <QFile>
#include <QFileDialog>
#include <QDialog>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QProgressDialog>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QProcess>
#include <QSemaphore>
#include <QSettings>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QElapsedTimer>
#include <QtTest>
namespace everything_lite {
class UiUrlSink : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void accept(const QUrl& url) { urls << url; }
};
template<class Service>
auto cancellableSearch(Service& service, const SearchOptions& options, const std::atomic_bool* token, int)
    -> decltype(service.search("item", 1000, 0, options, token)) {
    return service.search("item", 1000, 0, options, token);
}
template<class Service>
auto cancellableSearch(Service& service, const SearchOptions& options, const std::atomic_bool*, long)
    -> decltype(service.search("item", 1000, 0, options)) {
    return service.search("item", 1000, 0, options);
}
class TooltipProbeDocument final : public QTextDocument {
public:
    QList<QUrl> resource_requests;
protected:
    QVariant loadResource(int, const QUrl& url) override {
        resource_requests << url;
        return {};
    }
};
struct WorkerGate {
    std::shared_ptr<QSemaphore> started=std::make_shared<QSemaphore>();
    std::shared_ptr<QSemaphore> release=std::make_shared<QSemaphore>();
    void queue(SearchWorker* worker) {
        QMetaObject::invokeMethod(worker,[s=started,r=release] {s->release();r->acquire();},Qt::QueuedConnection);
    }
    bool block(SearchWorker* worker) {
        queue(worker);
        return started->tryAcquire(1,3000);
    }
    ~WorkerGate() {release->release();}
};
class UiTest : public QObject {
    Q_OBJECT
  public:
    QString fixture_root;
  private slots:
    void defaultSearchControlsAndStatusAreVisible() {
        auto settings = makeAppSettings();
        settings.setValue("view/statusBar", false);
        settings.setValue("view/filterBar", false);
        settings.setValue("view/scopeBar", false);
        settings.remove("view/restoredControls0415Fixbug");
        settings.remove("view/restoredControls0415Fixbug2");
        MainWindow window;
        window.show();
        QVERIFY(window.statusBar()->isVisible());
        QVERIFY(window.scope_combo_->isVisible());
        QVERIFY(window.scope_label_->isVisible());
        QVERIFY(window.match_path_check_->isVisible());
        QVERIFY(window.include_hidden_check_->isVisible());
        window.search_edit_->setText("item");
        window.runSearch();
        QTRY_VERIFY(!window.search_in_flight_);
        QVERIFY(window.search_metrics_label_->text().contains(QStringLiteral("ms")));
        QVERIFY(window.search_metrics_label_->text().contains(QStringLiteral("结果")));
    }
    void previousRestoreMarkerDoesNotKeepControlsHidden() {
        auto settings = makeAppSettings();
        settings.setValue("view/statusBar", false);
        settings.setValue("view/filterBar", false);
        settings.setValue("view/scopeBar", false);
        settings.setValue("view/restoredControls0415Fixbug", true);
        settings.remove("view/restoredControls0415Fixbug2");
        MainWindow window;
        window.show();
        QVERIFY(window.statusBar()->isVisible());
        QVERIFY(window.scope_combo_->isVisible());
        QVERIFY(window.scope_label_->isVisible());
    }
    void settingsOverviewShowsSavedConfigurationAndStoragePaths() {
        MainWindow window;
        QString report;
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            if (auto* text = dialog->findChild<QPlainTextEdit*>("settingsOverviewText"))
                report = text->toPlainText();
            dialog->accept();
        });
        window.showSettingsOverview();
        QVERIFY(report.contains(QStringLiteral("index/roots")));
        QVERIFY(report.contains(QStringLiteral("排除规则")));
        QVERIFY(report.contains(QStringLiteral("数据库")));
        QVERIFY(report.contains(QStringLiteral("偏好设置文件")));
    }
    void init() {
        auto settings = makeAppSettings();
        settings.setValue("index/syncRetryBaseMs", 1500);
        settings.setValue("index/roots", QStringList{fixture_root});
        settings.setValue("search/itemScope", 0);
        settings.setValue("search/matchPath", false);
        settings.setValue("search/includeHidden", true);
        settings.setValue("search/folderScope", QString{});
        settings.setValue("table/sortOrder", static_cast<int>(Qt::AscendingOrder));
        settings.setValue("view/statusBar", true);
        settings.setValue("view/scopeBar", true);
        settings.setValue("view/filterBar", true);
    }
    void storageMenuOpensAsyncReport() {
        MainWindow window; window.watcher_->stop();
        QAction* action=nullptr;
        for (auto* candidate:window.findChildren<QAction*>())
            if(candidate->text()==QStringLiteral("索引存储…")) action=candidate;
        QVERIFY2(action,"Tools must expose explicit index storage maintenance");
        action->trigger();
        auto* dialog=window.findChild<QDialog*>(QStringLiteral("indexStorageDialog"));
        QVERIFY(dialog);
        auto* report=dialog->findChild<QLabel*>(QStringLiteral("storageReport"));
        QVERIFY(report);
        QTRY_VERIFY_WITH_TIMEOUT(report->text().contains(QStringLiteral("主库")),3000);
        QVERIFY(report->text().contains(QStringLiteral("备份")));
        QVERIFY(report->text().contains(QStringLiteral("总空间")));
        dialog->close();
    }
    void storageWaitsForSearchPreservesEventsAndLatestQuery() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto saved=qgetenv("EVERYTHING_LITE_DB");
        qputenv("EVERYTHING_LITE_DB",(dir.path()+"/index.db").toUtf8());
        MainWindow window; qputenv("EVERYTHING_LITE_DB",saved);
        window.watcher_->stop(); window.roots_={fixture_root};
        window.rebuildIndex();QTRY_VERIFY_WITH_TIMEOUT(!window.worker_thread_,10000);
        WorkerGate gate;QVERIFY(gate.block(window.search_worker_));
        window.search_edit_->setText("alpha");window.runSearch();
        QVERIFY(window.search_in_flight_);
        window.showIndexStorage();window.requestStorageMaintenance(false);
        QVERIFY(window.storage_maintenance_);QVERIFY(!window.worker_thread_);
        window.scheduleFileEvent(FileEvent{fixture_root.toStdString(),true,{}});
        window.processPendingEvents();
        QVERIFY(window.pending_rescan_roots_.contains(fixture_root));
        QVERIFY(window.normal_events_due_);
        window.search_edit_->setText("beta");window.runSearch();
        QVERIFY(window.search_pending_);QVERIFY(!window.worker_thread_);
        gate.release->release();
        QTRY_VERIFY_WITH_TIMEOUT(!window.storage_maintenance_,10000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.worker_thread_,10000);
        QTRY_COMPARE_WITH_TIMEOUT(window.model_->rowCount(),1,10000);
        QCOMPARE(window.model_->nameAt(0),QStringLiteral("beta.txt"));
        QVERIFY(window.pending_rescan_roots_.isEmpty());
        QVERIFY(window.storage_dialog_->findChild<QLabel*>(QStringLiteral("storageOutcome"))->text().contains(QStringLiteral("整理完成")));
    }
    void storageWaitsForScanAndCloseCancelsPendingMaintenance() {
        MainWindow window;window.watcher_->stop();
        auto release=std::make_shared<QSemaphore>();auto started=std::make_shared<QSemaphore>();
        window.startWorker([started,release]{started->release();release->acquire();},QStringLiteral("synthetic job"));
        QVERIFY(started->tryAcquire(1,3000));
        window.showIndexStorage();window.requestStorageMaintenance(true);
        QVERIFY(window.storage_maintenance_);QVERIFY(window.worker_thread_);
        auto token=window.storage_cancellation_;
        window.close();QVERIFY(token->load());release->release();
        QTRY_VERIFY_WITH_TIMEOUT(!window.worker_thread_,5000);
    }
    void storageReadOnlyReportDoesNotCreateMissingDatabase() {
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto path=dir.path()+"/absent.db";
        auto* dialog=new IndexStorageDialog(path);dialog->show();
        auto* report=dialog->findChild<QLabel*>(QStringLiteral("storageReport"));
        QTRY_VERIFY_WITH_TIMEOUT(report->text().contains(QStringLiteral("失败")),3000);
        QVERIFY(!QFile::exists(path));dialog->close();
    }
    void storageOptimizeReportsRepeatWithoutNewBackup() {
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto saved=qgetenv("EVERYTHING_LITE_DB");
        qputenv("EVERYTHING_LITE_DB",(dir.path()+"/index.db").toUtf8());
        MainWindow window;qputenv("EVERYTHING_LITE_DB",saved);window.watcher_->stop();
        window.showIndexStorage();window.requestStorageMaintenance(true);
        QTRY_VERIFY_WITH_TIMEOUT(!window.storage_maintenance_,10000);
        const auto first=readIndexStorage(window.db_path_.toStdString());QCOMPARE(first.backup_count,std::uint64_t(0));
        window.requestStorageMaintenance(true);QTRY_VERIFY_WITH_TIMEOUT(!window.storage_maintenance_,10000);
        QCOMPARE(readIndexStorage(window.db_path_.toStdString()).backup_count,first.backup_count);
        QVERIFY(window.storage_dialog_->findChild<QLabel*>(QStringLiteral("storageOutcome"))->text().contains(QStringLiteral("本次未迁移")));
        auto* outcome=window.storage_dialog_->findChild<QLabel*>(QStringLiteral("storageOutcome"));
        const auto finished=outcome->text();window.storage_dialog_->setPhase(IndexStoragePhase::Backup);
        QCOMPARE(outcome->text(),finished);
    }
    void manualRebuildRegistersNestedRootsBeforeScanning() {
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto root=dir.path()+"/parent", child=root+"/child";
        QVERIFY(QDir().mkpath(child));QFile file(child+"/nested.txt");QVERIFY(file.open(QIODevice::WriteOnly));file.write("fixture");file.close();
        const auto saved=qgetenv("EVERYTHING_LITE_DB");qputenv("EVERYTHING_LITE_DB",(dir.path()+"/index.db").toUtf8());
        MainWindow window;qputenv("EVERYTHING_LITE_DB",saved);window.watcher_->stop();window.roots_={root,child,child+"/"};
        window.rebuildIndex();QTRY_VERIFY_WITH_TIMEOUT(!window.worker_thread_,10000);
        Database db(window.db_path_.toStdString());const auto rows=db.search("nested.txt");
        QCOMPARE(rows.size(),std::size_t(1));QCOMPARE(QString::fromStdString(rows[0].file.root),child);
    }
    void storageCleanupRequiresExactSelectionAndAllowsLastBackup() {
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto saved=qgetenv("EVERYTHING_LITE_DB");
        const auto path=dir.path()+"/index.db";qputenv("EVERYTHING_LITE_DB",path.toUtf8());
        MainWindow window;qputenv("EVERYTHING_LITE_DB",saved);window.watcher_->stop();
        const auto first=path+".pre-storage-1.backup.sqlite",second=path+".pre-storage-2.backup.sqlite";
        compactIndexStorage(path.toStdString(),first.toStdString());compactIndexStorage(path.toStdString(),second.toStdString());
        window.showIndexStorage();auto* dialog=window.storage_dialog_.data();
        auto* list=dialog->findChild<QListWidget*>(QStringLiteral("storageBackups"));
        auto* cleanup=dialog->findChild<QPushButton*>(QStringLiteral("storageCleanup"));
        QVERIFY(list && cleanup);QVERIFY(!cleanup->isEnabled());
        QVERIFY(!dialog->findChild<QPushButton*>(QStringLiteral("storageOptimize"))->isEnabled());
        QTRY_COMPARE_WITH_TIMEOUT(list->count(),2,3000);QVERIFY(cleanup->isEnabled());
        cleanup->click();QVERIFY(!window.storage_maintenance_);QVERIFY(QFile::exists(first) && QFile::exists(second));
        for(int row=0;row<list->count();++row)if(list->item(row)->data(Qt::UserRole).toString()==QFileInfo(second).canonicalFilePath())list->item(row)->setSelected(true);
        QCOMPARE(list->selectedItems().size(),1);
        QTimer::singleShot(0,[]{if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))box->button(QMessageBox::Yes)->click();});
        cleanup->click();QVERIFY(window.storage_maintenance_);QTRY_VERIFY_WITH_TIMEOUT(!window.storage_maintenance_,10000);
        QVERIFY(QFile::exists(first));QVERIFY(!QFile::exists(second));
        QVERIFY(dialog->findChild<QLabel*>(QStringLiteral("storageOutcome"))->text().contains(QStringLiteral("已删除 1 份备份")));
        QTRY_COMPARE_WITH_TIMEOUT(list->count(),1,3000);
        list->item(0)->setSelected(true);
        QTimer::singleShot(0,[]{if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))box->button(QMessageBox::Yes)->click();});
        cleanup->click();QTRY_VERIFY_WITH_TIMEOUT(!window.storage_maintenance_,10000);
        QVERIFY(!QFile::exists(first));QTRY_COMPARE_WITH_TIMEOUT(list->count(),0,3000);
    }
    void storageDialogCloseCancelsAndDefersDestruction() {
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto saved=qgetenv("EVERYTHING_LITE_DB");
        qputenv("EVERYTHING_LITE_DB",(dir.path()+"/index.db").toUtf8());
        MainWindow window;qputenv("EVERYTHING_LITE_DB",saved);window.watcher_->stop();
        window.showIndexStorage();window.requestStorageMaintenance(false);
        QVERIFY(window.worker_thread_);auto cancellation=window.storage_cancellation_;
        window.storage_dialog_->close();QVERIFY(cancellation->load());
        QTRY_VERIFY_WITH_TIMEOUT(!window.worker_thread_ && !window.storage_dialog_,10000);
    }
    void storageCompactionReportsFailureWithoutBackupWhenVacuumIsBusy() {
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto saved=qgetenv("EVERYTHING_LITE_DB");
        const auto path=dir.path()+"/index.db";qputenv("EVERYTHING_LITE_DB",path.toUtf8());
        MainWindow window;qputenv("EVERYTHING_LITE_DB",saved);window.watcher_->stop();
        sqlite3* writer=nullptr;QCOMPARE(sqlite3_open(path.toUtf8().constData(),&writer),SQLITE_OK);
        QCOMPARE(sqlite3_exec(writer,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr),SQLITE_OK);
        window.showIndexStorage();window.requestStorageMaintenance(false);
        QTRY_VERIFY_WITH_TIMEOUT(!window.storage_maintenance_,10000);
        sqlite3_exec(writer,"ROLLBACK",nullptr,nullptr,nullptr);sqlite3_close(writer);
        const auto outcome=window.storage_dialog_->findChild<QLabel*>(QStringLiteral("storageOutcome"))->text();
        QVERIFY(outcome.contains(QStringLiteral("未完成")));
        QVERIFY2(outcome.contains(QStringLiteral("VACUUM 已完成：否")),qPrintable(outcome));
        QCOMPARE(readIndexStorage(path.toStdString()).backup_count,std::uint64_t(0));
    }
    void storageMaintenanceBlocksDatabaseMenus_data() {
        QTest::addColumn<bool>("edit_roots");
        QTest::newRow("status")<<false;QTest::newRow("roots")<<true;
    }
    void storageMaintenanceBlocksDatabaseMenus() {
        QFETCH(bool,edit_roots);
        MainWindow window;window.watcher_->stop();window.storage_maintenance_=true;
        bool opened=false;QTimer closer;closer.setSingleShot(true);
        connect(&closer,&QTimer::timeout,&window,[&]{if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())){opened=true;dialog->reject();}});
        closer.start(0);if(edit_roots)window.editRoots();else window.showIndexStatus();closer.stop();
        window.storage_maintenance_=false;
        QVERIFY2(!opened,"Database menus must not open synchronous dialogs during maintenance");
        QVERIFY(window.status_label_->text().contains(QStringLiteral("维护")));
    }
    void indexStatusUsesCachedCountsWithoutOpeningDatabase() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path=dir.filePath("status.db");
        const auto saved=qgetenv("EVERYTHING_LITE_DB"); qputenv("EVERYTHING_LITE_DB",path.toUtf8());
        MainWindow window; qputenv("EVERYTHING_LITE_DB",saved); window.watcher_->stop();
        QVERIFY(QFile::remove(path));
        QString text;
        QTimer::singleShot(0,[&]{
            if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                text=box->text();box->accept();
            }
        });
        window.showIndexStatus();
        QVERIFY(!QFile::exists(path));
        QVERIFY(text.contains(QStringLiteral("最近统计")));
    }
    void paginationDeduplicatesStructuredIdentity() {
        SearchResultModel model;
        auto row=[](std::int64_t source,const std::string& relative,const std::string& name) {
            SearchResult result; result.file.source_id=source; result.file.relative_path=relative;
            result.file.name=name; result.file.path="/synthetic/source"+std::to_string(source)+"/"+relative;
            return result;
        };
        const auto first=row(1,"same.txt","first"), second=row(2,"same.txt","second");
        const auto third=row(1,"other.txt","third");
        model.setResults({first,first,second});
        QCOMPARE(model.rowCount(),2);
        model.sort(0,Qt::DescendingOrder);
        model.appendResults({third,second,first,third});
        QCOMPARE(model.rowCount(),3);
        QCOMPARE(model.nameAt(0),QStringLiteral("third"));
        // Delimiters in relative paths must remain part of the structured key.
        model.appendResults({row(12,"3:same.txt","delimiter-a"),row(123,"same.txt","delimiter-b")});
        QCOMPARE(model.rowCount(),5);
        // Unknown/manual rows have no valid identity and remain independent.
        model.appendResults({row(0,"","unknown-a"),row(0,"","unknown-b"),
                             row(0,"same.txt","unknown-c"),row(-1,"same.txt","unknown-d"),
                             row(1,"","unknown-e"),row(1,"","unknown-f")});
        QCOMPARE(model.rowCount(),11);
        model.setResults({first,first}); QCOMPARE(model.rowCount(),1);
        model.appendResults({second}); QCOMPARE(model.rowCount(),2);
        model.clearResults(); model.appendResults({first,first,second});
        QCOMPARE(model.rowCount(),2);
    }
    void searchResultsShowOnePathAndPreferAccessibleSource() {
        SearchResult offline,live;
        offline.file.source_id=1;offline.file.relative_path="shared.txt";
        offline.file.path="/synthetic/shared.txt";offline.file.parent_path="/synthetic";
        offline.file.name="shared.txt";offline.source_status=SourceStatus::Offline;
        live=offline;live.file.source_id=2;live.source_status=SourceStatus::Accessible;
        SearchResultModel model;model.setResults({offline,live});
        QCOMPARE(model.rowCount(),1);
        QCOMPARE(model.pathAt(0),QStringLiteral("/synthetic/shared.txt"));
        QCOMPARE(model.data(model.index(0,5)).toString(),QStringLiteral("上次可用"));
        model.appendResults({offline,live});QCOMPARE(model.rowCount(),1);
        model.setResults({offline});model.sort(0,Qt::AscendingOrder);
        model.appendResults({live});QCOMPARE(model.rowCount(),1);
        QCOMPARE(model.data(model.index(0,5)).toString(),QStringLiteral("上次可用"));
    }
    void workerRejectsChangedPageRevision() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path=dir.filePath("paging.db").toStdString();
        Database db(path); db.initialize(); const auto source=db.ensureSourceForRoot("/synthetic/paging");
        std::vector<FileRecord> rows;
        for(int i=0;i<6;++i) {
            FileRecord file; file.root="/synthetic/paging"; file.relative_path="page"+std::to_string(i)+".txt";
            file.path=file.root+"/"+file.relative_path; file.name=file.relative_path; file.parent_path=file.root;
            rows.push_back(file);
        }
        db.mergeSourceSnapshot(db.source(source),rows);
        SearchWorker worker(path);
        auto search=[&](std::size_t offset,std::optional<std::uint64_t> expected=std::nullopt) {
            return worker.search("page",3,offset,SearchOptions{},nullptr,expected);
        };
        auto first=search(0);
        QVERIFY(first.error.empty()); QCOMPARE(first.results.size(),std::size_t(3));
        const auto token=first.revision; QVERIFY(token.has_value());
        auto repeat=search(0);
        QCOMPARE(repeat.revision,token);
        auto second=search(3,token);
        QVERIFY(second.error.empty()); QVERIFY(!second.snapshot_changed);
        QCOMPARE(second.results.size(),std::size_t(3));
        QVERIFY(first.results.back().file.name<second.results.front().file.name);
        // Raw writers do not increment the source revision. data_version must
        // still notice their commits on the same monitoring connection.
        sqlite3* raw=nullptr; QCOMPARE(sqlite3_open(path.c_str(),&raw),SQLITE_OK);
        struct Close { sqlite3* db; ~Close(){sqlite3_close(db);} } close{raw};
        QCOMPARE(sqlite3_exec(raw,"UPDATE files SET size=size+7",nullptr,nullptr,nullptr),SQLITE_OK);
        auto invalid=search(3,token);
        QVERIFY2(invalid.snapshot_changed,"A raw SQL commit between pages must invalidate the earlier page token");
        QVERIFY(invalid.results.empty()); QVERIFY(invalid.error.empty());
        const auto fresh=search(0);
        QVERIFY(!fresh.snapshot_changed); QVERIFY(fresh.revision!=token);
        // Delete/recreate with identical ids/revisions must not produce an ABA
        // match, even though the catalog returns to the exact previous values.
        QCOMPARE(sqlite3_exec(raw,"BEGIN; CREATE TEMP TABLE prior_sources AS SELECT * FROM sources; DELETE FROM sources; INSERT INTO sources SELECT * FROM prior_sources; DROP TABLE prior_sources; COMMIT",nullptr,nullptr,nullptr),SQLITE_OK);
        const auto aba=search(3,fresh.revision);
        QVERIFY(aba.snapshot_changed); QVERIFY(aba.results.empty());
        auto restart=search(0); QVERIFY(!restart.snapshot_changed);
        int frames=-1,checkpointed=-1;
        QCOMPARE(sqlite3_wal_checkpoint_v2(raw,nullptr,SQLITE_CHECKPOINT_TRUNCATE,&frames,&checkpointed),SQLITE_OK);
        // A page token must not keep a SQLite read transaction alive.
        QCOMPARE(frames,0); QCOMPARE(checkpointed,0);
    }
    void cachedSourceColumns() {
        SearchResultModel model;
        SearchResult unknown; unknown.file.name="cache.txt"; unknown.file.path="/unreachable/cache.txt";
        model.setResults({unknown});
        QCOMPARE(model.columnCount(), 6);
        QVERIFY(model.data(model.index(0,4)).toString().contains(QStringLiteral("本地磁盘")));
        QVERIFY(model.data(model.index(0,5)).toString().contains(QStringLiteral("未核验")));
        auto offline=unknown; offline.source_label="NAS"; offline.source_type=SourceType::NAS;
        offline.source_status=SourceStatus::Offline; offline.source_last_success=1700000000;
        model.setResults({offline});
        QVERIFY(model.data(model.index(0,5)).toString().contains(QStringLiteral("离线缓存")));
        QVERIFY(model.data(model.index(0,5),Qt::ToolTipRole).toString().contains(QStringLiteral("上次可用")));
        auto app=unknown; app.file.name="Sample.app"; app.file.path="/Applications/Sample.app";
        app.file.is_directory=true;
        model.setResults({app});
        QVERIFY(model.data(model.index(0,0)).toString().contains(QStringLiteral("程序包")));
        QVERIFY(!model.data(model.index(0,0),Qt::DecorationRole).value<QIcon>().isNull());
    }
    void cancellationReachesServiceAndWorker() {
        MainWindow window; window.watcher_->stop();
        std::atomic_bool cancelled{true};
        SearchService service(window.db_path_.toStdString());
        for (auto scope : {SearchItemScope::All, SearchItemScope::Files}) {
            SearchOptions options; options.scope=scope;
            bool threw=false;
            try { cancellableSearch(service,options,&cancelled,0); }
            catch(const std::exception& e) { threw=std::string(e.what()).find("cancel")!=std::string::npos; }
            QVERIFY2(threw,"The default and filtered service branches must pass the cancellation token to SQLite");
        }
        SearchWorker worker(window.db_path_.toStdString());
        const auto outcome=cancellableSearch(worker,SearchOptions{},&cancelled,0);
        QVERIFY(!outcome.error.empty());
    }
    void continuousEventsSynchronizeBeforeQuiet() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto root=dir.path();
        MainWindow window; window.watcher_->stop(); window.roots_={root};
        QFile file(root+"/continuous.txt"); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("a"); file.close();
        QTimer events; events.setInterval(100);
        connect(&events,&QTimer::timeout,&window,[&] { window.scheduleFileEvent({file.fileName().toStdString(),false,{}}); });
        window.scheduleFileEvent({file.fileName().toStdString(),false,{}}); events.start();
        Database db(window.db_path_.toStdString());
        QTRY_VERIFY_WITH_TIMEOUT(!db.search("continuous.txt").empty(),1600);
        QVERIFY(events.isActive()); events.stop();
        QTRY_VERIFY(!window.worker_thread_);
    }
    void overflowKeepsEveryRoot() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto first=dir.filePath("a"), second=dir.filePath("b");
        QVERIFY(QDir().mkpath(first)); QVERIFY(QDir().mkpath(second));
        MainWindow window; window.watcher_->stop(); window.roots_={first,second};
        window.pending_paths_.clear(); window.pending_rescan_roots_.clear();
        window.scheduleFileEvent({(first+"/first.txt").toStdString(),false,{}});
        for(int i=0;i<401;++i) window.scheduleFileEvent({(second+QString("/file%1").arg(i)).toStdString(),false,{}});
        QVERIFY(window.pending_rescan_roots_.contains(first));
        QVERIFY(window.pending_rescan_roots_.contains(second));
    }
    void filesystemRefreshDoesNotInvalidateUserSearch() {
        MainWindow window; window.watcher_->stop();
        QSemaphore started,release;
        QMetaObject::invokeMethod(window.search_worker_,[&] { started.release(); release.acquire(); },Qt::QueuedConnection);
        QVERIFY(started.tryAcquire(1,3000));
        struct Release { QSemaphore& semaphore; ~Release() { semaphore.release(); } } guard{release};
        window.search_edit_->setText("alpha"); window.runSearch();
        const auto id=window.search_request_id_;
        window.reportSyncOutcome(fixture_root, fixture_root, true, {});
        QTest::qWait(1900);
        QCOMPARE(window.search_request_id_,id);
        QVERIFY(window.search_in_flight_);
        release.release(); QTRY_COMPARE(window.model_->nameAt(0),QStringLiteral("alpha.txt"));
    }
    void indexChangesKeepSearchAndSelectionUntilNextRequest() {
        MainWindow window; window.watcher_->stop();
        window.search_edit_->setText("alpha"); window.runSearch(); QTRY_COMPARE(window.model_->rowCount(),1);
        const auto id=window.search_request_id_;
        window.table_->selectRow(0);
        const auto selected=window.model_->pathAt(0);
        window.reportSyncOutcome(fixture_root, fixture_root, true, {});
        QTest::qWait(2100);
        QCOMPARE(window.search_request_id_,id);
        QCOMPARE(window.table_->selectionModel()->selectedRows().size(),1);
        QCOMPARE(window.model_->pathAt(window.table_->selectionModel()->selectedRows().front().row()),selected);
    }
    void failedRootDoesNotLoseOtherJobsAndRecovers() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto offline=dir.filePath("offline"), good=dir.filePath("good");
        QVERIFY(QDir().mkpath(good));
        QFile file(good+"/good_sync.txt"); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        MainWindow window; window.watcher_->stop(); window.roots_={offline,good};
        window.scheduleFileEvent({offline.toStdString(),true,{}});
        window.scheduleFileEvent({good.toStdString(),true,{}});
        window.processPendingEvents();
        Database db(window.db_path_.toStdString());
        QTRY_VERIFY_WITH_TIMEOUT(!db.search("good_sync.txt").empty(),4000);
        QTRY_VERIFY(!window.worker_thread_);
        QCOMPARE(window.sync_error_label_->text(),QStringLiteral("待更新 1 个范围"));
        QVERIFY(window.sync_error_label_->toolTip().contains(QStringLiteral("保留已有缓存")));
        QVERIFY(QDir().mkpath(offline));
        QFile restored(offline+"/restored_sync.txt"); QVERIFY(restored.open(QIODevice::WriteOnly)); restored.close();
        QTRY_VERIFY_WITH_TIMEOUT(!db.search("restored_sync.txt").empty(),5000);
        QTRY_VERIFY(!window.worker_thread_);
    }
    void renameDeleteAndOpenUseActualCurrentPath() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto root=dir.path(), old=root+"/Old", fresh=root+"/New";
        QVERIFY(QDir().mkpath(old));
        const auto filename=QStringLiteral("中文 空格#?%\"\\\n.txt");
        QFile file(old+"/"+filename); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("sample"); file.close();
        MainWindow window; window.watcher_->stop(); window.roots_={root};
        IndexManager manager(window.db_path_.toStdString()); QVERIFY(manager.rebuildRoot(root.toStdString()).completed);
        const auto offlineRoot=dir.filePath("offline_cache"); QVERIFY(QDir().mkpath(offlineRoot));
        QFile cached(offlineRoot+"/offline_survivor.txt"); QVERIFY(cached.open(QIODevice::WriteOnly)); cached.close();
        QVERIFY(manager.rebuildRoot(offlineRoot.toStdString()).completed);
        QVERIFY(QDir(offlineRoot).removeRecursively());
        QVERIFY(!manager.rebuildRoot(offlineRoot.toStdString()).completed);
        window.search_edit_->setText(QStringLiteral("中文 空格")); window.runSearch(); QTRY_COMPARE(window.model_->rowCount(),1);
        QVERIFY(QDir().rename(old,fresh));
        window.scheduleFileEvent({old.toStdString(),false,{}}); window.scheduleFileEvent({fresh.toStdString(),false,{}});
        QTimer events; events.setInterval(100); connect(&events,&QTimer::timeout,&window,[&] {window.scheduleFileEvent({root.toStdString(),false,{}});}); events.start();
        Database db(window.db_path_.toStdString());
        const auto oldScope = std::string("path:\"") + old.toStdString() + "\"";
        QTRY_VERIFY_WITH_TIMEOUT(db.search(oldScope).empty(),1800);
        QCOMPARE(window.model_->pathAt(0), old + "/" + filename);
        window.runSearch();
        QTRY_COMPARE_WITH_TIMEOUT(window.model_->pathAt(0),fresh+"/"+filename,4000);
        UiUrlSink sink; QDesktopServices::setUrlHandler("file",&sink,"accept");
        struct Unset { ~Unset(){QDesktopServices::unsetUrlHandler("file");} } unset;
        window.table_->selectRow(0); window.openCurrent();
        QCOMPARE(sink.urls.size(),1); QCOMPARE(sink.urls[0].toLocalFile(),fresh+"/"+filename);
        QVERIFY(QFile::remove(fresh+"/"+filename)); window.scheduleFileEvent({(fresh+"/"+filename).toStdString(),false,{}});
        QTRY_VERIFY_WITH_TIMEOUT(db.search("中文 空格").empty(),1800);
        QVERIFY(QDir(fresh).removeRecursively());
        window.scheduleFileEvent({fresh.toStdString(),false,{}});
        const auto newScope = std::string("path:\"") + fresh.toStdString() + "\"";
        QTRY_VERIFY_WITH_TIMEOUT(db.search(newScope).empty(),1800);
        events.stop(); QTRY_VERIFY(!window.worker_thread_);
        QVERIFY(!db.search("offline_survivor.txt").empty());
    }
    void nestedRootsKeepIndependentEventCoverage() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto root=dir.path(), nested=dir.filePath("nested"); QVERIFY(QDir().mkpath(nested));
        MainWindow window; window.watcher_->stop(); window.roots_={root,nested};
        window.pending_paths_.clear(); window.pending_rescan_roots_.clear();
        window.scheduleFileEvent({(root+"/a/file").toStdString(),false,{}});
        window.scheduleFileEvent({(root+"/a").toStdString(),false,{}});
        QCOMPARE(window.pending_paths_.size(),1); QVERIFY(window.pending_paths_.contains(root+"/a"));
        window.scheduleFileEvent({nested.toStdString(),false,{}});
        window.scheduleFileEvent({root.toStdString(),false,{}});
        // Root-sized ordinary changes use full-source scopes; the outer
        // source still cannot absorb the independently configured nested root.
        QCOMPARE(window.pending_rescan_roots_.size(),2);
        QVERIFY(window.pending_rescan_roots_.contains(root)); QVERIFY(window.pending_rescan_roots_.contains(nested));
    }
    void userDebounceRespects800msAndLateDelivery() {
        MainWindow window; window.watcher_->stop(); WorkerGate gate; QVERIFY(gate.block(window.search_worker_));
        window.search_edit_->setText("alpha"); window.runSearch(); QVERIFY(window.search_in_flight_);
        QElapsedTimer elapsed; qint64 deliveredNs=0;
        QSignalSpy deadlines(window.search_timer_,&QTimer::timeout);
        connect(window.search_timer_,&QTimer::timeout,&window,[&] {deliveredNs=elapsed.nsecsElapsed();});
        // Reproduce delayed delivery with a busy GUI event handler. Sleeping
        // 850 ms alone is not evidence that the real 800 ms timeout was sent.
        QTimer busy; busy.setSingleShot(true); busy.setTimerType(Qt::PreciseTimer);
        connect(&busy,&QTimer::timeout,&window,[] {QThread::msleep(170);});
        elapsed.start(); QTest::keyClicks(window.search_edit_,"b"); busy.start(700);
        QTest::qWait(850);
        qInfo("after 850 ms wait: elapsed=%lld ms, timeout_count=%lld, pending=%d, remaining=%d",
              static_cast<long long>(elapsed.elapsed()),static_cast<long long>(deadlines.size()),window.search_pending_,window.search_timer_->remainingTime());
        QTRY_COMPARE_WITH_TIMEOUT(deadlines.size(),1,2000);
        QVERIFY(deliveredNs>=870000000); QVERIFY(window.search_pending_); QVERIFY(window.search_in_flight_);
        for(int i=0;i<6;++i) {
            QTest::qWait(31); elapsed.restart(); QTest::keyClicks(window.search_edit_,"b");
            QVERIFY(window.search_timer_->isActive()); QVERIFY(!window.search_pending_);
            QTRY_COMPARE_WITH_TIMEOUT(deadlines.size(),i+2,2000);
            QVERIFY2(deliveredNs>=800000000,qPrintable(QStringLiteral("800 ms debounce delivered early at %1 ms").arg(deliveredNs/1000000.0)));
            QVERIFY(window.search_pending_); QVERIFY(window.search_in_flight_);
        }
    }
    void editsCancelAndCompletionHonorsDebounce() {
        MainWindow window; window.watcher_->stop();
        window.search_edit_->setText("alpha"); window.runSearch(); QTRY_COMPARE(window.model_->rowCount(),1);
        WorkerGate gate; QVERIFY(gate.block(window.search_worker_));
        window.runSearch(); const auto token=window.active_search_cancellation_; QVERIFY(token);
        window.search_edit_->selectAll(); QTest::keyClicks(window.search_edit_,"beta");
        QVERIFY(token->load());
        // Deliver the real timer signal at a controlled deadline. Real elapsed
        // 800 ms behavior is checked separately, including late delivery.
        window.search_timer_->stop();
        QVERIFY(QMetaObject::invokeMethod(window.search_timer_,"timeout",Qt::DirectConnection));
        QVERIFY(window.search_pending_);
        const auto dispatchedBeforeEditing=window.last_search_request_id_;
        window.search_edit_->selectAll(); QTest::keyClicks(window.search_edit_,"alpha");
        const auto id=window.search_request_id_;
        // A second worker gate proves the cancelled request has completed. Send
        // its actual posted GUI callback before delivering any new timer event.
        WorkerGate completed; completed.queue(window.search_worker_);
        gate.release->release(); QVERIFY(completed.started->tryAcquire(1,3000));
        QCoreApplication::sendPostedEvents(&window,QEvent::MetaCall);
        QVERIFY(!window.search_in_flight_); QVERIFY(window.search_timer_->isActive());
        QCOMPARE(window.last_search_request_id_,dispatchedBeforeEditing);
        completed.release->release(); window.search_timer_->stop();
        QVERIFY(QMetaObject::invokeMethod(window.search_timer_,"timeout",Qt::DirectConnection));
        QVERIFY(window.last_search_request_id_>id);
        QTRY_VERIFY(!window.search_in_flight_);
        QTRY_COMPARE(window.model_->nameAt(0),QStringLiteral("alpha.txt"));
    }
    void imeCancelsWithoutPrematureQueuedSearch() {
        MainWindow window; window.watcher_->stop();
        window.search_edit_->setText("alpha"); window.runSearch(); QTRY_COMPARE(window.model_->rowCount(),1);
        WorkerGate gate; QVERIFY(gate.block(window.search_worker_));
        window.runSearch(); const auto token=window.active_search_cancellation_;
        window.search_edit_->selectAll(); QTest::keyClicks(window.search_edit_,"beta");
        window.search_timer_->stop();
        QVERIFY(QMetaObject::invokeMethod(window.search_timer_,"timeout",Qt::DirectConnection));
        QVERIFY(window.search_pending_);
        QInputMethodEvent preedit(QStringLiteral("zhong"),{}); QApplication::sendEvent(window.search_edit_,&preedit);
        QVERIFY(token->load()); const auto id=window.last_search_request_id_;
        gate.release->release(); QTRY_VERIFY_WITH_TIMEOUT(!window.search_in_flight_,500);
        QTest::qWait(900); QCOMPARE(window.last_search_request_id_,id);
        QCOMPARE(window.model_->nameAt(0),QStringLiteral("alpha.txt"));
        QInputMethodEvent commit; commit.setCommitString(QStringLiteral("中文"),-4,4);
        QApplication::sendEvent(window.search_edit_,&commit); QVERIFY(window.search_timer_->isActive());
        QTest::qWait(300); QCOMPARE(window.last_search_request_id_,id);
        QTRY_COMPARE_WITH_TIMEOUT(window.model_->nameAt(0),QStringLiteral("中文.txt"),3000);
    }
    void scopesAndCloseCancelActiveToken() {
        MainWindow window; window.watcher_->stop(); WorkerGate gate; QVERIFY(gate.block(window.search_worker_));
        window.search_edit_->setText("alpha"); window.runSearch(); auto token=window.active_search_cancellation_;
        window.scope_combo_->setCurrentIndex(1); QVERIFY(token->load());
        gate.release->release(); QTRY_VERIFY(!window.search_in_flight_);
        window.search_timer_->stop();
        WorkerGate second; QVERIFY(second.block(window.search_worker_)); window.runSearch(); token=window.active_search_cancellation_;
        window.match_path_check_->setChecked(true); QVERIFY(token->load());
        second.release->release(); QTRY_VERIFY(!window.search_in_flight_); window.search_timer_->stop();
        WorkerGate last; QVERIFY(last.block(window.search_worker_)); window.runSearch(); token=window.active_search_cancellation_;
        window.close(); QVERIFY(token->load()); last.release->release(); QTRY_VERIFY(!window.search_in_flight_);
    }
    void exhaustedOfflineRetriesStayQuietUntilNewEvent() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto root=dir.filePath("absent");
        MainWindow window; window.watcher_->stop(); window.roots_={root};
        window.scheduleFileEvent({root.toStdString(),true,{}}); window.processPendingEvents();
        QTRY_VERIFY_WITH_TIMEOUT(window.dormant_rescan_roots_.contains(root),6500);
        QTRY_VERIFY(!window.worker_thread_);
        const auto attempts=window.sync_attempts_; QTest::qWait(1200);
        QVERIFY(!window.worker_thread_); QVERIFY(!window.event_timer_->isActive()); QCOMPARE(window.sync_attempts_,attempts);
        QVERIFY(QDir().mkpath(root)); QFile file(root+"/after_new_event.txt"); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        window.scheduleFileEvent({root.toStdString(),true,{}});
        Database db(window.db_path_.toStdString()); QTRY_VERIFY_WITH_TIMEOUT(!db.search("after_new_event.txt").empty(),2000);
        QTRY_VERIFY(!window.worker_thread_); QVERIFY(window.sync_errors_.isEmpty());
    }
    void manualRebuildReportsIncompleteSnapshot() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        MainWindow window; window.watcher_->stop(); window.roots_={dir.filePath("absent")};
        window.rebuildIndex(); QTRY_VERIFY(!window.worker_thread_);
        QVERIFY(!window.sync_errors_.isEmpty()); QVERIFY(!window.sync_error_label_->isHidden());
        QVERIFY(window.sync_error_label_->toolTip().contains(QStringLiteral("保留已有缓存")));
    }
    void syncFailureKeepsFixedSingleLineStatusSections() {
        MainWindow window;
        window.show();
        window.watcher_->stop();
        QVERIFY(window.search_metrics_label_);
        window.search_metrics_label_->setText(QStringLiteral("搜索结果 7 条 · 12.3 ms"));
        const auto metrics = window.search_metrics_label_->text();
        const auto path = QStringLiteral("/synthetic/very/long/directory/name/that/should/not/stretch/the/status/bar");
        window.reportSyncOutcome(path, path, true, QStringLiteral("synthetic synchronization failure"));
        QCOMPARE(window.search_metrics_label_->text(), metrics);
        QVERIFY(window.search_metrics_label_->isVisible());
        QVERIFY(!window.sync_error_label_->wordWrap());
        QVERIFY(window.sync_error_label_->maximumWidth() <= 140);
        QVERIFY(!window.sync_error_label_->text().contains(path));
        QVERIFY(window.sync_error_label_->toolTip().contains(path));
        window.scheduleFileEvent({{}, false, "synthetic watcher failure with a long path"});
        QCOMPARE(window.watcher_error_label_->text(), QStringLiteral("目录监听异常"));
        QVERIFY(window.watcher_error_label_->toolTip().contains(QStringLiteral("long path")));
        QVERIFY(!window.watcher_error_label_->wordWrap());
        window.status_label_->resize(110, window.status_label_->height());
        window.status_label_->setText(path);
        QVERIFY(window.status_label_->displayText().endsWith(QChar(0x2026)));
        QVERIFY(window.statusBar()->height() <= window.fontMetrics().height() * 2 + 16);
    }
    void optionalStatisticsFailureDoesNotReportSyncFailure() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        MainWindow window; window.watcher_->stop();
        window.db_path_=dir.path(); // SQLite cannot open a directory as a database.
        QString job_error=QStringLiteral("callback not called");
        window.startWorker([]{}, {}, false, [&](const QString& error){job_error=error;});
        QTRY_VERIFY_WITH_TIMEOUT(!window.worker_thread_,3000);
        QVERIFY(job_error.isEmpty());
        QCOMPARE(window.sync_error_label_->text(),QStringLiteral("索引同步待命"));
        window.startWorker([]{throw std::runtime_error("synthetic job failure");}, {}, false,
            [&](const QString& error){job_error=error;});
        QTRY_VERIFY_WITH_TIMEOUT(!window.worker_thread_,3000);
        QVERIFY(job_error.contains(QStringLiteral("synthetic job failure")));
    }
    void syncRetryDoesNotRepeatFailureBanner() {
        MainWindow window; window.watcher_->stop();
        const auto root=QStringLiteral("/synthetic/retry-source");
        window.reportSyncOutcome(root,root,true,QStringLiteral("synthetic scan failure"));
        QCOMPARE(window.sync_error_label_->text(),QStringLiteral("待更新 1 个范围"));
        QVERIFY(!window.statusBar()->currentMessage().contains(QStringLiteral("失败")));
        window.reportSyncOutcome(root,root,true,QStringLiteral("synthetic scan failure"));
        QCOMPARE(window.sync_error_label_->text(),QStringLiteral("待更新 1 个范围"));
        window.reportSyncOutcome(root,root,true,QStringLiteral("synthetic scan failure"));
        QCOMPARE(window.sync_error_label_->text(),QStringLiteral("待更新 1 个范围"));
        QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("待更新")));
        window.reportSyncOutcome(root,root,true,{});
        QCOMPARE(window.sync_error_label_->text(),QStringLiteral("索引同步成功"));
    }
    void incompleteIndexUpdatesAreInformationWithExactScopes() {
        MainWindow window; window.watcher_->stop();
        const auto path = QStringLiteral("/synthetic/incomplete-source");
        window.reportSyncOutcome(path, path, true, QStringLiteral("scan interrupted"));
        QVERIFY(window.sync_error_label_->text().contains(QStringLiteral("待更新")));
        QVERIFY(!window.sync_error_label_->text().contains(QStringLiteral("失败")));
        QString details;
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!dialog) return;
            details = dialog->text(); dialog->accept();
        });
        window.showIndexStatus();
        QVERIFY(details.contains(path));
        QVERIFY(details.contains(QStringLiteral("scan interrupted")));
    }
    void indexStatusShowsPersistedIncompleteSourcesAfterRestart() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto db_path = dir.filePath(QStringLiteral("index.db"));
        const auto absent = dir.filePath(QStringLiteral("missing-root"));
        IndexManager manager(db_path.toStdString());
        QVERIFY(!manager.rebuildRoot(absent.toStdString()).completed);
        sqlite3* raw = nullptr;
        QCOMPARE(sqlite3_open(db_path.toStdString().c_str(), &raw), SQLITE_OK);
        QCOMPARE(sqlite3_exec(raw, "UPDATE sources SET status=3", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(raw);
        MainWindow window; window.watcher_->stop();
        window.db_path_ = db_path;
        QVERIFY(window.sync_errors_.isEmpty());
        QString details;
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!dialog) return;
            details = dialog->text(); dialog->accept();
        });
        window.showIndexStatus();
        QVERIFY(details.contains(absent));
        QVERIFY(details.contains(QStringLiteral("上次扫描未完成")));
    }
    void failedSourceWithPriorSnapshotShowsCachedState() {
        SearchResult row;
        row.file.path="/synthetic/cached.txt";
        row.file.name="cached.txt";
        row.source_status=SourceStatus::Failed;
        row.source_last_success=1700000000;
        QCOMPARE(statusText(row),QStringLiteral("缓存结果（更新未完成）"));
        row.source_last_success=0;
        QCOMPARE(statusText(row),QStringLiteral("检查未完成"));
    }
    void statusLabelsExplainListenerSyncAndSearch() {
        auto settings = makeAppSettings();
        settings.remove("index/syncRetryBaseMs");
        MainWindow window;
        window.watcher_->stop();
        QCOMPARE(window.sync_retry_base_ms_, 5000);
        QCOMPARE(window.watcher_error_label_->text(), QStringLiteral("目录监听正常"));
        QVERIFY(window.watcher_error_label_->toolTip().contains(QStringLiteral("索引目录")));
        QCOMPARE(window.sync_error_label_->text(), QStringLiteral("索引同步待命"));
        QCOMPARE(window.search_metrics_label_->text(), QStringLiteral("尚未搜索"));
        const auto path = QStringLiteral("/synthetic/status-source");
        window.reportSyncOutcome(path, path, true, {});
        QCOMPARE(window.sync_error_label_->text(), QStringLiteral("索引同步成功"));
        QVERIFY(window.sync_error_label_->toolTip().contains(QStringLiteral("已完成")));
        window.search_edit_->setText(QStringLiteral("alpha"));
        window.runSearch();
        QTRY_COMPARE(window.search_log_entries_.back().state, QStringLiteral("完成"));
        QVERIFY(window.search_metrics_label_->text().startsWith(QStringLiteral("搜索结果 1 条 · ")));
        QVERIFY(window.search_metrics_label_->text().endsWith(QStringLiteral(" ms")));
    }
    void searchLogListsRunDetailsAndCanBeCleared() {
        MainWindow window;
        window.watcher_->stop();
        window.search_edit_->setText(QStringLiteral("alpha"));
        window.runSearch();
        QTRY_VERIFY(!window.search_in_flight_);
        QCOMPARE(window.search_log_entries_.size(), std::size_t{1});
        QCOMPARE(window.search_log_entries_.front().query, QStringLiteral("alpha"));
        QTRY_COMPARE(window.search_log_entries_.front().state, QStringLiteral("完成"));
        QVERIFY(window.search_log_entries_.front().elapsed_ms >= 0.0);

        QAction* log_action = nullptr;
        for (auto* action : window.findChildren<QAction*>())
            if (action->text() == QStringLiteral("搜索日志…")) log_action = action;
        QVERIFY(log_action);
        log_action->trigger();
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("searchLogDialog"));
        QVERIFY(dialog && dialog->isVisible());
        auto* list = dialog->findChild<QListWidget*>(QStringLiteral("searchLogList"));
        auto* details = dialog->findChild<QPlainTextEdit*>(QStringLiteral("searchLogDetails"));
        QVERIFY(list && details);
        QCOMPARE(list->count(), 1);
        QVERIFY(details->toPlainText().contains(QStringLiteral("alpha")));
        QVERIFY(details->toPlainText().contains(QStringLiteral("耗时")));
        auto* clear = dialog->findChild<QPushButton*>(QStringLiteral("clearSearchLog"));
        QVERIFY(clear);
        clear->click();
        QCOMPARE(list->count(), 0);
        QCOMPARE(window.search_log_entries_.size(), std::size_t{0});
    }
    void searchLogMarksSupersededQueryCancelled() {
        MainWindow window;
        window.watcher_->stop();
        WorkerGate gate;
        QVERIFY(gate.block(window.search_worker_));
        window.search_edit_->setText(QStringLiteral("alpha"));
        window.runSearch();
        QVERIFY(window.search_in_flight_);
        window.search_edit_->setText(QStringLiteral("beta"));
        window.runSearch();
        gate.release->release();
        QTRY_COMPARE_WITH_TIMEOUT(window.model_->nameAt(0), QStringLiteral("beta.txt"), 5000);
        QTRY_VERIFY(!window.search_in_flight_);
        QCOMPARE(window.search_log_entries_.size(), std::size_t{2});
        QCOMPARE(window.search_log_entries_.front().state, QStringLiteral("取消"));
        QTRY_COMPARE(window.search_log_entries_.back().state, QStringLiteral("完成"));
    }
    void searchLogRecordsIndexSyncWarningAndUnindexedFolder() {
        MainWindow window;
        window.watcher_->stop();
        const auto path = QStringLiteral("/synthetic/failed-source");
        window.reportSyncOutcome(path, path, true, QStringLiteral("synthetic failure"));
        window.search_edit_->setText(QStringLiteral("alpha"));
        window.runSearch();
        QTRY_COMPARE(window.search_log_entries_.back().state, QStringLiteral("完成"));
        QCOMPARE(window.search_log_entries_.back().sync_failure_count, 1);
        window.showSearchLog();
        auto* details = window.findChild<QPlainTextEdit*>(QStringLiteral("searchLogDetails"));
        QVERIFY(details);
        QVERIFY(details->toPlainText().contains(QStringLiteral("1 个范围正在重试或未更新")));

        window.setFolderScope(QStringLiteral("/synthetic/unindexed-folder"));
        window.runSearch();
        QCOMPARE(window.search_log_entries_.back().state, QStringLiteral("未执行"));
        QVERIFY(window.search_log_entries_.back().detail.contains(QStringLiteral("未索引")));
    }
    void unchangedFilesystemSyncDoesNotRepeatSearch() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto root = dir.filePath(QStringLiteral("root"));
        QVERIFY(QDir().mkpath(root));
        QFile first(root + QStringLiteral("/alpha-one.txt"));
        QVERIFY(first.open(QIODevice::WriteOnly)); first.close();
        const auto db_path = dir.filePath(QStringLiteral("index.db"));
        IndexManager manager(db_path.toStdString());
        QVERIFY(manager.rebuildRoot(root.toStdString()).completed);
        const auto saved_db = qgetenv("EVERYTHING_LITE_DB");
        qputenv("EVERYTHING_LITE_DB", db_path.toUtf8());
        MainWindow window;
        qputenv("EVERYTHING_LITE_DB", saved_db);
        window.watcher_->stop();
        window.roots_ = {root};
        window.search_edit_->setText(QStringLiteral("alpha"));
        window.runSearch();
        QTRY_COMPARE(window.search_log_entries_.back().state, QStringLiteral("完成"));
        QCOMPARE(window.model_->rowCount(), 1);
        const auto initial_log_count = window.search_log_entries_.size();
        const auto initial_status = window.status_label_->text();
        const auto initial_metrics = window.search_metrics_label_->text();
        window.scheduleFileEvent({first.fileName().toStdString(), false, {}});
        window.processPendingEvents();
        QTRY_VERIFY(!window.worker_thread_);
        QTest::qWait(2100);
        QCOMPARE(window.search_log_entries_.size(), initial_log_count);
        QCOMPARE(window.status_label_->text(), initial_status);
        QCOMPARE(window.search_metrics_label_->text(), initial_metrics);

        QFile second(root + QStringLiteral("/alpha-two.txt"));
        QVERIFY(second.open(QIODevice::WriteOnly)); second.close();
        window.scheduleFileEvent({second.fileName().toStdString(), false, {}});
        window.processPendingEvents();
        QTRY_VERIFY(!window.worker_thread_);
        QTest::qWait(2100);
        QCOMPARE(window.model_->rowCount(), 1);
        QCOMPARE(window.search_log_entries_.size(), initial_log_count);
        window.runSearch();
        QTRY_COMPARE_WITH_TIMEOUT(window.model_->rowCount(), 2, 5000);
    }
    void ownIndexDatabaseEventsDoNotScheduleSync() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto db_path = dir.filePath(QStringLiteral("index.db"));
        const auto saved_db = qgetenv("EVERYTHING_LITE_DB");
        qputenv("EVERYTHING_LITE_DB", db_path.toUtf8());
        MainWindow window;
        qputenv("EVERYTHING_LITE_DB", saved_db);
        window.watcher_->stop();
        window.roots_ = {dir.path()};
        window.scheduleFileEvent({db_path.toStdString(), false, {}});
        window.scheduleFileEvent({(db_path + QStringLiteral("-wal")).toStdString(), false, {}});
        window.scheduleFileEvent({(db_path + QStringLiteral("-shm")).toStdString(), false, {}});
        QVERIFY(window.pending_paths_.isEmpty());
        QVERIFY(window.pending_rescan_roots_.isEmpty());
        QVERIFY(!window.event_timer_->isActive());
    }
    void manualRebuildShowsBusyAndCompletionFeedback() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto saved=qgetenv("EVERYTHING_LITE_DB");
        qputenv("EVERYTHING_LITE_DB",dir.filePath("index.db").toUtf8());
        MainWindow window; qputenv("EVERYTHING_LITE_DB",saved); window.watcher_->stop();
        window.roots_={dir.path()};
        auto release=std::make_shared<QSemaphore>();
        window.startWorker([release]{release->acquire();},QStringLiteral("busy"));
        window.rebuildIndex();
        QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("正在运行")));
        release->release(); QTRY_VERIFY_WITH_TIMEOUT(!window.worker_thread_,5000);
        QFile file(dir.filePath("new.txt")); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        window.rebuildIndex();
        QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("正在重建")));
        auto* progress=window.findChild<QProgressDialog*>(QStringLiteral("rebuildProgressDialog"));
        QVERIFY(progress && progress->isVisible());
        QVERIFY(progress->labelText().contains(QStringLiteral("已处理")));
        QTRY_VERIFY_WITH_TIMEOUT(!window.worker_thread_,10000);
        Database db(window.db_path_.toStdString()); QVERIFY(!db.search("new.txt").empty());
        QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("重建完成")));
    }
    void manualRetryRenewsExhaustedSourceBudget() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto root=dir.filePath("paused");
        QVERIFY(QDir().mkpath(root));
        QFile file(root+"/manual_offline_cache.txt"); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        MainWindow window; window.watcher_->stop(); window.roots_={root};
        IndexManager manager(window.db_path_.toStdString()); QVERIFY(manager.rebuildRoot(root.toStdString()).completed);
        QVERIFY(QDir(root).removeRecursively());
        window.scheduleFileEvent({root.toStdString(),true,{}}); window.processPendingEvents();
        QTRY_VERIFY_WITH_TIMEOUT(window.dormant_rescan_roots_.contains(root),6500);
        QTRY_VERIFY(!window.worker_thread_);
        window.rebuildIndex(); QTRY_VERIFY(!window.worker_thread_);
        auto atSecondAttempt=[&] {return window.sync_attempts_.size()==1 && window.sync_attempts_.cbegin().value()==2;};
        QTRY_VERIFY_WITH_TIMEOUT(atSecondAttempt(),3500);
        QTRY_VERIFY_WITH_TIMEOUT(window.dormant_rescan_roots_.contains(root),3500);
        QTRY_VERIFY(!window.worker_thread_);
        const auto attempts=window.sync_attempts_; QTest::qWait(1100);
        QCOMPARE(window.sync_attempts_,attempts); QVERIFY(!window.retry_timer_->isActive());
        Database db(window.db_path_.toStdString()); QVERIFY(!db.search("manual_offline_cache.txt").empty());
    }
    void cachedActionsBlockOnlyKnownUnavailableAndCopyStillWorks() {
        MainWindow window; window.watcher_->stop();
        SearchResult row; row.file.path="/unverified/cache.txt"; row.file.name="cache.txt";
        UiUrlSink sink; QDesktopServices::setUrlHandler("file",&sink,"accept");
        struct Unset {~Unset(){QDesktopServices::unsetUrlHandler("file");}} unset;
        for (const auto status : {SourceStatus::Unverified,SourceStatus::Failed,SourceStatus::Offline}) {
            row.source_status=status; window.model_->setResults({row}); window.table_->selectRow(0);
            window.table_->setCurrentIndex(window.model_->index(0,0));
            const auto before=sink.urls.size(); window.openCurrent();
            QCOMPARE(sink.urls.size(),before+(status==SourceStatus::Offline ? 0 : 1));
            window.copyCurrentPath(); QCOMPARE(QApplication::clipboard()->text(),QStringLiteral("/unverified/cache.txt"));
            if (status==SourceStatus::Offline) {window.revealCurrent();window.quickLookCurrent();QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("离线缓存")));}
        }
        row.source_status=SourceStatus::Accessible; row.file.cloud_copy=CloudCopy::RemoteOnly;
        window.model_->setResults({row}); window.table_->selectRow(0); const auto before=sink.urls.size();
        window.openCurrent(); QCOMPARE(sink.urls.size(),before);
        QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("云端")));
    }
    void doubleClickOpensFileOrContainingFolderByColumn() {
        MainWindow window; window.watcher_->stop();
        SearchResult row;
        row.file.path = (fixture_root + QStringLiteral("/alpha.txt")).toStdString();
        row.file.name = "alpha.txt";
        row.source_status = SourceStatus::Accessible;
        window.model_->setResults({row});
        UiUrlSink sink; QDesktopServices::setUrlHandler("file", &sink, "accept");
        struct Unset { ~Unset() { QDesktopServices::unsetUrlHandler("file"); } } unset;
        QVERIFY(QMetaObject::invokeMethod(window.table_, "doubleClicked", Qt::DirectConnection,
            Q_ARG(QModelIndex, window.model_->index(0, 0))));
        QCOMPARE(sink.urls.size(), 1);
        QCOMPARE(sink.urls.back().toLocalFile(), fixture_root + QStringLiteral("/alpha.txt"));
        QVERIFY(QMetaObject::invokeMethod(window.table_, "doubleClicked", Qt::DirectConnection,
            Q_ARG(QModelIndex, window.model_->index(0, 1))));
        QCOMPARE(sink.urls.size(), 2);
        QCOMPARE(sink.urls.back().toLocalFile(), fixture_root);
    }
    void sixColumnSortAndSettingsMigration() {
        QStandardItemModel old(0,4); QTableView oldTable; oldTable.setModel(&old);
        oldTable.setColumnWidth(0,222);
        auto settings = makeAppSettings(); settings.setValue("table/header",oldTable.horizontalHeader()->saveState());
        settings.setValue("table/sortColumn",99);
        MainWindow window; window.watcher_->stop(); QCOMPARE(window.model_->columnCount(),6);
        QCOMPARE(window.table_->horizontalHeader()->count(),6);
        QVERIFY(window.table_->columnWidth(4)>0); QVERIFY(window.table_->columnWidth(5)>0);
        SearchResult a,b; a.source_label="Z"; b.source_label="A"; a.file.path="/z"; b.file.path="/a";
        a.source_status=SourceStatus::Offline; b.source_status=SourceStatus::Accessible;
        window.model_->setResults({a,b}); window.model_->sort(4); QCOMPARE(window.model_->pathAt(0),QStringLiteral("/a"));
        SearchBookmark bookmark; bookmark.sort_column=5; window.applyBookmark(bookmark);
        QCOMPARE(window.table_->horizontalHeader()->sortIndicatorSection(),5);
        window.resetColumns(); QVERIFY(window.table_->columnWidth(4)>0);
        settings.remove("table/header"); settings.remove("table/sortColumn");
    }
    void busyWorkerContinuesDueEventsPromptly() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto root=dir.path();
        QFile file(root+"/queued_due.txt"); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        MainWindow window; window.watcher_->stop(); window.roots_={root};
        auto release=std::make_shared<QSemaphore>();
        struct Release {std::shared_ptr<QSemaphore> sem;~Release(){sem->release();}} guard{release};
        window.startWorker([release]{release->acquire();},QStringLiteral("blocked test worker"),false);
        window.scheduleFileEvent({file.fileName().toStdString(),false,{}});
        QTest::qWait(850); QVERIFY(window.worker_thread_); release->release();
        Database db(window.db_path_.toStdString());
        QTRY_VERIFY_WITH_TIMEOUT(!db.search("queued_due.txt").empty(),200);
        QTRY_VERIFY(!window.worker_thread_);
    }
    void emptyQueryWorkerFinishesWithReadyStatus() {
        MainWindow window; window.watcher_->stop();
        window.rebuildIndex(); QTRY_VERIFY_WITH_TIMEOUT(!window.worker_thread_,5000);
        QVERIFY2(!window.status_label_->text().contains(QStringLiteral("正在")),"Completed index work must not leave a stale busy status at empty query");
        QVERIFY(window.status_label_->text().contains(QStringLiteral("个对象")));
        QCOMPARE(window.last_search_request_id_,std::uint64_t(0));
    }
    void thrownJobDoesNotDiscardOtherSource() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto first = dir.filePath("first"), second = dir.filePath("second");
        QVERIFY(QDir().mkpath(first));
        QVERIFY(QDir().mkpath(second));
        MainWindow window;
        window.watcher_->stop();
        window.roots_ = {first, second};
        window.scheduleFileEvent({first.toStdString(), true, {}});
        window.scheduleFileEvent({second.toStdString(), true, {}});
        // Make the actual first job throw, so a batch-level catch would lose
        // the other root regardless of QSet's randomized iteration order.
        const auto bad = *window.pending_rescan_roots_.begin();
        const auto good = bad == first ? second : first;
        QFile file(good + "/exception_survivor.txt");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        Database db(window.db_path_.toStdString());
        db.ensureSourceForRoot(bad.toStdString());
        SourceRecord duplicate;
        duplicate.root = bad.toStdString();
        duplicate.label = "ambiguous";
        duplicate.identity = "second-source";
        db.saveSource(duplicate);
        window.processPendingEvents();
        QTRY_VERIFY_WITH_TIMEOUT(!db.search("exception_survivor.txt").empty(), 3000);
        QTRY_VERIFY(!window.worker_thread_);
        QVERIFY(!window.sync_errors_.isEmpty());
    }
    void csvAppendsQuotedCachedSourceAndStatus() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto output=dir.filePath("results.csv");
        MainWindow window; window.watcher_->stop();
        SearchResult row; row.file.name="a\"b.txt"; row.file.path="/cache/a\"b.txt";
        row.source_label="Cloud,\"A\"\nB"; row.source_type=SourceType::Cloud; row.source_status=SourceStatus::Offline;
        window.model_->setResults({row});
        bool accepted=false;
        QTimer chooser; chooser.setInterval(20);
        connect(&chooser,&QTimer::timeout,&window,[&] {
            for(auto* widget:QApplication::topLevelWidgets()) if(auto* dialog=qobject_cast<QFileDialog*>(widget)) {
                if(!dialog->isVisible()) continue;
                // Qt 6.4 preserves focused filename edits in selectFile().
                // Move focus away before using the public selection API.
                if(auto* focused=dialog->focusWidget()) focused->clearFocus();
                dialog->selectFile(output);
                if(dialog->selectedFiles()!=QStringList{output}) return;
                connect(dialog,&QDialog::accepted,&window,[&,dialog] {
                    accepted=dialog->selectedFiles()==QStringList{output};
                });
                chooser.stop(); QMetaObject::invokeMethod(dialog,"accept",Qt::DirectConnection); return;
            }
        });
        QTimer::singleShot(2000,&window,[&]{if(!accepted) for(auto* widget:QApplication::topLevelWidgets()) if(auto* dialog=qobject_cast<QFileDialog*>(widget)) dialog->reject();});
        chooser.start(); window.exportLoadedResults(); QVERIFY(accepted);
        QFile file(output); QVERIFY(file.open(QIODevice::ReadOnly)); const auto csv=file.readAll();
        QVERIFY(csv.startsWith("Name,Path,Size,Modified,Source,Status\n"));
        QVERIFY(csv.contains("\"a\"\"b.txt\""));
        QVERIFY(csv.contains(QStringLiteral("\"Cloud,\"\"A\"\"\nB（网盘）\"").toUtf8()));
        QVERIFY(csv.contains(QStringLiteral("\"离线缓存 · 副本未核验\"").toUtf8()));
    }
    void paginationConsumesRawRows() {
        MainWindow window; window.watcher_->stop();
        window.search_edit_->setText("item"); window.runSearch();
        QTRY_COMPARE(window.model_->rowCount(),1000); QTRY_VERIFY(!window.search_in_flight_);
        QCOMPARE(window.last_search_raw_result_count_,1001); QVERIFY(window.results_have_more_);
        // Projecting fewer display rows must not rewind the server cursor.
        // This represents a deduplicated display window independent of SQL rows.
        Database db(window.db_path_.toStdString());
        const auto first=db.search("item",1); QVERIFY(!first.empty());
        window.model_->setResults(first); QCOMPARE(window.model_->rowCount(),1);
        window.model_->sort(0,Qt::DescendingOrder);
        window.loadMoreResults(); QTRY_VERIFY(!window.search_in_flight_);
        QCOMPARE(window.last_search_raw_result_count_,5);
        QCOMPARE(window.model_->rowCount(),6);
        QCOMPARE(window.model_->nameAt(0),QStringLiteral("item1004.txt"));
        QVERIFY(!window.results_have_more_);
    }
    void folderScopeSearchPersistsAndRejectsUnindexedFolder() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto root=dir.filePath("root");
        QVERIFY(QDir().mkpath(root+"/docs/sub"));
        QVERIFY(QDir().mkpath(root+"/docs-old"));
        for (const auto& path : {root+"/docs/item-a.txt",root+"/docs/sub/item-b.txt",root+"/docs-old/item-c.txt"}) {
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("test");
        }
        const auto db_path=dir.filePath("index.db");
        IndexManager manager(db_path.toStdString()); QVERIFY(manager.rebuildRoot(root.toStdString()).completed);
        auto settings = makeAppSettings(); settings.setValue("index/roots",QStringList{root});
        const auto old=qgetenv("EVERYTHING_LITE_DB"); qputenv("EVERYTHING_LITE_DB",db_path.toUtf8());
        MainWindow window; qputenv("EVERYTHING_LITE_DB",old); window.watcher_->stop();
        QVERIFY(window.findChild<QPushButton*>(QStringLiteral("chooseFolderScope")));
        auto* clear=window.findChild<QPushButton*>(QStringLiteral("clearFolderScope")); QVERIFY(clear);
        window.setFolderScope(root+"/docs");
        window.search_edit_->setText("item"); window.runSearch();
        QTRY_COMPARE(window.model_->rowCount(),2);
        QCOMPARE(window.model_->nameAt(0),QStringLiteral("item-a.txt"));
        QCOMPARE(window.model_->nameAt(1),QStringLiteral("item-b.txt"));
        window.saveSettings();
        QCOMPARE(settings.value("search/folderScope").toString(),root+"/docs");
        window.bookmarks_.push_back(SearchBookmark{QStringLiteral("docs"),QStringLiteral("item"),root+"/docs"});
        window.saveBookmarks();
        qputenv("EVERYTHING_LITE_DB",db_path.toUtf8());
        MainWindow restored; qputenv("EVERYTHING_LITE_DB",old); restored.watcher_->stop();
        QCOMPARE(restored.folder_scope_,root+"/docs");
        QCOMPARE(restored.bookmarks_.back().folder_scope,root+"/docs");
        SearchBookmark bookmark; bookmark.folder_scope=root+"/docs-old";
        bookmark.query="item"; window.applyBookmark(bookmark);
        QTRY_COMPARE(window.model_->rowCount(),1);
        QCOMPARE(window.model_->nameAt(0),QStringLiteral("item-c.txt"));
        window.setFolderScope(dir.filePath("unindexed"));
        QCOMPARE(window.model_->rowCount(),0);
        QVERIFY(window.findChild<QLabel*>(QStringLiteral("folderScopeStatus"))->text().contains(QStringLiteral("未索引")));
        window.runSearch(); QTRY_COMPARE(window.model_->rowCount(),0);
        clear->click(); QTRY_COMPARE(window.model_->rowCount(),3);
        QCOMPARE(window.folder_scope_,QString{});
        WorkerGate gate; QVERIFY(gate.block(window.search_worker_));
        window.setFolderScope(root+"/docs"); window.runSearch();
        QVERIFY(window.search_in_flight_);
        window.setFolderScope(root+"/docs-old");
        gate.release->release();
        QTRY_COMPARE_WITH_TIMEOUT(window.model_->rowCount(),1,3000);
        QCOMPARE(window.model_->nameAt(0),QStringLiteral("item-c.txt"));
    }
    void choosingUnindexedFolderRequiresConsentAndIndexesOnlyThatFolder() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto root=dir.filePath("indexed");
        const auto new_root=dir.filePath("new-root");
        QVERIFY(QDir().mkpath(root));
        QVERIFY(QDir().mkpath(new_root));
        QFile sample(new_root+"/unique-needle.txt");
        QVERIFY(sample.open(QIODevice::WriteOnly)); sample.write("test"); sample.close();
        const auto db_path=dir.filePath("index.db");
        IndexManager manager(db_path.toStdString()); QVERIFY(manager.rebuildRoot(root.toStdString()).completed);
        auto settings = makeAppSettings(); settings.setValue("index/roots",QStringList{root});
        const auto old=qgetenv("EVERYTHING_LITE_DB"); qputenv("EVERYTHING_LITE_DB",db_path.toUtf8());
        MainWindow window; qputenv("EVERYTHING_LITE_DB",old); window.watcher_->stop();
        window.search_edit_->setText("unique-needle");
        QTimer::singleShot(0, &window, [] {
            auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            QVERIFY(box); QVERIFY(box->text().contains(QStringLiteral("未索引")));
            box->button(QMessageBox::No)->click();
        });
        window.selectFolderScope(new_root);
        QCOMPARE(window.folder_scope_,new_root);
        QCOMPARE(window.model_->rowCount(),0);
        QVERIFY(window.folder_scope_status_->text().contains(QStringLiteral("未索引")));
        window.runSearch();
        QVERIFY(!window.search_in_flight_);
        QVERIFY(!window.search_timer_->isActive());
        QCOMPARE(window.roots_,(QStringList{root}));
        QTimer::singleShot(0, &window, [] {
            auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            QVERIFY(box); box->button(QMessageBox::Yes)->click();
        });
        window.selectFolderScope(new_root);
        QVERIFY(window.roots_.contains(new_root));
        QTRY_VERIFY_WITH_TIMEOUT(!window.worker_thread_,10000);
        QCOMPARE(window.sync_error_label_->text(), QStringLiteral("索引同步成功"));
        Database indexed(db_path.toStdString());
        QVERIFY2(indexed.totalFileCount()>=2,qPrintable(window.status_label_->text()));
        QTRY_VERIFY_WITH_TIMEOUT(window.search_request_id_>0,5000);
        QTRY_COMPARE_WITH_TIMEOUT(window.model_->rowCount(),1,5000);
        QCOMPARE(window.model_->nameAt(0),QStringLiteral("unique-needle.txt"));
        QVERIFY(window.roots_.contains(new_root));
    }
    void hiddenItemControlPersistsAndFiltersSearch() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto root=dir.filePath("root"); QVERIFY(QDir().mkpath(root));
        QFile hidden(root+"/.private-sample.txt");
        QVERIFY(hidden.open(QIODevice::WriteOnly)); hidden.write("test"); hidden.close();
        const auto db_path=dir.filePath("index.db");
        IndexManager manager(db_path.toStdString()); QVERIFY(manager.rebuildRoot(root.toStdString()).completed);
        auto settings = makeAppSettings(); settings.setValue("index/roots",QStringList{root});
        const auto old=qgetenv("EVERYTHING_LITE_DB"); qputenv("EVERYTHING_LITE_DB",db_path.toUtf8());
        MainWindow window; qputenv("EVERYTHING_LITE_DB",old); window.watcher_->stop();
        window.search_edit_->setText("private-sample"); window.runSearch();
        QTRY_COMPARE(window.model_->rowCount(),1);
        window.include_hidden_check_->setChecked(false); window.runSearch();
        QTRY_VERIFY(!window.search_in_flight_);
        QCOMPARE(window.model_->rowCount(),0);
        window.saveSettings();
        QCOMPARE(settings.value("search/includeHidden").toBool(),false);
        window.include_hidden_check_->setChecked(true); window.runSearch();
        QTRY_COMPARE(window.model_->rowCount(),1);
    }
    void paginationRefreshesChangedSnapshot() {
        MainWindow window; window.watcher_->stop();
        QCOMPARE(window.table_->horizontalHeader()->sortIndicatorOrder(),Qt::AscendingOrder);
        window.search_edit_->setText("item"); window.runSearch();
        QTRY_COMPARE(window.model_->rowCount(),1000); QTRY_VERIFY(!window.search_in_flight_);
        QCOMPARE(window.model_->nameAt(0),QStringLiteral("item0000.txt"));
        sqlite3* raw=nullptr; QCOMPARE(sqlite3_open(window.db_path_.toStdString().c_str(),&raw),SQLITE_OK);
        const auto changed=sqlite3_exec(raw,"UPDATE files SET size=size+1 WHERE name='item0000.txt'",nullptr,nullptr,nullptr);
        sqlite3_close(raw); QCOMPARE(changed,SQLITE_OK);
        window.loadMoreResults(); QTRY_VERIFY(!window.search_in_flight_);
        QCOMPARE(window.model_->rowCount(),0);
        QVERIFY(!window.results_have_more_); QVERIFY(window.search_timer_->isActive());
        const auto rejectedId=window.search_request_id_;
        QTest::qWait(100); QCOMPARE(window.search_request_id_,rejectedId);
        QTRY_COMPARE_WITH_TIMEOUT(window.model_->rowCount(),1000,3000);
        QVERIFY(window.search_request_id_>rejectedId);
        QCOMPARE(window.model_->nameAt(0),QStringLiteral("item0000.txt"));
        window.loadMoreResults(); QTRY_COMPARE(window.model_->rowCount(),1005);
        QVERIFY(!window.results_have_more_);
    }
    void paginationSignaturePreservesPercentText() {
        MainWindow window; window.watcher_->stop();
        window.search_edit_->setText("%2");
        const auto oldSignature=window.currentSearchSignature();
        window.search_edit_->setText("0");
        QVERIFY2(window.currentSearchSignature()!=oldSignature,"Literal query placeholders must not be formatted into another query's signature");
        // Retain an earlier page's signature and token, then attempt to append
        // after programmatic setText(), which emits no textEdited signal.
        window.pagination_signature_=oldSignature;
        window.search_revision_=1; window.next_search_offset_=1000;
        window.results_have_more_=true;
        window.loadMoreResults();
        QVERIFY(!window.search_in_flight_); QVERIFY(!window.results_have_more_);
        QCOMPARE(window.next_search_offset_,std::size_t(0));
        QVERIFY(!window.search_revision_); QVERIFY(window.search_timer_->isActive());
    }
    void paginationRejectsProgrammaticQueryChange() {
        MainWindow window; window.watcher_->stop();
        window.search_edit_->setText("item"); window.runSearch();
        QTRY_COMPARE(window.model_->rowCount(),1000); QTRY_VERIFY(!window.search_in_flight_);
        window.search_edit_->setText("alpha"); window.loadMoreResults();
        QVERIFY(!window.search_in_flight_); QVERIFY(!window.results_have_more_);
        QVERIFY(window.search_timer_->isActive());
        QTRY_COMPARE_WITH_TIMEOUT(window.model_->rowCount(),1,3000);
        QCOMPARE(window.model_->nameAt(0),QStringLiteral("alpha.txt"));
    }
    void paginationOldAppendCannotReplaceChangedQuery_data() {
        QTest::addColumn<bool>("changeFilter");
        QTest::newRow("query")<<false; QTest::newRow("filter")<<true;
    }
    void paginationOldAppendCannotReplaceChangedQuery() {
        QFETCH(bool,changeFilter);
        MainWindow window; window.watcher_->stop();
        window.search_edit_->setText("item"); window.runSearch();
        QTRY_COMPARE(window.model_->rowCount(),1000); QTRY_VERIFY(!window.search_in_flight_);
        WorkerGate gate; QVERIFY(gate.block(window.search_worker_));
        window.loadMoreResults(); QVERIFY(window.search_in_flight_);
        if(changeFilter) window.scope_combo_->setCurrentIndex(2);
        else {window.search_edit_->selectAll(); QTest::keyClicks(window.search_edit_,"beta");}
        QVERIFY(!window.results_have_more_);
        gate.release->release(); QTRY_VERIFY(!window.search_in_flight_);
        QCOMPARE(window.model_->rowCount(),1000);
        QTRY_COMPARE_WITH_TIMEOUT(window.model_->rowCount(),changeFilter?0:1,3000);
        if(!changeFilter) QCOMPARE(window.model_->nameAt(0),QStringLiteral("beta.txt"));
        QVERIFY(!window.results_have_more_);
    }
    void paginationWaitsForPendingUserQueryAndIme() {
        MainWindow window; window.watcher_->stop();
        window.search_edit_->setText("item"); window.runSearch(); QTRY_COMPARE(window.model_->rowCount(),1000);
        QVERIFY(window.results_have_more_); QTRY_VERIFY(!window.search_in_flight_);
        window.scope_combo_->setCurrentIndex(1);
        // The old result window must not issue pagination under the new scope
        // until the user's idle timer has fired.
        const auto id=window.search_request_id_;
        window.loadMoreResults(); QCOMPARE(window.search_request_id_,id);
        QInputMethodEvent preedit(QStringLiteral("zhong"),{}); QApplication::sendEvent(window.search_edit_,&preedit);
        const auto imeId=window.search_request_id_;
        window.loadMoreResults(); QCOMPARE(window.search_request_id_,imeId);
        QVERIFY(!window.search_in_flight_);
    }
    void unrelatedSourceChurnDoesNotResetFailedPathRetries() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto root=dir.path(),blocked=dir.filePath("blocked");
        QVERIFY(QDir().mkpath(blocked));
        QFile old(blocked+"/blocked_original.txt"); QVERIFY(old.open(QIODevice::WriteOnly)); old.close();
        QFile unrelated(root+"/unrelated.txt"); QVERIFY(unrelated.open(QIODevice::WriteOnly)); unrelated.close();
        MainWindow window; window.watcher_->stop(); window.roots_={root};
        IndexManager manager(window.db_path_.toStdString()); QVERIFY(manager.rebuildRoot(root.toStdString()).completed);
        const auto originalPermissions=QFile::permissions(blocked);
        struct RestorePermissions {QString path;QFile::Permissions permissions;~RestorePermissions(){QFile::setPermissions(path,permissions);}} restore{blocked,originalPermissions};
        QVERIFY(QFile::setPermissions(blocked,{}));
        if (manager.applyPathChange(root.toStdString(),blocked.toStdString()).completed)
            QSKIP("The current user can traverse this directory despite removed permissions; no real failure fixture is available.");
        window.scheduleFileEvent({blocked.toStdString(),false,{}}); window.processPendingEvents();
        QTimer churn; churn.setInterval(100); connect(&churn,&QTimer::timeout,&window,[&]{window.scheduleFileEvent({unrelated.fileName().toStdString(),false,{}});}); churn.start();
        QTRY_VERIFY_WITH_TIMEOUT(window.dormant_paths_.contains(blocked),6500);
        const auto attempts=window.sync_attempts_; QTest::qWait(900);
        QVERIFY(window.dormant_paths_.contains(blocked)); QCOMPARE(window.sync_attempts_,attempts);
        QVERIFY(!window.sync_errors_.isEmpty());
        QVERIFY(QFile::setPermissions(blocked,originalPermissions));
        QFile restored(blocked+"/restored_related.txt"); QVERIFY(restored.open(QIODevice::WriteOnly)); restored.close();
        window.scheduleFileEvent({blocked.toStdString(),false,{}});
        Database db(window.db_path_.toStdString()); QTRY_VERIFY_WITH_TIMEOUT(!db.search("restored_related.txt").empty(),2500);
        churn.stop(); QTRY_VERIFY(!window.worker_thread_); QVERIFY(window.sync_errors_.isEmpty());
    }
    void overflowDoesNotWakeExhaustedSource() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto paused=dir.filePath("paused"), good=dir.filePath("good");
        QVERIFY(QDir().mkpath(paused)); QVERIFY(QDir().mkpath(good));
        QFile cache(paused+"/overflow_offline_cache.txt"); QVERIFY(cache.open(QIODevice::WriteOnly)); cache.close();
        QFile newFile(good+"/overflow_live.txt"); QVERIFY(newFile.open(QIODevice::WriteOnly)); newFile.close();
        MainWindow window; window.watcher_->stop(); window.roots_={paused,good};
        IndexManager manager(window.db_path_.toStdString()); QVERIFY(manager.rebuildRoot(paused.toStdString()).completed);
        QVERIFY(QDir(paused).removeRecursively());
        window.scheduleFileEvent({paused.toStdString(),true,{}}); window.processPendingEvents();
        QTRY_VERIFY_WITH_TIMEOUT(window.dormant_rescan_roots_.contains(paused),6500);
        QTRY_VERIFY(!window.worker_thread_);
        const auto attempts=window.sync_attempts_;
        for(int i=0;i<402;++i) window.scheduleFileEvent({(paused+QString("/missing%1").arg(i)).toStdString(),false,{}});
        for(int i=0;i<402;++i) window.scheduleFileEvent({(good+QString("/sibling%1").arg(i)).toStdString(),false,{}});
        QVERIFY(window.pending_rescan_roots_.contains(good));
        QVERIFY2(!window.pending_rescan_roots_.contains(paused),"Overflow must retain the exhausted root as dormant instead of launching another full rescan");
        window.processPendingEvents();
        Database db(window.db_path_.toStdString()); QTRY_VERIFY_WITH_TIMEOUT(!db.search("overflow_live.txt").empty(),2000);
        QTRY_VERIFY(!window.worker_thread_);
        QCOMPARE(window.sync_attempts_,attempts);
        QVERIFY(window.dormant_rescan_roots_.contains(paused));
        QVERIFY(!db.search("overflow_offline_cache.txt").empty());
    }
    void normalEventsKeep700msDeadlineDuringRetryBackoff() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto paused=dir.filePath("paused"), good=dir.filePath("good"); QVERIFY(QDir().mkpath(good));
        MainWindow window; window.watcher_->stop(); window.roots_={paused,good};
        window.scheduleFileEvent({paused.toStdString(),true,{}}); window.processPendingEvents();
        auto atSecondAttempt=[&] {return window.sync_attempts_.size()==1 && window.sync_attempts_.cbegin().value()==2;};
        QTRY_VERIFY_WITH_TIMEOUT(atSecondAttempt(),3500); QTRY_VERIFY(!window.worker_thread_);
        QFile file(good+"/deadline_live.txt"); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        window.scheduleFileEvent({file.fileName().toStdString(),false,{}});
        QTest::qWait(1100);
        Database db(window.db_path_.toStdString());
        QVERIFY2(!db.search("deadline_live.txt").empty(),"A normal source event must sync after its own 700 ms deadline, without waiting for another source's 3000 ms retry");
        QVERIFY2(atSecondAttempt(),"Processing a normal event must not pull an unrelated retry forward");
        QTRY_VERIFY_WITH_TIMEOUT(window.dormant_rescan_roots_.contains(paused),3500);
        QTRY_VERIFY(!window.worker_thread_);
    }
    void successfulAncestorClearsOnlyCoveredFailures() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto root=dir.filePath("root"), ancestor=root+"/branch", child=ancestor+"/blocked";
        const auto unrelated=root+"/unrelated", offline=dir.filePath("offline");
        for(const auto& path:{child,unrelated,offline}) QVERIFY(QDir().mkpath(path));
        for(const auto& path:{child+"/child.txt",unrelated+"/sibling.txt",offline+"/ancestor_offline_cache.txt"}) {
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        }
        MainWindow window; window.watcher_->stop(); window.roots_={root,offline};
        IndexManager manager(window.db_path_.toStdString()); QVERIFY(manager.rebuildRoots({root.toStdString(),offline.toStdString()}).completed);
        const auto childPermissions=QFile::permissions(child), unrelatedPermissions=QFile::permissions(unrelated);
        struct Restore {QString path;QFile::Permissions permissions;~Restore(){QFile::setPermissions(path,permissions);}} first{child,childPermissions}, second{unrelated,unrelatedPermissions};
        QVERIFY(QFile::setPermissions(child,{})); QVERIFY(QFile::setPermissions(unrelated,{}));
        if (manager.applyPathChange(root.toStdString(),child.toStdString()).completed
            || manager.applyPathChange(root.toStdString(),unrelated.toStdString()).completed)
            QSKIP("The current user can traverse directories despite removed permissions; no real failure fixture is available.");
        QVERIFY(QDir(offline).removeRecursively());
        window.scheduleFileEvent({child.toStdString(),false,{}}); window.scheduleFileEvent({unrelated.toStdString(),false,{}}); window.scheduleFileEvent({offline.toStdString(),true,{}});
        window.processPendingEvents();
        QTRY_VERIFY_WITH_TIMEOUT(window.dormant_paths_.contains(child) && window.dormant_paths_.contains(unrelated) && window.dormant_rescan_roots_.contains(offline),6500);
        QTRY_VERIFY(!window.worker_thread_);
        QVERIFY(QFile::setPermissions(child,childPermissions));
        QFile file(child+"/ancestor_recovered.txt"); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        window.scheduleFileEvent({ancestor.toStdString(),false,{}});
        Database db(window.db_path_.toStdString()); QTRY_VERIFY_WITH_TIMEOUT(!db.search("ancestor_recovered.txt").empty(),2000);
        QTRY_VERIFY(!window.worker_thread_);
        QCOMPARE(window.sync_errors_.size(),2);
        QVERIFY(window.dormant_paths_.contains(unrelated)); QVERIFY(window.dormant_rescan_roots_.contains(offline));
        for(const auto& scope:window.sync_failure_scopes_) QVERIFY(scope.path!=child);
        QVERIFY(!db.search("ancestor_offline_cache.txt").empty());
    }
    void normalAndReadyRetryMergeOneScope() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto root=dir.filePath("root"), blocked=root+"/blocked", good=dir.filePath("good");
        QVERIFY(QDir().mkpath(blocked)); QVERIFY(QDir().mkpath(good));
        QFile cached(blocked+"/merge_retry_cache.txt"); QVERIFY(cached.open(QIODevice::WriteOnly)); cached.close();
        QFile live(good+"/merge_retry_other.txt"); QVERIFY(live.open(QIODevice::WriteOnly)); live.close();
        MainWindow window; window.watcher_->stop(); window.roots_={root,good};
        IndexManager manager(window.db_path_.toStdString()); QVERIFY(manager.rebuildRoot(root.toStdString()).completed);
        const auto permissions=QFile::permissions(blocked);
        struct Restore {QString path;QFile::Permissions permissions;~Restore(){QFile::setPermissions(path,permissions);}} restore{blocked,permissions};
        QVERIFY(QFile::setPermissions(blocked,{}));
        if (manager.applyPathChange(root.toStdString(),blocked.toStdString()).completed)
            QSKIP("The current user can traverse this directory despite removed permissions; no real failure fixture is available.");
        WorkerGate busy; const auto dbPath=window.db_path_;
        window.startWorker([&window,root,blocked,dbPath,started=busy.started,release=busy.release] {
            IndexManager manager(dbPath.toStdString());
            const auto stats=manager.applyPathChange(root.toStdString(),blocked.toStdString());
            const auto error=stats.completed ? QString{} : QString::fromUtf8(stats.error.c_str());
            QMetaObject::invokeMethod(&window,[&window,root,blocked,error] {window.reportSyncOutcome(root,blocked,false,error);},Qt::QueuedConnection);
            started->release(); release->acquire();
        },QStringLiteral("同步合并夹具"),false);
        // Queue a normal change before the first failure report reaches the GUI.
        // It therefore cannot renew/cancel the not-yet-recorded retry scope.
        window.scheduleFileEvent({blocked.toStdString(),false,{}});
        window.scheduleFileEvent({live.fileName().toStdString(),false,{}}); window.processPendingEvents();
        QVERIFY(window.sync_failure_scopes_.isEmpty()); QVERIFY(window.pending_paths_.contains(blocked));
        QVERIFY(busy.started->tryAcquire(1,3000)); QTRY_COMPARE(window.sync_errors_.size(),1);
        const auto key=window.sync_failure_scopes_.cbegin().key();
        QTRY_VERIFY_WITH_TIMEOUT(window.ready_sync_retries_.contains(key),2500);
        QVERIFY(window.normal_events_due_); busy.release->release();
        QTRY_VERIFY_WITH_TIMEOUT(window.sync_attempts_.value(key)>=2,2500); QTRY_VERIFY(!window.worker_thread_);
        QCOMPARE(window.sync_attempts_.value(key),2);
        QVERIFY(window.retry_deadlines_.contains(key)); QVERIFY(!window.ready_sync_retries_.contains(key));
        Database db(window.db_path_.toStdString()); QVERIFY(!db.search("merge_retry_other.txt").empty());
        QVERIFY(!db.search("merge_retry_cache.txt").empty());
        QVERIFY(QFile::setPermissions(blocked,permissions));
        window.scheduleFileEvent({blocked.toStdString(),false,{}});
        QTRY_VERIFY_WITH_TIMEOUT(!window.sync_errors_.contains(key),2500); QTRY_VERIFY(!window.worker_thread_);
        QVERIFY(!window.sync_attempts_.contains(key)); QVERIFY(!window.retry_deadlines_.contains(key));
        QVERIFY(!window.ready_sync_retries_.contains(key)); QVERIFY(!window.dormant_paths_.contains(blocked));
    }
    void ordinaryRootEventUsesFullSourceRetryBudget() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto root=dir.filePath("paused");
        QVERIFY(QDir().mkpath(root));
        QFile cached(root+"/ordinary_root_cache.txt"); QVERIFY(cached.open(QIODevice::WriteOnly)); cached.close();
        MainWindow window; window.watcher_->stop(); window.roots_={root};
        IndexManager manager(window.db_path_.toStdString()); QVERIFY(manager.rebuildRoot(root.toStdString()).completed);
        QVERIFY(QDir(root).removeRecursively());
        window.scheduleFileEvent({root.toStdString(),false,{}}); window.processPendingEvents();
        QTRY_VERIFY(!window.sync_failure_scopes_.isEmpty());
        const auto key=window.sync_failure_scopes_.cbegin().key();
        QVERIFY2(window.sync_failure_scopes_.value(key).rescan,"An ordinary event at the configured root performs a full-source snapshot and must share its full-source budget");
        QTimer churn; churn.setInterval(100);
        connect(&churn,&QTimer::timeout,&window,[&] {window.scheduleFileEvent({(root+"/unrelated.txt").toStdString(),false,{}});}); churn.start();
        QTRY_VERIFY_WITH_TIMEOUT(window.dormant_rescan_roots_.contains(root),6500);
        QTRY_VERIFY(!window.worker_thread_); QCOMPARE(window.sync_attempts_.value(key),3);
        QTest::qWait(900); QCOMPARE(window.sync_attempts_.value(key),3); QVERIFY(!window.worker_thread_);
        churn.stop(); Database db(window.db_path_.toStdString()); QVERIFY(!db.search("ordinary_root_cache.txt").empty());
    }
    void rescheduledBackoffClearsExpiredReadyJob() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto root=dir.filePath("root"), blocked=root+"/blocked", good=dir.filePath("good");
        QVERIFY(QDir().mkpath(blocked)); QVERIFY(QDir().mkpath(good));
        QFile cached(blocked+"/ready_cache.txt"); QVERIFY(cached.open(QIODevice::WriteOnly)); cached.close();
        QFile live(good+"/ready_other_scope.txt"); QVERIFY(live.open(QIODevice::WriteOnly)); live.close();
        MainWindow window; window.watcher_->stop(); window.roots_={root,good};
        IndexManager manager(window.db_path_.toStdString()); QVERIFY(manager.rebuildRoot(root.toStdString()).completed);
        const auto permissions=QFile::permissions(blocked);
        struct Restore {QString path;QFile::Permissions permissions;~Restore(){QFile::setPermissions(path,permissions);}} restore{blocked,permissions};
        QVERIFY(QFile::setPermissions(blocked,{}));
        if (manager.applyPathChange(root.toStdString(),blocked.toStdString()).completed)
            QSKIP("The current user can traverse this directory despite removed permissions; no real failure fixture is available.");
        WorkerGate busy;
        const auto secondBackoff=std::make_shared<std::atomic<int>>(0);
        const auto failures=std::make_shared<std::atomic<int>>(0);
        const auto dbPath=window.db_path_;
        // Two actual incomplete snapshots share a busy batch. A semaphore
        // keeps the batch running while the first real retry deadline expires.
        window.startWorker([&window,root,blocked,dbPath,busyStarted=busy.started,busyRelease=busy.release,secondBackoff,failures] {
            for(int attempt=1;attempt<=2;++attempt) {
                IndexManager manager(dbPath.toStdString());
                const auto stats=manager.applyPathChange(root.toStdString(),blocked.toStdString());
                if(!stats.completed) ++*failures;
                const auto error=stats.completed ? QString{} : QString::fromUtf8(stats.error.c_str());
                QMetaObject::invokeMethod(&window,[&window,root,blocked,error,attempt,secondBackoff] {
                    window.reportSyncOutcome(root,blocked,false,error);
                    if(attempt==2) {
                        QString key;
                        for(auto it=window.sync_failure_scopes_.cbegin();it!=window.sync_failure_scopes_.cend();++it)
                            if(it.value().root==root && it.value().path==blocked) key=it.key();
                        const bool valid=!key.isEmpty() && window.sync_attempts_.value(key)==2
                            && window.retry_deadlines_.contains(key) && !window.ready_sync_retries_.contains(key);
                        secondBackoff->store(valid ? 2 : 1);
                    }
                },Qt::QueuedConnection);
                if(attempt==1) {busyStarted->release();busyRelease->acquire();}
            }
        },QStringLiteral("同步退避夹具"),false);
        QVERIFY(busy.started->tryAcquire(1,3000)); QTRY_COMPARE(window.sync_errors_.size(),1);
        const auto key=window.sync_failure_scopes_.cbegin().key();
        QTRY_VERIFY_WITH_TIMEOUT(window.ready_sync_retries_.contains(key),2500);
        QVERIFY(window.worker_thread_); QCOMPARE(window.sync_attempts_.value(key),1);
        // Another source must still run when the busy worker completes.
        window.scheduleFileEvent({live.fileName().toStdString(),false,{}}); window.processPendingEvents();
        busy.release->release(); QTRY_VERIFY(secondBackoff->load()!=0);
        QCOMPARE(failures->load(),2);
        QVERIFY2(secondBackoff->load()==2,"A fresh backoff deadline must replace the expired ready marker for the same scope");
        Database db(window.db_path_.toStdString());
        QTRY_VERIFY_WITH_TIMEOUT(!db.search("ready_other_scope.txt").empty(),2500);
        QTRY_VERIFY_WITH_TIMEOUT(window.dormant_paths_.contains(blocked),4000);
        QTRY_VERIFY(!window.worker_thread_); QCOMPARE(window.sync_attempts_.value(key),3);
        QVERIFY(!window.ready_sync_retries_.contains(key)); QVERIFY(!window.retry_deadlines_.contains(key));
        QVERIFY(!db.search("ready_cache.txt").empty());
        QVERIFY(QFile::setPermissions(blocked,permissions));
        QFile recovered(blocked+"/ready_recovered.txt"); QVERIFY(recovered.open(QIODevice::WriteOnly)); recovered.close();
        window.scheduleFileEvent({recovered.fileName().toStdString(),false,{}});
        QTRY_VERIFY_WITH_TIMEOUT(!db.search("ready_recovered.txt").empty(),2500); QTRY_VERIFY(!window.worker_thread_);
        QVERIFY(!window.sync_failure_scopes_.contains(key)); QVERIFY(!window.sync_attempts_.contains(key));
        QVERIFY(!window.sync_errors_.contains(key)); QVERIFY(!window.ready_sync_retries_.contains(key));
        QVERIFY(!window.retry_deadlines_.contains(key)); QVERIFY(!window.dormant_paths_.contains(blocked));
        QVERIFY(!db.search("ready_other_scope.txt").empty());
    }
    void relatedChildEventRetainsBackingOffParentScope() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto root=dir.filePath("root"), blocked=root+"/blocked", offline=dir.filePath("offline");
        QVERIFY(QDir().mkpath(blocked)); QVERIFY(QDir().mkpath(offline));
        const auto stale=blocked+"/backoff_old.txt";
        for(const auto& path:{stale,offline+"/backoff_offline_cache.txt"}) {
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        }
        MainWindow window; window.watcher_->stop(); window.roots_={root,offline};
        IndexManager manager(window.db_path_.toStdString()); QVERIFY(manager.rebuildRoots({root.toStdString(),offline.toStdString()}).completed);
        QVERIFY(QDir(offline).removeRecursively());
        window.scheduleFileEvent({offline.toStdString(),true,{}}); window.processPendingEvents();
        QTRY_VERIFY_WITH_TIMEOUT(window.dormant_rescan_roots_.contains(offline),6500);
        QTRY_VERIFY(!window.worker_thread_); const auto offlineAttempts=window.sync_attempts_;
        const auto permissions=QFile::permissions(blocked);
        struct Restore {QString path;QFile::Permissions permissions;~Restore(){QFile::setPermissions(path,permissions);}} restore{blocked,permissions};
        QVERIFY(QFile::setPermissions(blocked,{}));
        if (manager.applyPathChange(root.toStdString(),blocked.toStdString()).completed)
            QSKIP("The current user can traverse this directory despite removed permissions; no real failure fixture is available.");
        window.scheduleFileEvent({blocked.toStdString(),false,{}}); window.processPendingEvents();
        QTRY_VERIFY(!window.worker_thread_);
        QString parentKey;
        for(auto it=window.sync_failure_scopes_.cbegin();it!=window.sync_failure_scopes_.cend();++it)
            if(it.value().root==root && it.value().path==blocked) parentKey=it.key();
        QVERIFY(!parentKey.isEmpty()); QCOMPARE(window.sync_attempts_.value(parentKey),1);
        QVERIFY(window.retry_deadlines_.contains(parentKey)); QVERIFY(!window.dormant_paths_.contains(blocked));
        QVERIFY(QFile::setPermissions(blocked,permissions)); QVERIFY(QFile::remove(stale));
        const auto child=blocked+"/backoff_trigger.txt", sibling=blocked+"/backoff_sibling.txt";
        for(const auto& path:{child,sibling}) {QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();}
        window.scheduleFileEvent({child.toStdString(),false,{}});
        Database db(window.db_path_.toStdString());
        QTRY_VERIFY_WITH_TIMEOUT(!db.search("backoff_sibling.txt").empty(),2500);
        QTRY_VERIFY(!window.worker_thread_);
        QVERIFY(db.search("backoff_old.txt").empty()); QVERIFY(!db.search("backoff_trigger.txt").empty());
        QVERIFY(!window.sync_failure_scopes_.contains(parentKey)); QVERIFY(!window.sync_errors_.contains(parentKey));
        QVERIFY(!window.retry_deadlines_.contains(parentKey)); QVERIFY(!window.ready_sync_retries_.contains(parentKey));
        for(auto it=offlineAttempts.cbegin();it!=offlineAttempts.cend();++it) QCOMPARE(window.sync_attempts_.value(it.key()),it.value());
        QVERIFY(window.dormant_rescan_roots_.contains(offline)); QVERIFY(!db.search("backoff_offline_cache.txt").empty());
    }
    void syncStartAndFinishKeepInputState() {
        MainWindow window; window.watcher_->stop();
        QTest::keyClicks(window.search_edit_,"alpha"); QVERIFY(window.search_timer_->isActive());
        const auto typing=window.status_label_->text(); QVERIFY(typing.contains(QStringLiteral("输入中")));
        window.scheduleFileEvent({(fixture_root+"/alpha.txt").toStdString(),false,{}}); window.processPendingEvents();
        QCOMPARE(window.status_label_->text(),typing);
        // Real IME composition protects the input state while allowing a slow
        // CI filesystem worker to finish without changing the 800 ms product delay.
        QInputMethodEvent preedit(QStringLiteral("zhong"),{});
        QApplication::sendEvent(window.search_edit_,&preedit);
        QTRY_VERIFY_WITH_TIMEOUT(!window.worker_thread_,10000);
        QVERIFY(window.search_ime_composing_); QCOMPARE(window.status_label_->text(),typing);
    }
    void manualProgressKeepsRunningUserQueryState() {
        MainWindow window; window.watcher_->stop(); WorkerGate gate; QVERIFY(gate.block(window.search_worker_));
        window.search_edit_->setText("alpha"); window.runSearch(); QVERIFY(window.search_in_flight_);
        const auto searching=window.status_label_->text(); QVERIFY(searching.contains(QStringLiteral("后台搜索")));
        window.rebuildIndex();
        QTRY_VERIFY_WITH_TIMEOUT(!window.worker_thread_,10000);
        QVERIFY(window.search_in_flight_);
        QCOMPARE(window.status_label_->text(),searching);
        gate.release->release(); QTRY_COMPARE(window.model_->nameAt(0),QStringLiteral("alpha.txt"));
    }
    void tooltipShowsCachedMarkupLiterallyWithoutResourceRequests() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto path=dir.filePath(QStringLiteral("literal<b>&.txt"));
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        const auto image=QUrl::fromLocalFile(dir.filePath("tooltip_image.png")).toString(QUrl::FullyEncoded);
        const auto source=QStringLiteral("<b>NAS</b> <img src=\"%1\">").arg(image);
        SearchResult row; row.file.path=path.toStdString(); row.file.name="literal<b>&.txt";
        row.source_label=source.toStdString(); row.source_type=SourceType::NAS;
        SearchResultModel model; model.setResults({row});
        const auto tooltip=model.data(model.index(0,0),Qt::ToolTipRole).toString();
        TooltipProbeDocument document;
        // This is the real Qt AutoText choice used by labels/tooltips, with
        // resource loading observed rather than allowing a file-image read.
        if (Qt::mightBeRichText(tooltip)) document.setHtml(tooltip);
        else document.setPlainText(tooltip);
        document.documentLayout()->documentSize();
        QVERIFY2(document.resource_requests.isEmpty(),"Cached names/source labels must not become tooltip image-resource requests");
        QVERIFY(document.toPlainText().contains(path));
        QVERIFY(document.toPlainText().contains(source));
        QCOMPARE(model.pathAt(0),path);
        QVERIFY(model.data(model.index(0,4)).toString().contains(source));
    }
    void menuHasPreview() {
        MainWindow window;
        QAction* preview = nullptr;
        for (auto* action : window.findChildren<QAction*>())
            if (action->text() == QStringLiteral("快速查看"))
                preview = action;
        QVERIFY2(preview, "Both platforms must expose Quick Preview in the actual File menu");
        QCOMPARE(preview->shortcut(), QKeySequence(Qt::Key_Space));
        window.search_edit_->setText("alpha");
        window.runSearch();
        QTRY_COMPARE(window.model_->rowCount(), 1);
        window.show();
        QTest::qWait(10);
        bool contextPreview = false, finder = false;
        QTimer::singleShot(0, &window, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu)
                return;
            for (auto* action : menu->actions()) {
                if (action->text() == QStringLiteral("快速查看"))
                    contextPreview = true;
                if (action->text().contains("Finder"))
                    finder = true;
            }
            menu->close();
        });
        window.showContextMenu(window.table_->visualRect(window.model_->index(0, 0)).center());
        QVERIFY(contextPreview);
#ifdef Q_OS_LINUX
        QVERIFY(!finder);
#else
        QVERIFY(finder);
#endif
    }
    void searchSpacesDoNotTriggerPreview() {
        MainWindow window;
        window.show();
        window.search_edit_->setText("alpha");
        window.runSearch();
        QTRY_COMPARE(window.model_->rowCount(), 1);
        window.table_->selectRow(0);
        QAction* preview = nullptr;
        for (auto* action : window.findChildren<QAction*>()) {
            if (action->text() == QStringLiteral("快速查看"))
                preview = action;
        }
        QVERIFY(preview);
        QSignalSpy triggered(preview, &QAction::triggered);
        window.search_edit_->setFocus();
        window.search_edit_->setCursorPosition(5);
        QTest::keyClick(window.search_edit_, Qt::Key_Space);
        QCOMPARE(window.search_edit_->text(), QStringLiteral("alpha "));
        QCOMPARE(triggered.size(), 0);
        QObject::disconnect(preview, nullptr, &window, nullptr);
        window.table_->setFocus();
        QTest::keyClick(window.table_, Qt::Key_Space);
        QCOMPARE(triggered.size(), 1);
    }
    void startupAndProgrammaticTextDoNotSearch() {
        MainWindow window;
        QTest::qWait(950);
        QCOMPARE(window.model_->rowCount(), 0);
        QCOMPARE(window.last_search_request_id_, std::uint64_t(0));
        window.search_edit_->setText("alpha");
        QTest::qWait(950);
        QCOMPARE(window.model_->rowCount(), 0);
    }
    void editedDelayAndEnter() {
        MainWindow window;
        QTest::keyClicks(window.search_edit_, "alpha");
        QCOMPARE(window.search_timer_->interval(), 800);
        QVERIFY(window.search_timer_->isActive());
        QTest::qWait(300);
        QCOMPARE(window.last_search_request_id_, std::uint64_t(0));
        QCOMPARE(window.model_->rowCount(), 0);
        // Completion also waits for SQLite and the worker under container load;
        // it is not a performance deadline for the 800 ms input debounce.
        QTRY_COMPARE_WITH_TIMEOUT(window.model_->rowCount(), 1, 5000);
        QCOMPARE(window.model_->nameAt(0), QStringLiteral("alpha.txt"));
        window.search_edit_->selectAll();
        QTest::keyClicks(window.search_edit_, "beta");
        QTest::qWait(300);
        QCOMPARE(window.model_->nameAt(0), QStringLiteral("alpha.txt"));
        QTest::keyClick(window.search_edit_, Qt::Key_Return);
        QVERIFY(!window.search_timer_->isActive());
        QCOMPARE(window.last_search_query_, QStringLiteral("beta"));
        QTRY_COMPARE_WITH_TIMEOUT(window.model_->nameAt(0), QStringLiteral("beta.txt"), 5000);
    }
    void imePreeditAndCommit() {
        MainWindow window;
        QTest::keyClicks(window.search_edit_, "alpha");
        QVERIFY(window.search_timer_->isActive());
        QInputMethodEvent preedit(QStringLiteral("zhong"), {});
        QApplication::sendEvent(window.search_edit_, &preedit);
        QVERIFY(window.search_ime_composing_);
        QVERIFY(!window.search_timer_->isActive());
        QTest::qWait(950);
        QCOMPARE(window.last_search_request_id_, std::uint64_t(0));
        QCOMPARE(window.model_->rowCount(), 0);
        QTest::keyClick(window.search_edit_, Qt::Key_Return);
        QCOMPARE(window.last_search_request_id_, std::uint64_t(0));
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("中文"), -5, 5);
        QApplication::sendEvent(window.search_edit_, &commit);
        QCOMPARE(window.search_edit_->text(), QStringLiteral("中文"));
        QVERIFY(!window.search_ime_composing_);
        QCOMPARE(window.search_timer_->interval(), 800);
        QVERIFY(window.search_timer_->isActive());
        QTest::qWait(300);
        QCOMPARE(window.model_->rowCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(window.model_->rowCount(), 1, 5000);
        QCOMPARE(window.model_->nameAt(0), QStringLiteral("中文.txt"));
    }
    void staleWorkerResultsAndOldView() {
        MainWindow window;
        window.search_edit_->setText("alpha");
        window.runSearch();
        QTRY_COMPARE(window.model_->rowCount(), 1);
        QSemaphore started, release;
        QMetaObject::invokeMethod(
            window.search_worker_,
            [&] {
                started.release();
                release.acquire();
            },
            Qt::QueuedConnection);
        QVERIFY(started.tryAcquire(1, 3000));
        window.launchSearch("alpha");
        window.search_edit_->selectAll();
        QTest::keyClicks(window.search_edit_, "beta");
        QTest::qWait(900); // New query has queued while the old worker query remains gated.
        const auto oldName = window.model_->nameAt(0);
        const auto requestBefore = window.search_request_id_;
        release.release();
        QCOMPARE(oldName, QStringLiteral("alpha.txt"));
        QTRY_COMPARE_WITH_TIMEOUT(window.last_search_query_, QStringLiteral("beta"), 3000);
        QTRY_COMPARE(window.model_->nameAt(0), QStringLiteral("beta.txt"));
        QVERIFY(window.last_search_request_id_ >= requestBefore);
    }
    void scopePathAndPaginationSorting() {
        MainWindow window;
        window.search_edit_->setText("needle");
        window.scope_combo_->setCurrentIndex(1);
        window.runSearch();
        QTRY_VERIFY(!window.search_in_flight_);
        QCOMPARE(window.model_->rowCount(), 0);
        window.match_path_check_->setChecked(true);
        window.runSearch();
        QTRY_COMPARE(window.model_->rowCount(), 1);
        QCOMPARE(window.model_->nameAt(0), QStringLiteral("opaque.txt"));
        window.scope_combo_->setCurrentIndex(2);
        window.runSearch();
        QTRY_COMPARE(window.model_->rowCount(), 1);
        QTRY_VERIFY(window.model_->isDirectoryAt(0));
        window.scope_combo_->setCurrentIndex(1);
        window.match_path_check_->setChecked(false);
        window.search_edit_->setText("item");
        window.runSearch();
        QTRY_COMPARE(window.model_->rowCount(), 1000);
        QVERIFY(window.results_have_more_);
        window.model_->sort(0, Qt::DescendingOrder);
        window.loadMoreResults();
        QTRY_COMPARE(window.model_->rowCount(), 1005);
        QVERIFY(!window.results_have_more_);
        QCOMPARE(window.model_->nameAt(0), QStringLiteral("item1004.txt"));
    }
#ifdef Q_OS_LINUX
    void localizedDefaultRoots() {
        auto settings = makeAppSettings();
        settings.remove("index/roots");
        MainWindow window;
        QCOMPARE(window.roots_, (QStringList{QDir::homePath() + QStringLiteral("/桌面"),
                                             QDir::homePath() + QStringLiteral("/文档"),
                                             QDir::homePath() + QStringLiteral("/下载")}));
    }
    void watcherStartFailureIsPersistent() {
        auto settings = makeAppSettings();
        settings.setValue("index/roots", QStringList{QString {}});
        MainWindow window;
        QVERIFY(!window.watcher_error_.isEmpty());
        const auto error = window.watcher_error_;
        window.refreshStats();
        window.search_edit_->setText("alpha");
        window.runSearch();
        QTRY_COMPARE(window.model_->rowCount(), 1);
        QCOMPARE(window.watcher_error_label_->text(), error);
        QVERIFY(!window.watcher_error_label_->isHidden());
    }
#endif
    void obsoleteQueuedWatcherEventsAreIgnored() {
        MainWindow window;
        const auto old_generation = window.watcher_generation_;
        QMetaObject::invokeMethod(
            &window,
            [&window, old_generation] {
                window.receiveWatcherEvent(old_generation, FileEvent{"", false, "obsolete watcher failure"});
            },
            Qt::QueuedConnection);
        window.restartWatcher();
        const auto current_error = window.watcher_error_;
        QTest::qWait(50);
        QCOMPARE(window.watcher_error_, current_error);
        window.receiveWatcherEvent(window.watcher_generation_,
                                   FileEvent{"", false, "current watcher failure"});
        QCOMPARE(window.watcher_error_, QStringLiteral("current watcher failure"));
    }
    void watcherRecoveryClearsCurrentError() {
        MainWindow window;
        window.scheduleFileEvent(FileEvent{"", false, "synthetic coverage failure"});
        QVERIFY(!window.watcher_error_.isEmpty());
        window.scheduleFileEvent(FileEvent{fixture_root.toStdString(), true, {}});
        QVERIFY(window.watcher_error_.isEmpty());
        QCOMPARE(window.watcher_error_label_->text(), QStringLiteral("目录监听正常"));
    }
    void watcherErrorSurvivesStatisticsAndSearch() {
        MainWindow window;
#ifdef Q_OS_LINUX
        // Consume the real initial coverage-success notification before
        // injecting an error: a later success legitimately clears it.
        QTRY_VERIFY(window.pending_rescan_roots_.contains(fixture_root));
#endif
        window.scheduleFileEvent(FileEvent{"", false, "synthetic watcher failure"});
        QCOMPARE(window.watcher_error_, QStringLiteral("synthetic watcher failure"));
        window.refreshStats();
        window.search_edit_->setText("alpha");
        window.runSearch();
        QTRY_COMPARE(window.model_->rowCount(), 1);
        QCOMPARE(window.watcher_error_label_->text(), QStringLiteral("目录监听异常"));
        QCOMPARE(window.watcher_error_label_->toolTip(), QStringLiteral("synthetic watcher failure"));
        QVERIFY(!window.watcher_error_label_->isHidden());
    }
    void cliAndGuiShareExclusionPolicy() {
        MainWindow window;
        auto cli=[&](const QStringList& args) {
            QProcess process;
            QStringList command{QStringLiteral("--db"),window.db_path_}; command.append(args);
            process.start(QString::fromUtf8(EVERYTHING_LITE_CLI_PATH),command);
            if(!process.waitForFinished(10000) || process.exitStatus()!=QProcess::NormalExit) return -1;
            return process.exitCode();
        };
        QCOMPARE(cli({"excludes","set","--file",fixture_root+"/alpha.txt"}),0);
        Database db(window.db_path_.toStdString());
        QCOMPARE(db.exclusionPolicy().rules().files.size(),std::size_t(1));
        window.rebuildIndex(); QTRY_VERIFY_WITH_TIMEOUT(!window.worker_thread_,10000);
        window.search_edit_->setText("alpha"); window.runSearch();
        QTRY_VERIFY(!window.search_in_flight_); QCOMPARE(window.model_->rowCount(),0);
        QVERIFY(db.search("alpha").empty());
        QCOMPARE(cli({"excludes","clear"}),0);
        QVERIFY(db.search("alpha").empty()); // loosening alone cannot resurrect rows
        window.rebuildIndex(); QTRY_VERIFY_WITH_TIMEOUT(!window.worker_thread_,10000);
        window.runSearch(); QTRY_COMPARE(window.model_->rowCount(),1);
        QCOMPARE(window.model_->nameAt(0),QStringLiteral("alpha.txt"));
    }
    void exclusionSettingsValidatesAndSavesRegex() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto root=dir.path()+"/root"; QVERIFY(QDir().mkpath(root));
        for(const auto& name:{QStringLiteral("alpha.txt"),QStringLiteral("beta.txt")}) {
            QFile file(root+"/"+name); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("synthetic");
        }
        const auto db_path=dir.path()+"/index.db";
        IndexManager manager(db_path.toStdString()); QVERIFY(manager.rebuildRoot(root.toStdString()).completed);
        auto settings = makeAppSettings(); settings.setValue("index/roots",QStringList{root});
        const auto old=qgetenv("EVERYTHING_LITE_DB"); qputenv("EVERYTHING_LITE_DB",db_path.toUtf8());
        MainWindow window; qputenv("EVERYTHING_LITE_DB",old); window.watcher_->stop();
        QAction* action=nullptr;
        for(auto* candidate:window.findChildren<QAction*>())
            if(candidate->text()==QStringLiteral("排除规则…")) action=candidate;
        QVERIFY(action); action->trigger();
        auto* dialog=window.findChild<QDialog*>(QStringLiteral("exclusionSettingsDialog"));
        QVERIFY(dialog); QVERIFY(dialog->isVisible());
        auto* filter=dialog->findChild<QPlainTextEdit*>(QStringLiteral("fileFilterEdit"));
        QVERIFY(filter); filter->setPlainText(QStringLiteral("regex:^alpha\\.txt$"));
        QPointer<QDialog> saved(dialog);
        dialog->accept();
        QTRY_VERIFY_WITH_TIMEOUT(manager.database().exclusionPolicy().revision>0,10000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.worker_thread_,10000);
        QTRY_VERIFY(!saved || !saved->isVisible());
        QCOMPARE(manager.database().search("alpha").size(),std::size_t(0));
        QCOMPARE(manager.database().search("beta").size(),std::size_t(1));
        action->trigger();
        auto* invalid=window.findChild<QDialog*>(QStringLiteral("exclusionSettingsDialog"));
        QVERIFY(invalid); auto* invalid_filter=invalid->findChild<QPlainTextEdit*>(QStringLiteral("fileFilterEdit"));
        QVERIFY(invalid_filter); invalid_filter->setPlainText(QStringLiteral("regex:("));
        const auto revision=manager.database().exclusionPolicy().revision;
        invalid->accept();
        QVERIFY(invalid->isVisible());
        QVERIFY(!invalid->findChild<QLabel*>(QStringLiteral("exclusionError"))->text().isEmpty());
        QCOMPARE(manager.database().exclusionPolicy().revision,revision);
        invalid->reject();
    }
    void exclusionSettingsAcceptsTypedPathsWithoutFilesystemPicker() {
        ExclusionSettingsDialog dialog({});
        auto* directory=dialog.findChild<QLineEdit*>(QStringLiteral("excludedDirectoryInput"));
        auto* file=dialog.findChild<QLineEdit*>(QStringLiteral("excludedFileInput"));
        QVERIFY(directory && file);
        directory->setText(QStringLiteral("/synthetic/offline-folder"));
        QTest::keyClick(directory,Qt::Key_Return);
        file->setText(QStringLiteral("/synthetic/offline-file.txt"));
        QTest::keyClick(file,Qt::Key_Return);
        const auto rules=dialog.rules();
        QCOMPARE(rules.directories,std::vector<std::string>{"/synthetic/offline-folder"});
        QCOMPARE(rules.files,std::vector<std::string>{"/synthetic/offline-file.txt"});
        auto* app_contents=dialog.findChild<QCheckBox*>(QStringLiteral("indexAppBundleContents"));
        QVERIFY(app_contents && !app_contents->isChecked());
        app_contents->setChecked(true);
        QVERIFY(dialog.rules().index_app_bundle_contents);
    }
    void exclusionSettingsShowsSaveProgress() {
        ExclusionSettingsDialog dialog({}); dialog.show();
        dialog.setSaving(true);
        auto* progress=dialog.findChild<QLabel*>(QStringLiteral("exclusionSaving"));
        QVERIFY(progress && progress->isVisible());
        QVERIFY(progress->text().contains(QStringLiteral("正在保存")));
        auto* bar=dialog.findChild<QProgressBar*>(QStringLiteral("exclusionProgressBar"));
        QVERIFY(bar && bar->isVisible());
        dialog.setSaveProgress(3, 10, 2);
        QCOMPARE(bar->value(),28);
        QVERIFY(progress->text().contains(QStringLiteral("3 / 10")));
        dialog.setSaving(false);
        QVERIFY(progress->isHidden());
        QVERIFY(bar->isHidden());
    }
    void exclusionSettingsKeepsEditsWhenIndexing() {
        MainWindow window; window.watcher_->stop();
        QAction* action=nullptr;
        for(auto* candidate:window.findChildren<QAction*>())
            if(candidate->text()==QStringLiteral("排除规则…")) action=candidate;
        QVERIFY(action); action->trigger();
        auto* dialog=window.findChild<QDialog*>(QStringLiteral("exclusionSettingsDialog"));
        QVERIFY(dialog);
        auto* filter=dialog->findChild<QPlainTextEdit*>(QStringLiteral("fileFilterEdit"));
        QVERIFY(filter);filter->setPlainText(QStringLiteral("*.scratch"));
        WorkerGate gate;
        window.startWorker([started=gate.started,release=gate.release]{
            started->release();release->acquire();
        },QStringLiteral("synthetic indexing"));
        QVERIFY(gate.started->tryAcquire(1,3000));
        dialog->accept();
        QVERIFY(dialog->isVisible());
        QCOMPARE(filter->toPlainText(),QStringLiteral("*.scratch"));
        QVERIFY(!dialog->findChild<QLabel*>(QStringLiteral("exclusionError"))->text().isEmpty());
        gate.release->release();
        QTRY_VERIFY_WITH_TIMEOUT(!window.worker_thread_,3000);
        dialog->reject();
    }
};
} // namespace everything_lite
int main(int argc, char** argv) {
    QTemporaryDir sandbox;
    if (!sandbox.isValid())
        return 2;
    qputenv("HOME", sandbox.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", (sandbox.path() + "/config").toUtf8());
    qputenv("XDG_DATA_HOME", (sandbox.path() + "/data").toUtf8());
    qputenv("XDG_CACHE_HOME", (sandbox.path() + "/cache").toUtf8());
    qputenv("EVERYTHING_LITE_DB", (sandbox.path() + "/test.db").toUtf8());
    qputenv("EVERYTHING_LITE_TEST_SETTINGS_FILE", (sandbox.path() + "/settings.ini").toUtf8());
#ifdef Q_OS_LINUX
    QDir().mkpath(sandbox.path() + "/config");
    QFile dirs(sandbox.path() + "/config/user-dirs.dirs");
    if (!dirs.open(QIODevice::WriteOnly))
        return 2;
    dirs.write(QStringLiteral("XDG_DESKTOP_DIR=\"$HOME/桌面\"\nXDG_DOCUMENTS_DIR=\"$HOME/"
                              "文档\"\nXDG_DOWNLOAD_DIR=\"$HOME/下载\"\n")
                   .toUtf8());
    dirs.close();
    for (const auto& name : {QStringLiteral("桌面"), QStringLiteral("文档"), QStringLiteral("下载")})
        QDir().mkpath(sandbox.path() + "/" + name);
#endif
    QApplication app(argc, argv);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    const auto root = sandbox.path() + "/root";
    QDir().mkpath(root + "/needle");
    auto write = [&](const QString& name) {
        QFile f(root + "/" + name);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write("sample");
        return true;
    };
    if (!write("alpha.txt") || !write("beta.txt") || !write(QStringLiteral("中文.txt")) ||
        !write("needle/opaque.txt"))
        return 2;
    for (int i = 0; i < 1005; ++i)
        if (!write(QStringLiteral("item%1.txt").arg(i, 4, 10, QChar('0'))))
            return 2;
    auto settings = everything_lite::makeAppSettings();
    if (settings.format() != QSettings::IniFormat || settings.fileName() != sandbox.path() + "/settings.ini") {
        qCritical() << "Refusing to run UI tests with non-isolated settings:" << settings.fileName();
        return 2;
    }
    settings.setValue("index/roots", QStringList{root});
    settings.sync();
    everything_lite::IndexManager manager((sandbox.path() + "/test.db").toStdString());
    manager.rebuildRoots({root.toStdString()});
    everything_lite::UiTest test;
    test.fixture_root = root;
    return QTest::qExec(&test, argc, argv);
}
#include "test_ui.moc"
