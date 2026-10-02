#include "ui/main_window.h"

#include "core/app_paths.h"
#include "core/database.h"
#include "core/index_manager.h"
#include "core/path_utils.h"
#include "core/search_trace.h"
#include "core/search_query.h"
#include "platform/watcher_factory.h"
#include "ui/root_settings_dialog.h"
#include "ui/desktop_actions.h"
#include "ui/file_preview.h"
#include "ui/search_result_model.h"
#include "ui/search_worker.h"

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QInputMethodEvent>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QProcess>
#include <QPointer>
#include <QScrollBar>
#include <QSettings>
#include <QStringConverter>
#include <QShortcut>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableView>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <sstream>
#include <utility>
#include <vector>

namespace everything_lite {
namespace {

std::string toUtf8(const QString& value) {
    const auto bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString fromUtf8(const std::string& value) {
    return QString::fromUtf8(value.c_str(), static_cast<int>(value.size()));
}

QString normalizeGuiQuery(QString value) {
    // Keep GUI and CLI semantics aligned while removing invisible formatting
    // characters that can be introduced by IMEs or copy/paste. Chinese text
    // itself is preserved; NFC also makes equivalent Unicode input stable.
    value = value.normalized(QString::NormalizationForm_C);
    value.remove(QChar(0x200B)); // ZERO WIDTH SPACE
    value.remove(QChar(0xFEFF)); // ZERO WIDTH NO-BREAK SPACE / BOM
    value.remove(QChar(0x2060)); // WORD JOINER
    return value.trimmed();
}

QString utf8Hex(const QString& value) {
    return QString::fromLatin1(value.toUtf8().toHex(' '));
}

QString cliProbePath() {
    QDir dir(QFileInfo(QCoreApplication::applicationFilePath()).absolutePath());
#ifdef Q_OS_MACOS
    return QDir::cleanPath(dir.absoluteFilePath(QStringLiteral("../../../everything-lite-cli")));
#else
    return QDir::cleanPath(dir.absoluteFilePath(QStringLiteral("everything-lite-cli")));
#endif
}

void runCliProbe(const QString& db_path, const QString& query, std::size_t limit, std::size_t offset) {
    if (!searchTraceEnabled() || query.isEmpty()) return;
    const auto cli = cliProbePath();
    searchTrace("GUI-PROBE", "candidate_cli=\"" + toUtf8(cli) + "\"");
    if (!QFileInfo::exists(cli)) {
        searchTrace("GUI-PROBE", "CLI probe skipped: executable not found");
        return;
    }

    QStringList args;
    args << QStringLiteral("--trace-search")
         << QStringLiteral("--db") << db_path
         << QStringLiteral("probe-search") << query
         << QStringLiteral("--limit") << QString::number(static_cast<qulonglong>(limit))
         << QStringLiteral("--offset") << QString::number(static_cast<qulonglong>(offset));

    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(cli, args);
    if (!process.waitForStarted(5000)) {
        searchTrace("GUI-PROBE", "CLI probe failed to start: " + toUtf8(process.errorString()));
        return;
    }
    if (!process.waitForFinished(60000)) {
        searchTrace("GUI-PROBE", "CLI probe timed out after 60s; killing child");
        process.kill();
        process.waitForFinished(3000);
        return;
    }
    searchTrace("GUI-PROBE", "CLI exit_code=" + std::to_string(process.exitCode()));
    const auto stdout_text = process.readAllStandardOutput().toStdString();
    const auto stderr_text = process.readAllStandardError().toStdString();
    if (!stdout_text.empty()) searchTrace("GUI-PROBE-STDOUT", "\n" + stdout_text);
    if (!stderr_text.empty()) searchTrace("GUI-PROBE-STDERR", "\n" + stderr_text);
}

QStringList defaultRoots() {
    QStringList roots;
    for (const auto location : {QStandardPaths::DesktopLocation,QStandardPaths::DocumentsLocation,QStandardPaths::DownloadLocation}) {
        const auto writable_path = QStandardPaths::writableLocation(location);
        if (writable_path.isEmpty()) continue;
        const auto path = QDir::cleanPath(writable_path);
        if (QFileInfo(path).isDir() && !roots.contains(path)) roots << path;
    }
    if (roots.isEmpty()) roots << QDir::homePath();
    return roots;
}

QString csvQuote(QString value) {
    value.replace('"', QStringLiteral("\"\""));
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}

QString bookmarkDefaultName(const QString& query) {
    const auto trimmed = query.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("全部") : trimmed.left(48);
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    // v0.4.1: GUI and CLI share the same resolver. The resolver also prefers
    // an existing v0.4 database so upgrading never creates a silent empty DB.
    db_path_ = fromUtf8(defaultDatabasePath());
    QDir().mkpath(QFileInfo(db_path_).absolutePath());
    searchTrace("GUI", "MainWindow db=\"" + toUtf8(db_path_) + "\"");

    buildUi();
    desktop_actions_ = new DesktopActions(this);
    connect(desktop_actions_, &DesktopActions::feedback, this, [this](const QString& message) {
        statusBar()->showMessage(message, 10000);
    });
    connect(desktop_actions_, &DesktopActions::previewFallback, this, [this](const QString& path) {
        (new FilePreview(path, this))->show();
    });

    buildMenus();
    loadSettings();
    if (searchTraceEnabled()) {
        searchTrace("GUI", "settings scope=" + std::to_string(scope_combo_->currentIndex())
            + " match_path=" + std::to_string(match_path_check_->isChecked() ? 1 : 0)
            + " roots=" + std::to_string(roots_.size()));
        for (int i = 0; i < roots_.size(); ++i) {
            searchTrace("GUI", "root[" + std::to_string(i) + "]=\"" + toUtf8(roots_.at(i)) + "\"");
        }
    }
    loadBookmarks();
    rebuildBookmarksMenu();
    updateScopeLabel();
    updateSearchStateLabel();

    // v0.4.11: all SQLite searching lives on a dedicated worker thread.
    // SearchWorker lazily creates SearchService on that thread, so even the
    // first-time database/FTS initialization cannot stall the GUI event loop.
    search_thread_ = new QThread(this);
    search_worker_ = new SearchWorker(toUtf8(db_path_));
    search_worker_->moveToThread(search_thread_);
    connect(search_thread_, &QThread::finished, search_worker_, &QObject::deleteLater);
    search_thread_->start();

    search_timer_ = new QTimer(this);
    search_timer_->setSingleShot(true);
    search_timer_->setInterval(kSearchIdleDelayMs);
    connect(search_timer_, &QTimer::timeout, this, [this] {
        // v0.4.9: never start a search while an IME composition is active.
        // The timer is restarted after the composition is committed.
        if (search_ime_composing_) return;
        runSearch();
    });

    // Search only after the user has stopped editing for a short idle window.
    // textEdited() intentionally ignores programmatic setText() calls (for
    // example when applying a bookmark), which already trigger their own
    // explicit search. Every keystroke restarts the timer.
    connect(search_edit_, &QLineEdit::textEdited, this, [this](const QString&) {
        // Invalidate any result already being computed as soon as the user
        // changes the text. The running query is allowed to finish off-thread,
        // but its stale result will never replace the current view.
        ++search_request_id_;
        results_have_more_ = false;
        search_timer_->stop();
        status_label_->setText(QStringLiteral("输入中… 停止输入 %1 ms 后后台搜索").arg(kSearchIdleDelayMs));
        if (!search_ime_composing_) {
            search_timer_->start(kSearchIdleDelayMs);
        }
    });
    search_edit_->installEventFilter(this);

    connect(search_edit_, &QLineEdit::returnPressed, this, [this] {
        // Enter remains the explicit immediate-search action, but an Enter used
        // by the IME to choose/commit a candidate must not search preedit text.
        if (search_ime_composing_) return;
        search_timer_->stop();
        runSearch();
    });
    connect(scope_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (scope_action_group_) {
            if (index == 0) scope_all_action_->setChecked(true);
            else if (index == 1) scope_files_action_->setChecked(true);
            else if (index == 2) scope_folders_action_->setChecked(true);
        }
        ++search_request_id_;
        updateSearchStateLabel();
        search_timer_->start(kSearchIdleDelayMs);
    });
    connect(match_path_check_, &QCheckBox::toggled, this, [this](bool checked) {
        if (match_path_action_ && match_path_action_->isChecked() != checked) match_path_action_->setChecked(checked);
        ++search_request_id_;
        updateSearchStateLabel();
        search_timer_->start(kSearchIdleDelayMs);
    });

    event_timer_ = new QTimer(this);
    event_timer_->setSingleShot(true);
    event_timer_->setInterval(700);
    connect(event_timer_, &QTimer::timeout, this, &MainWindow::processPendingEvents);

    // v0.4.2: filesystem churn must never pre-empt a user search. FSEvents can
    // fire continuously under ~/Library; v0.4.1 restarted the foreground query
    // after every sync batch, so slower queries could be cancelled forever.
    // We refresh the visible result set only after the filesystem has been quiet
    // for a while, and never cancel a query that is already running.
    filesystem_refresh_timer_ = new QTimer(this);
    filesystem_refresh_timer_->setSingleShot(true);
    filesystem_refresh_timer_->setInterval(1800);
    connect(filesystem_refresh_timer_, &QTimer::timeout, this, [this] {
        // v0.4.10: filesystem refresh must never compete with typing. Do not
        // issue an expensive empty query and do not refresh while the user's
        // debounce timer or IME composition is active.
        if (search_ime_composing_ || (search_timer_ && search_timer_->isActive())) {
            filesystem_refresh_timer_->start(1800);
            return;
        }
        if (normalizeGuiQuery(search_edit_->text()).isEmpty()) return;
        runSearch();
    });

    connect(table_, &QTableView::doubleClicked, this, [this] { openCurrent(); });
    connect(table_, &QWidget::customContextMenuRequested, this, &MainWindow::showContextMenu);
    connect(table_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        auto* bar = table_->verticalScrollBar();
        if (results_have_more_ && value >= bar->maximum() - 8) loadMoreResults();
    });

    auto* open_shortcut = new QShortcut(QKeySequence(Qt::Key_Return), table_);
    connect(open_shortcut, &QShortcut::activated, this, &MainWindow::openCurrent);

    refreshStats();
    restartWatcher();

    // v0.3+ upgrades an existing v0.2 database in-place. Building the trigram
    // index reads the existing SQLite rows only; it does not rescan the disk.
    if (indexed_count_ > 0 && name_search_available_ && !name_search_ready_) {
        const auto db_path = db_path_;
        startWorker([db_path] {
            Database db(toUtf8(db_path));
            db.initialize();
            db.rebuildNameSearchIndex();
        }, QStringLiteral("首次升级：正在从现有数据库建立名称加速索引（无需重新扫描磁盘）…"));
    }

    // v0.4.10: do not synchronously run an empty 1001-row search at startup.
    // That query used to overlap with the user's first keystrokes and was the
    // main source of the "first input" hitch. The first real query is started
    // only after the user finishes typing (or presses Enter).
    search_edit_->setFocus();
    model_->setResults({});
    status_label_->setText(QStringLiteral("%1 个对象    ·    输入关键词开始搜索    ·    %2")
        .arg(indexed_count_)
        .arg(watcher_ ? fromUtf8(watcher_->backendName()) : QStringLiteral("未监听")));
}

MainWindow::~MainWindow() {
    if (watcher_) watcher_->stop();
    if (search_thread_) {
        search_thread_->quit();
        search_thread_->wait();
        search_worker_ = nullptr;
        search_thread_ = nullptr;
    }
    if (worker_thread_) {
        worker_thread_->quit();
        worker_thread_->wait();
        delete worker_thread_;
        worker_thread_ = nullptr;
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveSettings();
    saveBookmarks();
    QMainWindow::closeEvent(event);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == search_edit_ && event && event->type() == QEvent::InputMethod) {
        auto* input_event = static_cast<QInputMethodEvent*>(event);
        const bool composing_now = !input_event->preeditString().isEmpty();

        if (composing_now) {
            ++search_request_id_;
            search_ime_composing_ = true;
            if (search_timer_) search_timer_->stop();
            if (searchTraceEnabled()) {
                searchTrace("GUI-INPUT", "IME preedit active; search postponed");
            }
        } else if (search_ime_composing_) {
            // This event commits or cancels the composition. The QLineEdit will
            // process the commit after this filter returns; the delayed timer
            // therefore sees the final committed text, never the preedit text.
            search_ime_composing_ = false;
            if (search_timer_) search_timer_->start(kSearchIdleDelayMs);
            if (searchTraceEnabled()) {
                searchTrace("GUI-INPUT", "IME composition finished; idle timer started");
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::buildUi() {
    setWindowTitle(searchTraceEnabled()
        ? QStringLiteral("Everything Lite %1 [SEARCH TRACE]").arg(QStringLiteral(EVERYTHING_LITE_VERSION))
        : QStringLiteral("Everything Lite %1").arg(QStringLiteral(EVERYTHING_LITE_VERSION)));
    resize(1160, 740);
    setMinimumSize(820, 480);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(10, 8, 10, 6);
    layout->setSpacing(6);

    auto* search_row = new QHBoxLayout();
    search_row->setSpacing(6);

    search_edit_ = new QLineEdit(this);
    search_edit_->setPlaceholderText(QStringLiteral("搜索文件和文件夹…  例：report  ext:pdf  path:sample  size:>10m  modified:7d"));
    search_edit_->setClearButtonEnabled(true);
    search_edit_->setMinimumHeight(34);

    scope_combo_ = new QComboBox(this);
    scope_combo_->addItem(QStringLiteral("全部"));
    scope_combo_->addItem(QStringLiteral("文件"));
    scope_combo_->addItem(QStringLiteral("文件夹"));
    scope_combo_->setMinimumHeight(32);
    scope_combo_->setMinimumWidth(90);
    scope_combo_->setToolTip(QStringLiteral("Everything 风格过滤：全部 / 文件 / 文件夹"));

    match_path_check_ = new QCheckBox(QStringLiteral("匹配路径"), this);
    match_path_check_->setToolTip(QStringLiteral("关闭时普通关键词只匹配名称；打开后匹配完整路径"));
    match_path_check_->hide(); // Controlled from Search menu by default, like Everything.

    search_row->addWidget(search_edit_, 1);
    search_row->addWidget(scope_combo_);
    search_row->addWidget(match_path_check_);
    layout->addLayout(search_row);

    scope_label_ = new QLabel(this);
    scope_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto scope_font = scope_label_->font();
    scope_font.setPointSizeF(std::max(9.0, scope_font.pointSizeF() - 1.0));
    scope_label_->setFont(scope_font);
    layout->addWidget(scope_label_);

    model_ = new SearchResultModel(this);
    table_ = new QTableView(this);
    table_->setModel(model_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setAlternatingRowColors(true);
    table_->setSortingEnabled(true);
    table_->sortByColumn(0, Qt::AscendingOrder);
    table_->setShowGrid(false);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(25);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionsMovable(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    table_->setColumnWidth(0, 310);
    table_->setColumnWidth(2, 92);
    table_->setColumnWidth(3, 155);
    layout->addWidget(table_, 1);

    setCentralWidget(central);

    status_label_ = new QLabel(this);
    search_state_label_ = new QLabel(this);
    search_state_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    statusBar()->addWidget(status_label_, 1);
    watcher_error_label_ = new QLabel(this);
    watcher_error_label_->setTextFormat(Qt::PlainText);
    watcher_error_label_->setWordWrap(true);
    watcher_error_label_->setStyleSheet(QStringLiteral("color: #b33"));
    watcher_error_label_->hide();
    statusBar()->addPermanentWidget(watcher_error_label_);
    statusBar()->addPermanentWidget(search_state_label_);
    statusBar()->setSizeGripEnabled(false);
}

void MainWindow::buildMenus() {
    auto* file_menu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    auto* new_window = file_menu->addAction(QStringLiteral("新建搜索窗口"));
    new_window->setShortcut(QKeySequence::New);
    connect(new_window, &QAction::triggered, this, &MainWindow::newWindow);

    file_menu->addSeparator();
    auto* open = file_menu->addAction(QStringLiteral("打开"));
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, &MainWindow::openCurrent);

    auto* reveal = file_menu->addAction(QStringLiteral("打开所在位置"));
#ifdef Q_OS_MACOS
    reveal->setText(QStringLiteral("在 Finder 中显示"));
#endif
    connect(reveal, &QAction::triggered, this, &MainWindow::revealCurrent);

    auto* open_with = file_menu->addAction(QStringLiteral("打开方式…"));
    connect(open_with, &QAction::triggered, this, &MainWindow::openWithCurrent);

    auto* quick_look = file_menu->addAction(QStringLiteral("快速查看"));
    quick_look->setShortcut(QKeySequence(Qt::Key_Space));
    connect(quick_look, &QAction::triggered, this, &MainWindow::quickLookCurrent);

    file_menu->addSeparator();
    auto* export_action = file_menu->addAction(QStringLiteral("导出已加载结果…"));
    export_action->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    connect(export_action, &QAction::triggered, this, &MainWindow::exportLoadedResults);

    file_menu->addSeparator();
    auto* close_action = file_menu->addAction(QStringLiteral("关闭窗口"));
    close_action->setShortcut(QKeySequence::Close);
    connect(close_action, &QAction::triggered, this, &QWidget::close);

    auto* quit_action = file_menu->addAction(QStringLiteral("退出 Everything Lite"));
    quit_action->setShortcut(QKeySequence::Quit);
    quit_action->setMenuRole(QAction::QuitRole);
    connect(quit_action, &QAction::triggered, qApp, &QApplication::quit);

    auto* edit_menu = menuBar()->addMenu(QStringLiteral("编辑(&E)"));
    auto* copy_path = edit_menu->addAction(QStringLiteral("复制完整路径"));
    copy_path->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    connect(copy_path, &QAction::triggered, this, &MainWindow::copyCurrentPath);

    auto* copy_name = edit_menu->addAction(QStringLiteral("复制名称"));
    connect(copy_name, &QAction::triggered, this, &MainWindow::copyCurrentName);

    edit_menu->addSeparator();
    auto* select_all = edit_menu->addAction(QStringLiteral("全选"));
    select_all->setShortcut(QKeySequence::SelectAll);
    connect(select_all, &QAction::triggered, table_, &QTableView::selectAll);

    auto* focus_search = edit_menu->addAction(QStringLiteral("转到搜索框"));
    focus_search->setShortcut(QKeySequence::Find);
    connect(focus_search, &QAction::triggered, this, [this] {
        search_edit_->setFocus();
        search_edit_->selectAll();
    });

    auto* view_menu = menuBar()->addMenu(QStringLiteral("查看(&V)"));
    auto* refresh_action = view_menu->addAction(QStringLiteral("刷新结果"));
    refresh_action->setShortcut(QKeySequence::Refresh);
    connect(refresh_action, &QAction::triggered, this, &MainWindow::runSearch);

    view_menu->addSeparator();
    show_status_bar_action_ = view_menu->addAction(QStringLiteral("状态栏"));
    show_status_bar_action_->setCheckable(true);
    show_status_bar_action_->setChecked(true);
    connect(show_status_bar_action_, &QAction::toggled, statusBar(), &QStatusBar::setVisible);

    show_scope_bar_action_ = view_menu->addAction(QStringLiteral("索引范围栏"));
    show_scope_bar_action_->setCheckable(true);
    show_scope_bar_action_->setChecked(true);
    connect(show_scope_bar_action_, &QAction::toggled, scope_label_, &QWidget::setVisible);

    show_filter_bar_action_ = view_menu->addAction(QStringLiteral("过滤器栏"));
    show_filter_bar_action_->setCheckable(true);
    show_filter_bar_action_->setChecked(true);
    connect(show_filter_bar_action_, &QAction::toggled, scope_combo_, &QWidget::setVisible);

    view_menu->addSeparator();
    auto* reset_columns_action = view_menu->addAction(QStringLiteral("重置列布局"));
    connect(reset_columns_action, &QAction::triggered, this, &MainWindow::resetColumns);

    auto* search_menu = menuBar()->addMenu(QStringLiteral("搜索(&S)"));
    match_path_action_ = search_menu->addAction(QStringLiteral("匹配路径"));
    match_path_action_->setCheckable(true);
    connect(match_path_action_, &QAction::toggled, this, [this](bool checked) {
        if (match_path_check_->isChecked() != checked) match_path_check_->setChecked(checked);
        updateSearchStateLabel();
    });

    search_menu->addSeparator();
    scope_action_group_ = new QActionGroup(this);
    scope_action_group_->setExclusive(true);
    scope_all_action_ = search_menu->addAction(QStringLiteral("全部"));
    scope_files_action_ = search_menu->addAction(QStringLiteral("文件"));
    scope_folders_action_ = search_menu->addAction(QStringLiteral("文件夹"));
    for (auto* action : {scope_all_action_, scope_files_action_, scope_folders_action_}) {
        action->setCheckable(true);
        scope_action_group_->addAction(action);
    }
    scope_all_action_->setChecked(true);
    connect(scope_all_action_, &QAction::triggered, this, [this] { setScopeFromMenu(0); });
    connect(scope_files_action_, &QAction::triggered, this, [this] { setScopeFromMenu(1); });
    connect(scope_folders_action_, &QAction::triggered, this, [this] { setScopeFromMenu(2); });

    search_menu->addSeparator();
    auto* syntax_action = search_menu->addAction(QStringLiteral("搜索语法…"));
    connect(syntax_action, &QAction::triggered, this, &MainWindow::showSearchSyntax);

    bookmarks_menu_ = menuBar()->addMenu(QStringLiteral("书签(&B)"));

    auto* tools_menu = menuBar()->addMenu(QStringLiteral("工具(&T)"));
    auto* rebuild_action = tools_menu->addAction(QStringLiteral("重建索引"));
    connect(rebuild_action, &QAction::triggered, this, &MainWindow::rebuildIndex);

    auto* roots_action = tools_menu->addAction(QStringLiteral("索引目录…"));
    connect(roots_action, &QAction::triggered, this, &MainWindow::editRoots);

    auto* status_action = tools_menu->addAction(QStringLiteral("索引状态…"));
    connect(status_action, &QAction::triggered, this, &MainWindow::showIndexStatus);

    auto* search_diag_action = tools_menu->addAction(QStringLiteral("最近搜索诊断…"));
    connect(search_diag_action, &QAction::triggered, this, &MainWindow::showSearchDiagnostics);

    tools_menu->addSeparator();
    auto* preferences_action = tools_menu->addAction(QStringLiteral("偏好设置…"));
    preferences_action->setMenuRole(QAction::PreferencesRole);
    preferences_action->setShortcut(QKeySequence::Preferences);
    connect(preferences_action, &QAction::triggered, this, &MainWindow::editRoots);

    auto* help_menu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    auto* help_syntax = help_menu->addAction(QStringLiteral("搜索语法"));
    connect(help_syntax, &QAction::triggered, this, &MainWindow::showSearchSyntax);

    help_menu->addSeparator();
    auto* about_action = help_menu->addAction(QStringLiteral("关于 Everything Lite"));
    about_action->setMenuRole(QAction::AboutRole);
    connect(about_action, &QAction::triggered, this, &MainWindow::showAbout);
}

void MainWindow::loadSettings() {
    QSettings settings(QStringLiteral("CICHI"), QStringLiteral("EverythingLite"));
    roots_ = settings.value(QStringLiteral("index/roots")).toStringList();
    if (roots_.isEmpty()) roots_ = defaultRoots();
    scope_combo_->setCurrentIndex(std::clamp(settings.value(QStringLiteral("search/itemScope"), 0).toInt(), 0, 2));
    match_path_check_->setChecked(settings.value(QStringLiteral("search/matchPath"), false).toBool());

    const bool show_status = settings.value(QStringLiteral("view/statusBar"), true).toBool();
    const bool show_scope = settings.value(QStringLiteral("view/scopeBar"), true).toBool();
    const bool show_filter = settings.value(QStringLiteral("view/filterBar"), true).toBool();
    show_status_bar_action_->setChecked(show_status);
    show_scope_bar_action_->setChecked(show_scope);
    show_filter_bar_action_->setChecked(show_filter);
    statusBar()->setVisible(show_status);
    scope_label_->setVisible(show_scope);
    scope_combo_->setVisible(show_filter);

    const auto geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    const auto header_state = settings.value(QStringLiteral("table/header")).toByteArray();
    if (!header_state.isEmpty()) table_->horizontalHeader()->restoreState(header_state);
    const int sort_column = settings.value(QStringLiteral("table/sortColumn"), 0).toInt();
    const auto sort_order = static_cast<Qt::SortOrder>(settings.value(QStringLiteral("table/sortOrder"), static_cast<int>(Qt::AscendingOrder)).toInt());
    table_->sortByColumn(sort_column, sort_order);
    syncSearchControlsFromMenus();
}

void MainWindow::saveSettings() {
    QSettings settings(QStringLiteral("CICHI"), QStringLiteral("EverythingLite"));
    settings.setValue(QStringLiteral("index/roots"), roots_);
    settings.setValue(QStringLiteral("search/itemScope"), scope_combo_->currentIndex());
    settings.setValue(QStringLiteral("search/matchPath"), match_path_check_->isChecked());
    settings.setValue(QStringLiteral("view/statusBar"), statusBar()->isVisible());
    settings.setValue(QStringLiteral("view/scopeBar"), scope_label_->isVisible());
    settings.setValue(QStringLiteral("view/filterBar"), scope_combo_->isVisible());
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("table/header"), table_->horizontalHeader()->saveState());
    settings.setValue(QStringLiteral("table/sortColumn"), table_->horizontalHeader()->sortIndicatorSection());
    settings.setValue(QStringLiteral("table/sortOrder"), static_cast<int>(table_->horizontalHeader()->sortIndicatorOrder()));
}

void MainWindow::loadBookmarks() {
    QSettings settings(QStringLiteral("CICHI"), QStringLiteral("EverythingLite"));
    bookmarks_.clear();
    const int count = settings.beginReadArray(QStringLiteral("bookmarks/items"));
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        SearchBookmark bookmark;
        bookmark.name = settings.value(QStringLiteral("name")).toString();
        bookmark.query = settings.value(QStringLiteral("query")).toString();
        bookmark.scope = settings.value(QStringLiteral("scope"), 0).toInt();
        bookmark.match_path = settings.value(QStringLiteral("matchPath"), false).toBool();
        bookmark.sort_column = settings.value(QStringLiteral("sortColumn"), 0).toInt();
        bookmark.sort_order = static_cast<Qt::SortOrder>(settings.value(QStringLiteral("sortOrder"), static_cast<int>(Qt::AscendingOrder)).toInt());
        if (!bookmark.name.isEmpty()) bookmarks_.push_back(std::move(bookmark));
    }
    settings.endArray();
}

void MainWindow::saveBookmarks() {
    QSettings settings(QStringLiteral("CICHI"), QStringLiteral("EverythingLite"));
    settings.remove(QStringLiteral("bookmarks/items"));
    settings.beginWriteArray(QStringLiteral("bookmarks/items"));
    for (int i = 0; i < static_cast<int>(bookmarks_.size()); ++i) {
        settings.setArrayIndex(i);
        const auto& bookmark = bookmarks_[static_cast<std::size_t>(i)];
        settings.setValue(QStringLiteral("name"), bookmark.name);
        settings.setValue(QStringLiteral("query"), bookmark.query);
        settings.setValue(QStringLiteral("scope"), bookmark.scope);
        settings.setValue(QStringLiteral("matchPath"), bookmark.match_path);
        settings.setValue(QStringLiteral("sortColumn"), bookmark.sort_column);
        settings.setValue(QStringLiteral("sortOrder"), static_cast<int>(bookmark.sort_order));
    }
    settings.endArray();
}

void MainWindow::rebuildBookmarksMenu() {
    if (!bookmarks_menu_) return;
    bookmarks_menu_->clear();

    add_bookmark_action_ = bookmarks_menu_->addAction(QStringLiteral("添加到书签…"));
    add_bookmark_action_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    connect(add_bookmark_action_, &QAction::triggered, this, &MainWindow::addCurrentBookmark);

    organize_bookmarks_action_ = bookmarks_menu_->addAction(QStringLiteral("整理书签…"));
    connect(organize_bookmarks_action_, &QAction::triggered, this, &MainWindow::organizeBookmarks);

    if (bookmarks_.empty()) return;
    bookmarks_menu_->addSeparator();
    for (std::size_t i = 0; i < bookmarks_.size(); ++i) {
        const auto bookmark = bookmarks_[i];
        auto* action = bookmarks_menu_->addAction(bookmark.name);
        connect(action, &QAction::triggered, this, [this, bookmark] { applyBookmark(bookmark); });
    }
}

void MainWindow::updateScopeLabel() {
    if (roots_.isEmpty()) {
        scope_label_->setText(QStringLiteral("未设置索引目录"));
        return;
    }
    const auto preview = roots_.size() <= 3 ? roots_.join(QStringLiteral("    "))
                                            : roots_.mid(0, 3).join(QStringLiteral("    ")) + QStringLiteral("    …");
    scope_label_->setText(QStringLiteral("索引范围（%1 个目录）：%2").arg(roots_.size()).arg(preview));
}

void MainWindow::updateSearchStateLabel() {
    QStringList states;
    if (match_path_check_->isChecked()) states << QStringLiteral("匹配路径");
    if (scope_combo_->currentIndex() == 1) states << QStringLiteral("文件");
    else if (scope_combo_->currentIndex() == 2) states << QStringLiteral("文件夹");
    else states << QStringLiteral("全部");
    search_state_label_->setText(states.join(QStringLiteral(" · ")));
}

void MainWindow::refreshStats() {
    try {
        Database db(toUtf8(db_path_));
        db.initialize();
        indexed_count_ = db.totalFileCount();
        name_search_available_ = db.nameSearchIndexAvailable();
        name_search_ready_ = db.nameSearchIndexReady();
        searchTrace("GUI", "refreshStats indexed_count=" + std::to_string(indexed_count_)
            + " fts_available=" + std::to_string(name_search_available_ ? 1 : 0)
            + " fts_ready=" + std::to_string(name_search_ready_ ? 1 : 0));
        QString acceleration;
        if (!name_search_available_) acceleration = QStringLiteral("    ·    当前 SQLite 未提供 FTS5 trigram，使用兼容搜索");
        else if (!name_search_ready_) acceleration = QStringLiteral("    ·    名称加速索引未就绪");
        status_label_->setText(QStringLiteral("%1 个对象    ·    %2%3")
            .arg(indexed_count_)
            .arg(watcher_ ? fromUtf8(watcher_->backendName()) : QStringLiteral("等待启动"))
            .arg(acceleration));
    } catch (const std::exception& e) {
        status_label_->setText(QStringLiteral("数据库错误：%1").arg(QString::fromUtf8(e.what())));
    }
}

QString MainWindow::currentSearchSignature() const {
    return QStringLiteral("%1\x1f%2\x1f%3")
        .arg(normalizeGuiQuery(search_edit_->text()))
        .arg(scope_combo_->currentIndex())
        .arg(match_path_check_->isChecked() ? 1 : 0);
}

void MainWindow::runSearch() {
    if (indexing_.load() && indexed_count_ == 0) return;
    pending_search_query_ = normalizeGuiQuery(search_edit_->text());
    results_have_more_ = false;
    ++search_request_id_;
    launchSearch(pending_search_query_, 0, false);
}

void MainWindow::loadMoreResults() {
    if (!results_have_more_ || search_in_flight_) return;
    ++search_request_id_;
    launchSearch(normalizeGuiQuery(search_edit_->text()), static_cast<std::size_t>(model_->rowCount()), true);
}

void MainWindow::launchSearch(const QString& query, std::size_t offset, bool append) {
    const auto raw_query = query;
    const auto normalized_query = normalizeGuiQuery(query);
    const int item_scope = scope_combo_->currentIndex();
    const bool match_path = match_path_check_->isChecked();
    const auto request_id = search_request_id_;
    const auto effective_limit = kPageSize + 1;

    last_search_query_ = normalized_query;
    last_search_utf8_hex_ = utf8Hex(normalized_query);
    last_search_request_id_ = request_id;
    last_search_execution_mode_ = QStringLiteral("Background SearchWorker + Shared SearchService");

    if (searchTraceEnabled()) {
        std::ostringstream t;
        t << "BEGIN request_id=" << request_id
          << " append=" << (append ? 1 : 0)
          << " raw=\"" << toUtf8(raw_query) << "\""
          << " normalized=\"" << toUtf8(normalized_query) << "\""
          << " utf8_hex=[" << bytesToHex(toUtf8(normalized_query)) << "]"
          << " scope=" << item_scope
          << " match_path=" << (match_path ? 1 : 0)
          << " limit=" << effective_limit
          << " offset=" << offset
          << " db=\"" << toUtf8(db_path_) << "\""
          << " model_rows_before=" << model_->rowCount();
        searchTrace("GUI-SEARCH", t.str());
    }

    // Keep exactly one SQLite query active. If a newer debounce fires while a
    // previous query is still running, remember only that a newer search is
    // needed; when the worker finishes, run the latest contents of the box.
    // This avoids both query pile-ups and cancellation-related correctness bugs.
    if (search_in_flight_) {
        search_pending_ = true;
        status_label_->setText(QStringLiteral("上一查询仍在后台运行，最新关键词已排队… 可继续输入"));
        searchTrace("GUI-SEARCH", "queued latest request while previous search is in flight");
        return;
    }

    if (!search_worker_ || !search_thread_ || !search_thread_->isRunning()) {
        status_label_->setText(QStringLiteral("搜索线程未就绪"));
        searchTrace("GUI-SEARCH", "background worker is not available");
        return;
    }

    SearchOptions options;
    if (item_scope == 1) options.scope = SearchItemScope::Files;
    else if (item_scope == 2) options.scope = SearchItemScope::Folders;
    else options.scope = SearchItemScope::All;
    options.match_path = match_path;

    search_in_flight_ = true;
    search_pending_ = false;
    status_label_->setText(QStringLiteral("正在后台搜索“%1”… 可继续输入")
        .arg(normalized_query.isEmpty() ? QStringLiteral("全部") : normalized_query));

    const auto query_utf8 = toUtf8(normalized_query);
    const auto db_path = db_path_;
    auto* worker = search_worker_;
    QPointer<MainWindow> window(this);

    QMetaObject::invokeMethod(worker, [window, worker, query_utf8, effective_limit, offset, options,
                                       request_id, append, normalized_query, db_path] {
        auto outcome = std::make_shared<BackgroundSearchResult>(
            worker->search(query_utf8, effective_limit, offset, options));
        if (!window) return;

        QMetaObject::invokeMethod(window.data(), [window, outcome, request_id, append,
                                                   normalized_query, effective_limit, offset, db_path] {
            if (!window) return;
            auto* self = window.data();
            self->search_in_flight_ = false;

            // textEdited/scope changes invalidate request_id immediately. Old
            // queries may finish, but are never allowed to flash stale results.
            const bool still_current = request_id == self->search_request_id_;
            if (!still_current) {
                searchTrace("GUI-SEARCH", "discarded stale background result request_id="
                    + std::to_string(request_id)
                    + " current_request_id=" + std::to_string(self->search_request_id_));
            } else if (!outcome->error.empty()) {
                self->last_search_elapsed_ms_ = outcome->elapsed_ms;
                self->last_search_raw_result_count_ = 0;
                self->last_search_result_count_ = self->model_->rowCount();
                searchTrace("GUI-SEARCH", "BACKGROUND EXCEPTION: " + outcome->error);
                self->status_label_->setText(QStringLiteral("搜索错误：%1")
                    .arg(QString::fromUtf8(outcome->error.c_str())));
            } else {
                self->last_search_raw_result_count_ = static_cast<int>(outcome->results.size());
                searchTrace("GUI-SEARCH", "background SearchService returned="
                    + std::to_string(outcome->results.size()));

                self->results_have_more_ = outcome->results.size() > kPageSize;
                if (self->results_have_more_) outcome->results.resize(kPageSize);

                if (append) self->model_->appendResults(std::move(outcome->results));
                else self->model_->setResults(std::move(outcome->results));

                self->last_search_elapsed_ms_ = outcome->elapsed_ms;
                self->last_search_result_count_ = self->model_->rowCount();
                searchTrace("GUI-MODEL", "model_rows_after=" + std::to_string(self->model_->rowCount())
                    + " have_more=" + std::to_string(self->results_have_more_ ? 1 : 0)
                    + " elapsed_ms=" + std::to_string(self->last_search_elapsed_ms_));

                const auto loaded = self->model_->rowCount();
                self->status_label_->setText(QStringLiteral("%1 个对象    ·    已加载 %2%3    ·    后台查询 %4 ms    ·    %5%6")
                    .arg(self->indexed_count_)
                    .arg(loaded)
                    .arg(self->results_have_more_ ? QStringLiteral("+") : QString{})
                    .arg(self->last_search_elapsed_ms_, 0, 'f', 1)
                    .arg(self->watcher_ ? fromUtf8(self->watcher_->backendName()) : QStringLiteral("未监听"))
                    .arg(self->name_search_ready_ ? QString{} : QStringLiteral("    ·    名称加速索引未就绪")));

                // Search trace mode intentionally keeps the independent CLI
                // comparison. Normal builds never execute this branch.
                runCliProbe(db_path, normalized_query, effective_limit, offset);
                searchTrace("GUI-SEARCH", "END request_id=" + std::to_string(request_id));
            }

            // If the 800 ms timer fired while the worker was busy, do one new
            // query using the latest UI state. Multiple edits collapse to this
            // single latest search instead of building a backlog.
            if (self->search_pending_) {
                self->search_pending_ = false;
                QTimer::singleShot(0, self, [self] { self->runSearch(); });
            }
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void MainWindow::cancelActiveSearch() {
    // Kept as a no-op for source compatibility with older 0.4.x code paths.
    // v0.4.6 does not cancel foreground searches.
}

void MainWindow::startWorker(std::function<void()> job, const QString& start_message, bool refresh_search_on_finish) {
    if (worker_thread_) return;
    setBusy(true, start_message);
    worker_thread_ = QThread::create([job = std::move(job)] { job(); });
    connect(worker_thread_, &QThread::finished, this, [this, refresh_search_on_finish] {
        worker_thread_->deleteLater();
        worker_thread_ = nullptr;
        setBusy(false);
        refreshStats();
        if (refresh_search_on_finish) {
            runSearch();
        } else {
            scheduleFilesystemResultRefresh();
        }
        if (!pending_paths_.isEmpty() || !pending_rescan_roots_.isEmpty()) event_timer_->start();
    });
    worker_thread_->start();
}

void MainWindow::scheduleFilesystemResultRefresh() {
    if (!filesystem_refresh_timer_) return;
    filesystem_refresh_timer_->start();
}

void MainWindow::rebuildIndex() {
    if (roots_.isEmpty()) {
        editRoots();
        if (roots_.isEmpty()) return;
    }
    const auto roots = roots_;
    const auto db_path = db_path_;
    startWorker([this, roots, db_path] {
        try {
            IndexManager manager(toUtf8(db_path));
            std::vector<std::string> native_roots;
            for (const auto& root : roots) native_roots.push_back(toUtf8(root));
            manager.rebuildRoots(native_roots, [this](const std::string& root, std::uint64_t count) {
                QMetaObject::invokeMethod(this, [this, root, count] {
                    status_label_->setText(QStringLiteral("正在索引：%1    %2 项").arg(fromUtf8(root)).arg(count));
                }, Qt::QueuedConnection);
            });
        } catch (const std::exception& e) {
            const auto message = QString::fromUtf8(e.what());
            QMetaObject::invokeMethod(this, [this, message] {
                QMessageBox::critical(this, QStringLiteral("索引失败"), message);
            }, Qt::QueuedConnection);
        }
    }, name_search_ready_ ? QStringLiteral("正在准备索引…")
                          : QStringLiteral("正在准备索引；完成后将一次性建立名称加速索引…"));
}

void MainWindow::editRoots() {
    RootSettingsDialog dialog(roots_, this);
    if (dialog.exec() != QDialog::Accepted) return;
    roots_ = dialog.roots();
    saveSettings();
    updateScopeLabel();
    restartWatcher();
    status_label_->setText(QStringLiteral("索引目录已修改。请从“工具 → 重建索引”同步数据库。"));
}

void MainWindow::showIndexStatus() {
    refreshStats();
    try {
        Database db(toUtf8(db_path_));
        db.initialize();
        name_search_count_ = name_search_available_ ? db.nameSearchIndexCount() : 0;
    } catch (...) {
        name_search_count_ = 0;
    }
    QStringList details;
    details << QStringLiteral("版本：%1").arg(QString::fromLatin1(EVERYTHING_LITE_VERSION));
    details << QStringLiteral("数据库：%1").arg(db_path_);
    details << QStringLiteral("文件记录：%1").arg(indexed_count_);
    details << QStringLiteral("名称 FTS 记录：%1").arg(name_search_count_);
    details << QStringLiteral("名称索引状态：%1").arg(
        !name_search_available_ ? QStringLiteral("unavailable") :
        !name_search_ready_ ? QStringLiteral("not ready") : QStringLiteral("ready"));
    if (name_search_ready_) {
        details << QStringLiteral("深度一致性：可运行 CLI 的 check-search-index（大量索引项可能需要一些时间）");
    }
    details << QStringLiteral("监听后端：%1").arg(watcher_ ? fromUtf8(watcher_->backendName()) : QStringLiteral("未启动"));
    if (!watcher_error_.isEmpty()) details << QStringLiteral("监听错误：%1").arg(watcher_error_);
    if (!last_search_query_.isNull()) {
        details << QStringLiteral("最近查询：%1").arg(last_search_query_.isEmpty() ? QStringLiteral("<全部>") : last_search_query_);
        details << QStringLiteral("最近查询耗时：%1 ms").arg(last_search_elapsed_ms_, 0, 'f', 1);
        details << QStringLiteral("当前已加载结果：%1").arg(last_search_result_count_);
    }
    details << QStringLiteral("索引目录：\n  %1").arg(roots_.join(QStringLiteral("\n  ")));
    QMessageBox::information(this, QStringLiteral("索引状态"), details.join(QStringLiteral("\n")));
}

void MainWindow::showSearchDiagnostics() {
    const auto visible_query = search_edit_ ? search_edit_->text() : QString{};
    const auto normalized_query = normalizeGuiQuery(visible_query);
    QStringList details;
    details << QStringLiteral("版本：%1").arg(QString::fromLatin1(EVERYTHING_LITE_VERSION));
    details << QStringLiteral("数据库：%1").arg(db_path_);
    details << QStringLiteral("搜索框原文：%1").arg(visible_query.isEmpty() ? QStringLiteral("<空>") : visible_query);
    details << QStringLiteral("归一化查询：%1").arg(normalized_query.isEmpty() ? QStringLiteral("<空>") : normalized_query);
    details << QStringLiteral("当前 UTF-8(hex)：%1").arg(utf8Hex(normalized_query));
    details << QStringLiteral("范围：%1").arg(scope_combo_ ? scope_combo_->currentText() : QStringLiteral("未知"));
    details << QStringLiteral("匹配路径：%1").arg(match_path_check_ && match_path_check_->isChecked() ? QStringLiteral("是") : QStringLiteral("否"));
    details << QStringLiteral("最近请求 ID：%1").arg(static_cast<qulonglong>(last_search_request_id_));
    details << QStringLiteral("最近核心查询：%1").arg(last_search_query_.isEmpty() ? QStringLiteral("<空>") : last_search_query_);
    details << QStringLiteral("最近核心 UTF-8(hex)：%1").arg(last_search_utf8_hex_);
    details << QStringLiteral("执行模式：%1").arg(last_search_execution_mode_.isEmpty() ? QStringLiteral("<尚无>") : last_search_execution_mode_);
    details << QStringLiteral("核心返回（含探测项）：%1").arg(last_search_raw_result_count_);
    details << QStringLiteral("模型当前行数：%1").arg(model_ ? model_->rowCount() : 0);
    details << QStringLiteral("查询耗时：%1 ms").arg(last_search_elapsed_ms_, 0, 'f', 1);
    details << QStringLiteral("待处理搜索：否");
    details << QStringLiteral("搜索执行方式：输入完成后延迟搜索（800 ms）+ IME 组词保护 + 启动不空查 + SearchService 复用（v0.4.10）");
    details << QStringLiteral("终端 Trace：%1").arg(searchTraceEnabled() ? QStringLiteral("已启用") : QStringLiteral("未启用"));
    details << QStringLiteral("CLI 探针：%1").arg(cliProbePath());
    QMessageBox::information(this, QStringLiteral("最近搜索诊断"), details.join(QStringLiteral("\n")));
}

QStringList MainWindow::selectedPaths() const {
    QStringList paths;
    if (!table_->selectionModel()) return paths;
    auto rows = table_->selectionModel()->selectedRows(0);
    std::sort(rows.begin(), rows.end(), [](const QModelIndex& a, const QModelIndex& b) { return a.row() < b.row(); });
    for (const auto& index : rows) {
        const auto path = model_->pathAt(index.row());
        if (!path.isEmpty()) paths << path;
    }
    return paths;
}

QStringList MainWindow::selectedNames() const {
    QStringList names;
    if (!table_->selectionModel()) return names;
    auto rows = table_->selectionModel()->selectedRows(0);
    std::sort(rows.begin(), rows.end(), [](const QModelIndex& a, const QModelIndex& b) { return a.row() < b.row(); });
    for (const auto& index : rows) {
        const auto name = model_->nameAt(index.row());
        if (!name.isEmpty()) names << name;
    }
    return names;
}

void MainWindow::openCurrent() {
    const auto paths = selectedPaths();
    if (paths.isEmpty()) return;
    for (const auto& path : paths.mid(0, 20)) desktop_actions_->open(path);
    if (paths.size() > 20) statusBar()->showMessage(QStringLiteral("一次最多打开前 20 个所选项目。"), 4000);
}

void MainWindow::openWithCurrent() {
    const auto paths = selectedPaths();
    if (paths.isEmpty()) return;
#ifdef Q_OS_MACOS
    bool ok = false;
    const auto app_name = QInputDialog::getText(this, QStringLiteral("打开方式"),
                                                 QStringLiteral("请输入 macOS 应用名称（例如 Preview、TextEdit、Visual Studio Code）："),
                                                 QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || app_name.isEmpty()) return;
    for (const auto& path : paths.mid(0, 20)) {
        desktop_actions_->openWithApplication(path, app_name);
    }
#else
    const auto executable = QFileDialog::getOpenFileName(this, QStringLiteral("选择可执行应用文件"), QStringLiteral("/usr/bin"));
    if (executable.isEmpty()) return;
    for (const auto& path : paths.mid(0, 20)) desktop_actions_->openWithExecutable(path, executable);
#endif
}

void MainWindow::revealCurrent() {
    const auto current = table_->currentIndex();
    if (!current.isValid()) return;
    const auto path = model_->pathAt(current.row());
    if (path.isEmpty()) return;
    desktop_actions_->reveal(path);
}

void MainWindow::quickLookCurrent() {
    const auto current = table_->currentIndex();
    if (!current.isValid()) return;
    const auto path = model_->pathAt(current.row());
    if (path.isEmpty()) return;
    desktop_actions_->preview(path);
}

void MainWindow::copyCurrentPath() {
    const auto paths = selectedPaths();
    if (!paths.isEmpty()) QApplication::clipboard()->setText(paths.join('\n'));
}

void MainWindow::copyCurrentName() {
    const auto names = selectedNames();
    if (!names.isEmpty()) QApplication::clipboard()->setText(names.join('\n'));
}

void MainWindow::exportLoadedResults() {
    const auto file_name = QFileDialog::getSaveFileName(this, QStringLiteral("导出已加载结果"),
                                                         QDir::home().filePath(QStringLiteral("everything-lite-results.csv")),
                                                         QStringLiteral("CSV 文件 (*.csv);;文本文件 (*.txt)"));
    if (file_name.isEmpty()) return;
    QFile file(file_name);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::critical(this, QStringLiteral("导出失败"), file.errorString());
        return;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << "Name,Path,Size,Modified\n";
    for (int row = 0; row < model_->rowCount(); ++row) {
        out << csvQuote(model_->data(model_->index(row, 0), Qt::DisplayRole).toString()) << ','
            << csvQuote(model_->pathAt(row)) << ','
            << csvQuote(model_->data(model_->index(row, 2), Qt::DisplayRole).toString()) << ','
            << csvQuote(model_->data(model_->index(row, 3), Qt::DisplayRole).toString()) << '\n';
    }
    statusBar()->showMessage(QStringLiteral("已导出 %1 条已加载结果：%2").arg(model_->rowCount()).arg(file_name), 5000);
}

void MainWindow::showContextMenu(const QPoint& pos) {
    const auto index = table_->indexAt(pos);
    if (!index.isValid()) return;
    if (!table_->selectionModel()->isRowSelected(index.row(), QModelIndex())) {
        table_->clearSelection();
        table_->selectRow(index.row());
    }
    table_->setCurrentIndex(index);

    QMenu menu(this);
    auto* open = menu.addAction(QStringLiteral("打开"));
    auto* quick_look = menu.addAction(QStringLiteral("快速查看"));
    quick_look->setShortcut(QKeySequence(Qt::Key_Space));
#ifdef Q_OS_MACOS
    auto* reveal = menu.addAction(QStringLiteral("在 Finder 中显示"));
#else
    auto* reveal = menu.addAction(QStringLiteral("打开所在位置"));
#endif
    auto* open_with = menu.addAction(QStringLiteral("打开方式…"));
    menu.addSeparator();
    auto* copy_path = menu.addAction(QStringLiteral("复制完整路径"));
    auto* copy_name = menu.addAction(QStringLiteral("复制名称"));
    const auto* selected = menu.exec(table_->viewport()->mapToGlobal(pos));
    if (selected == open) openCurrent();
    else if (selected == quick_look) quickLookCurrent();
    else if (selected == reveal) revealCurrent();
    else if (selected == open_with) openWithCurrent();
    else if (selected == copy_path) copyCurrentPath();
    else if (selected == copy_name) copyCurrentName();
}

void MainWindow::restartWatcher() {
    const auto generation = ++watcher_generation_;
    if (watcher_) watcher_->stop();
    watcher_ = createPlatformWatcher();
    std::vector<std::string> native_roots;
    for (const auto& root : roots_) native_roots.push_back(toUtf8(root));
    watcher_->setRoots(std::move(native_roots));
    watcher_->setCallback([this, generation](const FileEvent& event) {
        QMetaObject::invokeMethod(this, [this, generation, event] {
            receiveWatcherEvent(generation, event);
        }, Qt::QueuedConnection);
    });
    watcher_error_.clear();
    if (!watcher_->start()) {
        watcher_error_ = fromUtf8(watcher_->lastError());
        if (watcher_error_.isEmpty()) watcher_error_ = QStringLiteral("文件监听启动失败");
    } else if (!watcher_->lastError().empty()) {
        watcher_error_ = fromUtf8(watcher_->lastError());
    }
    watcher_error_label_->setText(watcher_error_);
    watcher_error_label_->setVisible(!watcher_error_.isEmpty());
    refreshStats();
}

void MainWindow::receiveWatcherEvent(std::uint64_t generation, const FileEvent& event) {
    if (generation != watcher_generation_) return;
    scheduleFileEvent(event);
}

void MainWindow::scheduleFileEvent(const FileEvent& event) {
    const auto error = !event.error.empty() ? fromUtf8(event.error)
                                          : watcher_ ? fromUtf8(watcher_->lastError()) : QString{};
    if (!error.isEmpty()) {
        watcher_error_ = error;
        watcher_error_label_->setText(error);
        watcher_error_label_->show();
    }
    if (event.path.empty()) return;
    const auto path = fromUtf8(event.path);
    const auto root = rootForPath(path);
    if (root.isEmpty()) return;
    if (event.needs_full_rescan && error.isEmpty()) {
        // A current coverage rebuild with empty lastError means the watcher
        // recovered. Statistics and ordinary path events never erase errors.
        watcher_error_.clear();
        watcher_error_label_->clear();
        watcher_error_label_->hide();
    }
    if (event.needs_full_rescan) {
        pending_rescan_roots_.insert(root);
    } else {
        pending_paths_.insert(path);
        if (pending_paths_.size() > 400) {
            pending_rescan_roots_.insert(root);
            pending_paths_.clear();
        }
    }
    event_timer_->start();
}

void MainWindow::processPendingEvents() {
    if (worker_thread_) {
        event_timer_->start();
        return;
    }
    const auto paths = pending_paths_;
    const auto rescans = pending_rescan_roots_;
    pending_paths_.clear();
    pending_rescan_roots_.clear();
    if (paths.isEmpty() && rescans.isEmpty()) return;

    const auto db_path = db_path_;
    const auto roots = roots_;
    startWorker([this, db_path, paths, rescans, roots] {
        try {
            IndexManager manager(toUtf8(db_path));
            for (const auto& root : rescans) manager.rebuildRoot(toUtf8(root));
            for (const auto& path : paths) {
                QString best;
                const auto native_path = toUtf8(path);
                for (const auto& root : roots) {
                    if (pathIsWithin(native_path, toUtf8(root)) && root.size() > best.size()) best = root;
                }
                if (!best.isEmpty() && !rescans.contains(best)) manager.applyPathChange(toUtf8(best), native_path);
            }
        } catch (...) {
            // A later FSEvents event or manual rebuild will reconcile the index.
        }
    }, QStringLiteral("正在同步文件变化…"), false);
}

QString MainWindow::rootForPath(const QString& path) const {
    QString best;
    const auto native_path = toUtf8(path);
    for (const auto& root : roots_) {
        if (pathIsWithin(native_path, toUtf8(root)) && root.size() > best.size()) best = root;
    }
    return best;
}

void MainWindow::setBusy(bool busy, const QString& message) {
    indexing_ = busy;
    if (!message.isEmpty()) status_label_->setText(message);
}

void MainWindow::newWindow() {
    QProcess::startDetached(QCoreApplication::applicationFilePath(), {});
}

void MainWindow::resetColumns() {
    auto* header = table_->horizontalHeader();
    for (int logical = 0; logical < model_->columnCount(); ++logical) {
        const int visual = header->visualIndex(logical);
        if (visual != logical) header->moveSection(visual, logical);
    }
    header->setSectionResizeMode(0, QHeaderView::Interactive);
    header->setSectionResizeMode(1, QHeaderView::Stretch);
    header->setSectionResizeMode(2, QHeaderView::Interactive);
    header->setSectionResizeMode(3, QHeaderView::Interactive);
    table_->setColumnWidth(0, 310);
    table_->setColumnWidth(2, 92);
    table_->setColumnWidth(3, 155);
    table_->sortByColumn(0, Qt::AscendingOrder);
}

void MainWindow::addCurrentBookmark() {
    bool ok = false;
    const auto name = QInputDialog::getText(this, QStringLiteral("添加到书签"), QStringLiteral("书签名称："),
                                             QLineEdit::Normal, bookmarkDefaultName(search_edit_->text()), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    SearchBookmark bookmark;
    bookmark.name = name;
    bookmark.query = search_edit_->text();
    bookmark.scope = scope_combo_->currentIndex();
    bookmark.match_path = match_path_check_->isChecked();
    bookmark.sort_column = table_->horizontalHeader()->sortIndicatorSection();
    bookmark.sort_order = table_->horizontalHeader()->sortIndicatorOrder();
    bookmarks_.push_back(std::move(bookmark));
    saveBookmarks();
    rebuildBookmarksMenu();
}

void MainWindow::organizeBookmarks() {
    if (bookmarks_.empty()) {
        QMessageBox::information(this, QStringLiteral("整理书签"), QStringLiteral("当前没有书签。"));
        return;
    }
    QStringList names;
    for (const auto& bookmark : bookmarks_) names << bookmark.name;
    bool ok = false;
    const auto selected = QInputDialog::getItem(this, QStringLiteral("整理书签"),
                                                 QStringLiteral("选择要删除的书签："), names, 0, false, &ok);
    if (!ok || selected.isEmpty()) return;
    const auto answer = QMessageBox::question(this, QStringLiteral("删除书签"),
                                               QStringLiteral("确定删除书签“%1”吗？").arg(selected));
    if (answer != QMessageBox::Yes) return;
    const auto it = std::find_if(bookmarks_.begin(), bookmarks_.end(), [&](const SearchBookmark& b) { return b.name == selected; });
    if (it != bookmarks_.end()) bookmarks_.erase(it);
    saveBookmarks();
    rebuildBookmarksMenu();
}

void MainWindow::applyBookmark(const SearchBookmark& bookmark) {
    search_edit_->setText(bookmark.query);
    scope_combo_->setCurrentIndex(std::clamp(bookmark.scope, 0, 2));
    match_path_check_->setChecked(bookmark.match_path);
    table_->sortByColumn(bookmark.sort_column, bookmark.sort_order);
    updateSearchStateLabel();
    runSearch();
}

void MainWindow::showSearchSyntax() {
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("搜索语法"));
    box.setIcon(QMessageBox::Information);
    box.setText(QStringLiteral(
        "Everything Lite v0.4 搜索语法\n\n"
        "report                 名称包含 report\n"
        "ext:pdf                PDF 文件/文件夹名后缀过滤\n"
        "path:sample            完整路径包含 sample\n"
        "type:file              仅文件\n"
        "type:dir               仅文件夹\n"
        "size:>100m             大于 100 MB\n"
        "modified:7d            最近 7 天修改\n"
        "\"Example Collection\"   短语\n\n"
        "多个条件可组合。普通关键词默认只匹配名称；“搜索 → 匹配路径”开启后才匹配完整路径。\n"
        "Match Case / Whole Word / Regex 将在 v0.6 搜索兼容版实现。"));
    box.exec();
}

void MainWindow::showAbout() {
    QMessageBox::about(this, QStringLiteral("关于 Everything Lite"),
        QStringLiteral("<b>Everything Lite %1</b><br><br>"
                       "跨平台本地文件名搜索工具。<br>"
                       "核心：C++17 + SQLite；界面：Qt 6；macOS 实时更新：FSEvents。<br><br>"
                       "v0.4 的目标是对齐 Everything 的主窗口、菜单和常用操作，同时保留 macOS 的 Finder 与 Quick Look 体验。")
            .arg(QString::fromLatin1(EVERYTHING_LITE_VERSION)));
}

void MainWindow::setScopeFromMenu(int scope_index) {
    scope_combo_->setCurrentIndex(std::clamp(scope_index, 0, 2));
    updateSearchStateLabel();
}

void MainWindow::syncSearchControlsFromMenus() {
    if (match_path_action_) match_path_action_->setChecked(match_path_check_->isChecked());
    const auto scope = scope_combo_->currentIndex();
    if (scope == 1) scope_files_action_->setChecked(true);
    else if (scope == 2) scope_folders_action_->setChecked(true);
    else scope_all_action_->setChecked(true);
    updateSearchStateLabel();
}

} // namespace everything_lite
