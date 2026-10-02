#include "ui/main_window.h"
#include "ui/search_result_model.h"
#include "ui/search_worker.h"
#include "core/index_manager.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSemaphore>
#include <QSettings>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
namespace everything_lite {
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
