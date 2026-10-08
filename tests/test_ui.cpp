#include "ui/main_window.h"
#include "ui/search_result_model.h"
#include "ui/search_worker.h"
#include "core/index_manager.h"
#include <QAction>
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
#include <QFile>
#include <QFileDialog>
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
    bool block(SearchWorker* worker) {
        QMetaObject::invokeMethod(worker,[s=started,r=release] {s->release();r->acquire();},Qt::QueuedConnection);
        return started->tryAcquire(1,3000);
    }
    ~WorkerGate() {release->release();}
};
class UiTest : public QObject {
    Q_OBJECT
  public:
    QString fixture_root;
  private slots:
    void init() {
        QSettings settings("CICHI", "EverythingLite");
        settings.setValue("index/roots", QStringList{fixture_root});
        settings.setValue("search/itemScope", 0);
        settings.setValue("search/matchPath", false);
    }
    void cachedSourceColumns() {
        SearchResultModel model;
        SearchResult unknown; unknown.file.name="cache.txt"; unknown.file.path="/unreachable/cache.txt";
        model.setResults({unknown});
        QCOMPARE(model.columnCount(), 6);
        QVERIFY(model.data(model.index(0,4)).toString().contains(QStringLiteral("未分类")));
        QVERIFY(model.data(model.index(0,5)).toString().contains(QStringLiteral("未核验")));
        auto offline=unknown; offline.source_label="NAS"; offline.source_type=SourceType::NAS;
        offline.source_status=SourceStatus::Offline; offline.source_last_success=1700000000;
        model.setResults({offline});
        QVERIFY(model.data(model.index(0,5)).toString().contains(QStringLiteral("离线缓存")));
        QVERIFY(model.data(model.index(0,5),Qt::ToolTipRole).toString().contains(QStringLiteral("上次可用")));
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
        window.scheduleFilesystemResultRefresh();
        QTest::qWait(1900);
        QCOMPARE(window.search_request_id_,id);
        QVERIFY(window.search_in_flight_);
        release.release(); QTRY_COMPARE(window.model_->nameAt(0),QStringLiteral("alpha.txt"));
    }
    void continuousResultChangesRefreshBeforeQuiet() {
        MainWindow window; window.watcher_->stop();
        window.search_edit_->setText("alpha"); window.runSearch(); QTRY_COMPARE(window.model_->rowCount(),1);
        const auto id=window.search_request_id_;
        QTimer changes; changes.setInterval(100);
        connect(&changes,&QTimer::timeout,&window,[&]{window.scheduleFilesystemResultRefresh();});
        changes.start(); window.scheduleFilesystemResultRefresh();
        QTRY_VERIFY_WITH_TIMEOUT(window.search_request_id_>id,2400);
        QVERIFY(changes.isActive()); changes.stop();
        QTRY_VERIFY(!window.search_in_flight_);
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
        QVERIFY2(window.statusBar()->currentMessage().contains(QStringLiteral("同步")),"Incomplete snapshots must expose a synchronization error");
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
        window.scheduleFileEvent({nested.toStdString(),false,{}});
        window.scheduleFileEvent({root.toStdString(),false,{}});
        QCOMPARE(window.pending_paths_.size(),2);
        QVERIFY(window.pending_paths_.contains(root)); QVERIFY(window.pending_paths_.contains(nested));
    }
    void editsCancelAndCompletionHonorsDebounce() {
        MainWindow window; window.watcher_->stop();
        window.search_edit_->setText("alpha"); window.runSearch(); QTRY_COMPARE(window.model_->rowCount(),1);
        WorkerGate gate; QVERIFY(gate.block(window.search_worker_));
        window.runSearch(); const auto token=window.active_search_cancellation_; QVERIFY(token);
        window.search_edit_->selectAll(); QTest::keyClicks(window.search_edit_,"beta");
        QVERIFY(token->load()); QTest::qWait(850); QVERIFY(window.search_pending_);
        const auto dispatchedBeforeEditing=window.last_search_request_id_;
        window.search_edit_->selectAll(); QTest::keyClicks(window.search_edit_,"alpha");
        const auto id=window.search_request_id_;
        gate.release->release(); QTRY_VERIFY_WITH_TIMEOUT(!window.search_in_flight_,400);
        QVERIFY(window.search_timer_->isActive());
        QCOMPARE(window.last_search_request_id_,dispatchedBeforeEditing);
        QTest::qWait(200); QVERIFY(!window.search_in_flight_);
        QTRY_VERIFY_WITH_TIMEOUT(window.last_search_request_id_>id,2000);
        QTRY_COMPARE(window.model_->nameAt(0),QStringLiteral("alpha.txt"));
    }
    void imeCancelsWithoutPrematureQueuedSearch() {
        MainWindow window; window.watcher_->stop();
        window.search_edit_->setText("alpha"); window.runSearch(); QTRY_COMPARE(window.model_->rowCount(),1);
        WorkerGate gate; QVERIFY(gate.block(window.search_worker_));
        window.runSearch(); const auto token=window.active_search_cancellation_;
        window.search_edit_->selectAll(); QTest::keyClicks(window.search_edit_,"beta"); QTest::qWait(850);
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
        QVERIFY(window.sync_error_label_->text().contains(QStringLiteral("保留缓存")));
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
    void sixColumnSortAndSettingsMigration() {
        QStandardItemModel old(0,4); QTableView oldTable; oldTable.setModel(&old);
        oldTable.setColumnWidth(0,222);
        QSettings settings("CICHI","EverythingLite"); settings.setValue("table/header",oldTable.horizontalHeader()->saveState());
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
                dialog->selectFile(output); QMetaObject::invokeMethod(dialog,"accept",Qt::DirectConnection); accepted=true; chooser.stop(); return;
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
        QSettings settings("CICHI", "EverythingLite");
        settings.remove("index/roots");
        MainWindow window;
        QCOMPARE(window.roots_, (QStringList{QDir::homePath() + QStringLiteral("/桌面"),
                                             QDir::homePath() + QStringLiteral("/文档"),
                                             QDir::homePath() + QStringLiteral("/下载")}));
    }
    void watcherStartFailureIsPersistent() {
        QSettings settings("CICHI", "EverythingLite");
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
        QVERIFY(window.watcher_error_label_->isHidden());
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
        QCOMPARE(window.watcher_error_label_->text(), QStringLiteral("synthetic watcher failure"));
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
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, sandbox.path() + "/settings");
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, sandbox.path() + "/system");
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
    QSettings settings("CICHI", "EverythingLite");
    settings.setValue("index/roots", QStringList{root});
    settings.sync();
    everything_lite::IndexManager manager((sandbox.path() + "/test.db").toStdString());
    manager.rebuildRoots({root.toStdString()});
    everything_lite::UiTest test;
    test.fixture_root = root;
    return QTest::qExec(&test, argc, argv);
}
#include "test_ui.moc"
