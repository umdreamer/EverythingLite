#include "ui/main_window.h"
#include "ui/app_settings.h"
#include "ui/index_storage_dialog.h"
#include "ui/exclusion_settings_dialog.h"
#include "core/index_storage.h"
#include <QDateTime>

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
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
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
#include <QProgressDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QProcess>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSizePolicy>
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

void runCliProbe(const QString& db_path, const QString& query, const QString& folder_scope,
                 std::size_t limit, std::size_t offset) {
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
    if (!folder_scope.isEmpty()) args << QStringLiteral("--in-folder") << folder_scope;

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

QString ElidedLabel::displayText() const {
    return fontMetrics().elidedText(text().simplified(), Qt::ElideRight,
                                    std::max(0, contentsRect().width()));
}

void ElidedLabel::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setFont(font());
    painter.setPen(palette().color(foregroundRole()));
    painter.drawText(contentsRect(), alignment() | Qt::TextSingleLine, displayText());
}

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
    search_timer_->setTimerType(Qt::PreciseTimer);
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
        // changes the text. SQLite receives cancellation immediately; stale
        // results never replace the current view.
        cancelActiveSearch();
        search_pending_ = false;
        ++search_request_id_;
        resetPaging();
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
        cancelActiveSearch();
        search_pending_ = false;
        ++search_request_id_;
        resetPaging();
        updateSearchStateLabel();
        search_timer_->start(kSearchIdleDelayMs);
    });
    connect(match_path_check_, &QCheckBox::toggled, this, [this](bool checked) {
        if (match_path_action_ && match_path_action_->isChecked() != checked) match_path_action_->setChecked(checked);
        cancelActiveSearch();
        search_pending_ = false;
        ++search_request_id_;
        resetPaging();
        updateSearchStateLabel();
        search_timer_->start(kSearchIdleDelayMs);
    });
    connect(include_hidden_check_, &QCheckBox::toggled, this, [this] {
        cancelActiveSearch();
        search_pending_ = false;
        ++search_request_id_;
        resetPaging();
        updateSearchStateLabel();
        search_timer_->start(kSearchIdleDelayMs);
    });

    event_timer_ = new QTimer(this);
    event_timer_->setSingleShot(true);
    event_timer_->setInterval(700);
    event_timer_->setTimerType(Qt::PreciseTimer);
    connect(event_timer_, &QTimer::timeout, this, &MainWindow::processPendingEvents);
    retry_timer_ = new QTimer(this);
    retry_timer_->setSingleShot(true);
    retry_timer_->setTimerType(Qt::PreciseTimer);
    connect(retry_timer_, &QTimer::timeout, this, &MainWindow::processDueRetries);

    // v0.4.2: filesystem churn must never pre-empt a user search. FSEvents can
    // fire continuously under ~/Library; v0.4.1 restarted the foreground query
    // after every sync batch, so slower queries could be cancelled forever.
    // The first change fixes a bounded refresh deadline; later changes cannot
    // postpone it indefinitely. Active user queries and edits retain priority.
    filesystem_refresh_timer_ = new QTimer(this);
    filesystem_refresh_timer_->setSingleShot(true);
    filesystem_refresh_timer_->setInterval(1800);
    connect(filesystem_refresh_timer_, &QTimer::timeout, this, [this] {
        if (!filesystem_results_dirty_ || closing_) return;
        // A filesystem refresh never supersedes an active user query or the
        // latest UI state waiting for its input debounce / IME commit.
        if (search_in_flight_ || search_pending_ || search_ime_composing_
            || search_timer_->isActive()) {
            filesystem_refresh_timer_->start(1800);
            return;
        }
        if (normalizeGuiQuery(search_edit_->text()).isEmpty()) {
            filesystem_results_dirty_ = false;
            return;
        }
        filesystem_results_dirty_ = false;
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
    status_label_->setText(QStringLiteral("索引 %1 个对象；输入关键词开始搜索").arg(indexed_count_));
}

MainWindow::~MainWindow() {
    closing_ = true;
    if (storage_cancellation_) storage_cancellation_->store(true);
    cancelActiveSearch();
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
    closing_ = true;
    if (storage_cancellation_) storage_cancellation_->store(true);
    cancelActiveSearch();
    search_timer_->stop();
    filesystem_refresh_timer_->stop();
    event_timer_->stop();
    retry_timer_->stop();
    saveSettings();
    saveBookmarks();
    QMainWindow::closeEvent(event);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == search_edit_ && event && event->type() == QEvent::InputMethod) {
        auto* input_event = static_cast<QInputMethodEvent*>(event);
        const bool composing_now = !input_event->preeditString().isEmpty();

        if (composing_now) {
            cancelActiveSearch();
            search_pending_ = false;
            ++search_request_id_;
            resetPaging();
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
    include_hidden_check_ = new QCheckBox(QStringLiteral("显示隐藏项目"), this);
    include_hidden_check_->setChecked(true);
    include_hidden_check_->setToolTip(QStringLiteral("包括点号开头和 Finder 标记为隐藏的文件及文件夹"));

    search_row->addWidget(search_edit_, 1);
    search_row->addWidget(scope_combo_);
    search_row->addWidget(match_path_check_);
    search_row->addWidget(include_hidden_check_);
    layout->addLayout(search_row);

    auto* folder_row = new QHBoxLayout();
    auto* choose_folder = new QPushButton(QStringLiteral("指定文件夹…"), this);
    choose_folder->setObjectName(QStringLiteral("chooseFolderScope"));
    clear_folder_scope_ = new QPushButton(QStringLiteral("清除范围"), this);
    clear_folder_scope_->setObjectName(QStringLiteral("clearFolderScope"));
    folder_scope_label_ = new QLabel(this);
    folder_scope_label_->setObjectName(QStringLiteral("folderScopeLabel"));
    folder_scope_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    folder_scope_status_ = new QLabel(this);
    folder_scope_status_->setObjectName(QStringLiteral("folderScopeStatus"));
    folder_scope_status_->setStyleSheet(QStringLiteral("color: #b33"));
    folder_row->addWidget(choose_folder);
    folder_row->addWidget(clear_folder_scope_);
    folder_row->addWidget(folder_scope_label_, 1);
    folder_row->addWidget(folder_scope_status_);
    layout->addLayout(folder_row);
    connect(choose_folder, &QPushButton::clicked, this, [this] {
        const auto initial = folder_scope_.isEmpty() ? QDir::homePath() : folder_scope_;
        const auto chosen = QFileDialog::getExistingDirectory(this, QStringLiteral("选择搜索文件夹"), initial);
        if (!chosen.isEmpty()) selectFolderScope(chosen);
    });
    connect(clear_folder_scope_, &QPushButton::clicked, this, [this] { setFolderScope({}); });

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
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Interactive);
    table_->setColumnWidth(4, 150);
    table_->setColumnWidth(5, 170);
    layout->addWidget(table_, 1);

    setCentralWidget(central);

    status_label_ = new ElidedLabel(this);
    status_label_->setTextFormat(Qt::PlainText);
    status_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusBar()->addWidget(status_label_, 1);
    search_metrics_label_ = new ElidedLabel(QStringLiteral("尚未搜索"), this);
    search_metrics_label_->setFixedWidth(205);
    search_metrics_label_->setAlignment(Qt::AlignCenter);
    statusBar()->addPermanentWidget(search_metrics_label_);
    search_state_label_ = new ElidedLabel(this);
    search_state_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sync_error_label_ = new ElidedLabel(this);
    sync_error_label_->setTextFormat(Qt::PlainText);
    sync_error_label_->setWordWrap(false);
    sync_error_label_->setFixedWidth(140);
    sync_error_label_->setText(QStringLiteral("索引同步待命"));
    statusBar()->addPermanentWidget(sync_error_label_);
    watcher_error_label_ = new ElidedLabel(this);
    watcher_error_label_->setTextFormat(Qt::PlainText);
    watcher_error_label_->setWordWrap(false);
    watcher_error_label_->setFixedWidth(120);
    watcher_error_label_->setText(QStringLiteral("目录监听待启动"));
    statusBar()->addPermanentWidget(watcher_error_label_);
    search_state_label_->setTextFormat(Qt::PlainText);
    search_state_label_->setFixedWidth(160);
    statusBar()->addPermanentWidget(search_state_label_);
    statusBar()->setSizeGripEnabled(false);
    statusBar()->setMaximumHeight(fontMetrics().height() * 2 + 16);
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
    auto* search_log_action = search_menu->addAction(QStringLiteral("搜索日志…"));
    connect(search_log_action, &QAction::triggered, this, &MainWindow::showSearchLog);

    auto* syntax_action = search_menu->addAction(QStringLiteral("搜索语法…"));
    connect(syntax_action, &QAction::triggered, this, &MainWindow::showSearchSyntax);

    bookmarks_menu_ = menuBar()->addMenu(QStringLiteral("书签(&B)"));

    auto* tools_menu = menuBar()->addMenu(QStringLiteral("工具(&T)"));
    auto* rebuild_action = tools_menu->addAction(QStringLiteral("重建索引"));
    connect(rebuild_action, &QAction::triggered, this, &MainWindow::rebuildIndex);

    auto* roots_action = tools_menu->addAction(QStringLiteral("索引目录…"));
    connect(roots_action, &QAction::triggered, this, &MainWindow::editRoots);

    auto* excludes_action = tools_menu->addAction(QStringLiteral("排除规则…"));
    connect(excludes_action, &QAction::triggered, this, &MainWindow::showExclusionSettings);

    auto* storage_action = tools_menu->addAction(QStringLiteral("索引存储…"));
    connect(storage_action, &QAction::triggered, this, &MainWindow::showIndexStorage);

    auto* status_action = tools_menu->addAction(QStringLiteral("索引状态…"));
    connect(status_action, &QAction::triggered, this, &MainWindow::showIndexStatus);

    auto* overview_action = tools_menu->addAction(QStringLiteral("设置总览…"));
    connect(overview_action, &QAction::triggered, this, &MainWindow::showSettingsOverview);

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
    auto settings = makeAppSettings();
    sync_retry_base_ms_ = std::clamp(settings.value(QStringLiteral("index/syncRetryBaseMs"), 5000).toInt(), 1000, 60000);
    roots_ = settings.value(QStringLiteral("index/roots")).toStringList();
    if (roots_.isEmpty()) roots_ = defaultRoots();
    scope_combo_->setCurrentIndex(std::clamp(settings.value(QStringLiteral("search/itemScope"), 0).toInt(), 0, 2));
    match_path_check_->setChecked(settings.value(QStringLiteral("search/matchPath"), false).toBool());
    include_hidden_check_->setChecked(settings.value(QStringLiteral("search/includeHidden"), true).toBool());
    folder_scope_ = settings.value(QStringLiteral("search/folderScope")).toString();
    if (!folder_scope_.isEmpty() && !QDir::isAbsolutePath(folder_scope_)) folder_scope_.clear();
    if (!folder_scope_.isEmpty()) folder_scope_ = QDir::cleanPath(folder_scope_);
    updateFolderScopeLabel();

    // Restore controls once after the 0.4.15 UI regression. Later user toggles
    // in the View menu remain persistent.
    const bool restore_controls = !settings.value(QStringLiteral("view/restoredControls0415Fixbug2"), false).toBool();
    const bool show_status = restore_controls || settings.value(QStringLiteral("view/statusBar"), true).toBool();
    const bool show_scope = restore_controls || settings.value(QStringLiteral("view/scopeBar"), true).toBool();
    const bool show_filter = restore_controls || settings.value(QStringLiteral("view/filterBar"), true).toBool();
    show_status_bar_action_->setChecked(show_status);
    show_scope_bar_action_->setChecked(show_scope);
    show_filter_bar_action_->setChecked(show_filter);
    statusBar()->setVisible(show_status);
    scope_label_->setVisible(show_scope);
    scope_combo_->setVisible(show_filter);
    if (restore_controls) settings.setValue(QStringLiteral("view/restoredControls0415Fixbug2"), true);

    const auto geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    const auto header_state = settings.value(QStringLiteral("table/header")).toByteArray();
    if (!header_state.isEmpty() && !table_->horizontalHeader()->restoreState(header_state)) resetColumns();
    const int sort_column = settings.value(QStringLiteral("table/sortColumn"), 0).toInt();
    const auto sort_order = static_cast<Qt::SortOrder>(settings.value(QStringLiteral("table/sortOrder"), static_cast<int>(Qt::AscendingOrder)).toInt());
    table_->sortByColumn(std::clamp(sort_column, 0, model_->columnCount() - 1), sort_order);
    syncSearchControlsFromMenus();
}

void MainWindow::saveSettings() {
    auto settings = makeAppSettings();
    settings.setValue(QStringLiteral("index/roots"), roots_);
    settings.setValue(QStringLiteral("search/itemScope"), scope_combo_->currentIndex());
    settings.setValue(QStringLiteral("search/matchPath"), match_path_check_->isChecked());
    settings.setValue(QStringLiteral("search/includeHidden"), include_hidden_check_->isChecked());
    settings.setValue(QStringLiteral("search/folderScope"), folder_scope_);
    settings.setValue(QStringLiteral("view/statusBar"), statusBar()->isVisible());
    settings.setValue(QStringLiteral("view/scopeBar"), scope_label_->isVisible());
    settings.setValue(QStringLiteral("view/filterBar"), scope_combo_->isVisible());
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("table/header"), table_->horizontalHeader()->saveState());
    settings.setValue(QStringLiteral("table/sortColumn"), table_->horizontalHeader()->sortIndicatorSection());
    settings.setValue(QStringLiteral("table/sortOrder"), static_cast<int>(table_->horizontalHeader()->sortIndicatorOrder()));
}

void MainWindow::loadBookmarks() {
    auto settings = makeAppSettings();
    bookmarks_.clear();
    const int count = settings.beginReadArray(QStringLiteral("bookmarks/items"));
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        SearchBookmark bookmark;
        bookmark.name = settings.value(QStringLiteral("name")).toString();
        bookmark.query = settings.value(QStringLiteral("query")).toString();
        bookmark.folder_scope = settings.value(QStringLiteral("folderScope")).toString();
        bookmark.scope = settings.value(QStringLiteral("scope"), 0).toInt();
        bookmark.match_path = settings.value(QStringLiteral("matchPath"), false).toBool();
        bookmark.sort_column = settings.value(QStringLiteral("sortColumn"), 0).toInt();
        bookmark.sort_order = static_cast<Qt::SortOrder>(settings.value(QStringLiteral("sortOrder"), static_cast<int>(Qt::AscendingOrder)).toInt());
        if (!bookmark.name.isEmpty()) bookmarks_.push_back(std::move(bookmark));
    }
    settings.endArray();
}

void MainWindow::saveBookmarks() {
    auto settings = makeAppSettings();
    settings.remove(QStringLiteral("bookmarks/items"));
    settings.beginWriteArray(QStringLiteral("bookmarks/items"));
    for (int i = 0; i < static_cast<int>(bookmarks_.size()); ++i) {
        settings.setArrayIndex(i);
        const auto& bookmark = bookmarks_[static_cast<std::size_t>(i)];
        settings.setValue(QStringLiteral("name"), bookmark.name);
        settings.setValue(QStringLiteral("query"), bookmark.query);
        settings.setValue(QStringLiteral("folderScope"), bookmark.folder_scope);
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

void MainWindow::updateFolderScopeLabel() {
    if (folder_scope_.isEmpty()) {
        folder_scope_label_->setText(QStringLiteral("搜索范围：全部已索引目录"));
        folder_scope_status_->clear();
        clear_folder_scope_->setEnabled(false);
        return;
    }
    folder_scope_label_->setText(QStringLiteral("搜索范围：%1").arg(folder_scope_));
    clear_folder_scope_->setEnabled(true);
    if (folder_scope_index_pending_) {
        folder_scope_status_->setText(QStringLiteral("正在加入索引…"));
        return;
    }
    if (folder_scope_index_failed_) {
        folder_scope_status_->setText(QStringLiteral("索引失败；请检查同步错误后重试"));
        return;
    }
    bool indexed = false;
    try {
        Database db(toUtf8(db_path_));
        const auto scope = toUtf8(folder_scope_);
        for (const auto& source : db.sources()) {
            if (!rootForPath(folder_scope_).isEmpty() && pathIsWithin(scope, source.root)) {
                indexed = true;
                break;
            }
        }
        folder_scope_status_->setText(indexed ? QString{} : QStringLiteral("该目录未索引；加入索引后才能搜索"));
    } catch (const std::exception& error) {
        folder_scope_status_->setText(QStringLiteral("索引状态读取失败：%1").arg(fromUtf8(error.what())));
    }
}

void MainWindow::setFolderScope(const QString& path) {
    if (!path.isEmpty() && !QDir::isAbsolutePath(path)) {
        folder_scope_status_->setText(QStringLiteral("请选择绝对路径"));
        return;
    }
    const auto normalized = path.isEmpty() ? QString{} : QDir::cleanPath(path);
    if (normalized == folder_scope_) return;
    folder_scope_ = normalized;
    folder_scope_index_pending_ = false;
    folder_scope_index_failed_ = false;
    cancelActiveSearch();
    search_pending_ = false;
    ++search_request_id_;
    resetPaging();
    model_->setResults({});
    updateCachedStatus();
    updateFolderScopeLabel();
    updateSearchStateLabel();
    search_timer_->start(kSearchIdleDelayMs);
}

void MainWindow::selectFolderScope(const QString& path) {
    if (!QDir(path).exists()) {
        QMessageBox::warning(this, QStringLiteral("选择搜索文件夹"), QStringLiteral("所选文件夹不存在或无法访问。"));
        return;
    }
    const auto normalized = QDir::cleanPath(path);
    setFolderScope(normalized);
    if (!rootForPath(normalized).isEmpty()) return;
    search_timer_->stop();
    const auto answer = QMessageBox::question(this, QStringLiteral("文件夹未索引"),
        QStringLiteral("该文件夹未索引。是否加入索引并在完成后搜索？\n%1").arg(normalized),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer != QMessageBox::Yes) {
        status_label_->setText(QStringLiteral("该文件夹未索引；未执行搜索"));
        return;
    }
    roots_.append(normalized);
    folder_scope_index_pending_ = true;
    saveSettings();
    updateScopeLabel();
    updateFolderScopeLabel();
    restartWatcher(false);
    pending_rescan_roots_.insert(normalized);
    normal_events_due_ = true;
    dispatchSyncJobs();
}

void MainWindow::updateSearchStateLabel() {
    QStringList states;
    if (match_path_check_->isChecked()) states << QStringLiteral("匹配路径");
    if (!include_hidden_check_->isChecked()) states << QStringLiteral("隐藏项目已过滤");
    if (scope_combo_->currentIndex() == 1) states << QStringLiteral("文件");
    else if (scope_combo_->currentIndex() == 2) states << QStringLiteral("文件夹");
    else states << QStringLiteral("全部");
    if (!folder_scope_.isEmpty()) states << QStringLiteral("指定文件夹");
    search_state_label_->setText(states.join(QStringLiteral(" · ")));
}

void MainWindow::refreshStats() {
    try {
        Database db(toUtf8(db_path_));
        db.initialize();
        indexed_count_ = db.totalFileCount();
        name_search_available_ = db.nameSearchIndexAvailable();
        name_search_ready_ = db.nameSearchIndexReady();
        if (!folder_scope_.isEmpty()) updateFolderScopeLabel();
        searchTrace("GUI", "refreshStats indexed_count=" + std::to_string(indexed_count_)
            + " fts_available=" + std::to_string(name_search_available_ ? 1 : 0)
            + " fts_ready=" + std::to_string(name_search_ready_ ? 1 : 0));
        updateCachedStatus();
    } catch (const std::exception& e) {
        status_label_->setText(QStringLiteral("数据库错误：%1").arg(QString::fromUtf8(e.what())));
    }
}

bool MainWindow::userSearchOwnsStatus() const {
    return search_in_flight_ || search_pending_ || search_ime_composing_
        || (search_timer_ && search_timer_->isActive());
}

void MainWindow::updateCachedStatus() {
    if (closing_ || userSearchOwnsStatus()) return;
    const auto state = normalizeGuiQuery(search_edit_->text()).isEmpty()
        ? QStringLiteral("输入关键词开始搜索")
        : QStringLiteral("搜索结果已加载");
    status_label_->setText(QStringLiteral("索引 %1 个对象；%2").arg(indexed_count_).arg(state));
    search_metrics_label_->setToolTip(QStringLiteral("索引总数：%1；当前已加载：%2；最近搜索用时：%3 ms")
        .arg(indexed_count_).arg(model_->rowCount()).arg(last_search_elapsed_ms_, 0, 'f', 1));
}

QString MainWindow::currentSearchSignature() const {
    return normalizeGuiQuery(search_edit_->text())
        + QLatin1Char('\x1f') + QString::number(scope_combo_->currentIndex())
        + QLatin1Char('\x1f') + QString::number(match_path_check_->isChecked() ? 1 : 0)
        + QLatin1Char('\x1f') + QString::number(include_hidden_check_->isChecked() ? 1 : 0)
        + QLatin1Char('\x1f') + folder_scope_;
}

void MainWindow::runSearch() {
    if(storage_maintenance_) {search_pending_=true;pending_search_query_=normalizeGuiQuery(search_edit_->text());return;}
    if (closing_ || search_ime_composing_ || (indexing_.load() && indexed_count_ == 0)) return;
    search_timer_->stop();
    if (!folder_scope_.isEmpty() && (rootForPath(folder_scope_).isEmpty()
        || folder_scope_index_pending_ || folder_scope_index_failed_)) {
        cancelActiveSearch();
        model_->clearResults();
        status_label_->setText(QStringLiteral("该文件夹未索引；未执行搜索"));
        search_metrics_label_->setText(QStringLiteral("未搜索"));
        search_log_entries_.push_back({++search_request_id_, QDateTime::currentDateTime().toString(Qt::ISODate),
                                       normalizeGuiQuery(search_edit_->text()), folder_scope_,
                                       scope_combo_->currentIndex(), match_path_check_->isChecked(),
                                       include_hidden_check_->isChecked(), 0, QStringLiteral("未执行"),
                                       QStringLiteral("该文件夹未索引；加入索引后才能搜索"), 0.0, 0, 0,
                                       static_cast<int>(sync_errors_.size())});
        if (search_log_entries_.size() > kMaxSearchLogEntries) search_log_entries_.erase(search_log_entries_.begin());
        refreshSearchLogDialog();
        return;
    }
    cancelActiveSearch();
    pending_search_query_ = normalizeGuiQuery(search_edit_->text());
    resetPaging();
    ++search_request_id_;
    launchSearch(pending_search_query_, 0, false);
}

void MainWindow::resetPaging() {
    results_have_more_ = false;
    next_search_offset_ = 0;
    search_revision_.reset();
    pagination_signature_.clear();
}

void MainWindow::loadMoreResults() {
    if (closing_ || storage_maintenance_ || !results_have_more_ || search_in_flight_ || search_pending_
        || search_ime_composing_ || search_timer_->isActive()) return;
    if (pagination_signature_ != currentSearchSignature() || !search_revision_) {
        resetPaging();
        search_timer_->start(kSearchIdleDelayMs);
        return;
    }
    ++search_request_id_;
    launchSearch(normalizeGuiQuery(search_edit_->text()), next_search_offset_, true);
}

void MainWindow::launchSearch(const QString& query, std::size_t offset, bool append) {
    const auto raw_query = query;
    const auto normalized_query = normalizeGuiQuery(query);
    const int item_scope = scope_combo_->currentIndex();
    const bool match_path = match_path_check_->isChecked();
    const auto folder_scope = folder_scope_;
    const auto request_id = search_request_id_;
    const auto effective_limit = kPageSize + 1;
    const auto signature = currentSearchSignature();
    const auto expected_revision = append ? search_revision_ : std::nullopt;

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
    // Cancellation stops obsolete SQL; the next query starts only on completion.
    if (search_in_flight_) {
        search_pending_ = true;
        status_label_->setText(QStringLiteral("上一查询仍在后台运行，最新关键词已排队… 可继续输入"));
        searchTrace("GUI-SEARCH", "queued latest request while previous search is in flight");
        return;
    }

    if (!search_worker_ || !search_thread_ || !search_thread_->isRunning()) {
        status_label_->setText(QStringLiteral("搜索线程未就绪"));
        search_metrics_label_->setText(QStringLiteral("搜索未就绪"));
        searchTrace("GUI-SEARCH", "background worker is not available");
        return;
    }

    SearchOptions options;
    if (item_scope == 1) options.scope = SearchItemScope::Files;
    else if (item_scope == 2) options.scope = SearchItemScope::Folders;
    else options.scope = SearchItemScope::All;
    options.match_path = match_path;
    options.include_hidden = include_hidden_check_->isChecked();
    if (!folder_scope.isEmpty()) options.folder_scope = toUtf8(folder_scope);

    active_search_cancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = active_search_cancellation_;
    search_in_flight_ = true;
    search_pending_ = false;
    search_log_entries_.push_back({request_id, QDateTime::currentDateTime().toString(Qt::ISODate),
                                   normalized_query, folder_scope, item_scope, match_path,
                                   options.include_hidden, offset, QStringLiteral("搜索中"), {}, 0.0, 0, 0,
                                   static_cast<int>(sync_errors_.size())});
    if (search_log_entries_.size() > kMaxSearchLogEntries) search_log_entries_.erase(search_log_entries_.begin());
    refreshSearchLogDialog();
    status_label_->setText(QStringLiteral("正在后台搜索“%1”… 可继续输入")
        .arg(normalized_query.isEmpty() ? QStringLiteral("全部") : normalized_query));
    search_metrics_label_->setText(QStringLiteral("搜索中…"));

    const auto query_utf8 = toUtf8(normalized_query);
    const auto db_path = db_path_;
    auto* worker = search_worker_;
    QPointer<MainWindow> window(this);

    QMetaObject::invokeMethod(worker, [window, worker, query_utf8, effective_limit, offset, options,
                                       request_id, append, normalized_query, db_path, cancellation, signature, expected_revision, folder_scope] {
        auto outcome = std::make_shared<BackgroundSearchResult>(
            worker->search(query_utf8, effective_limit, offset, options, cancellation.get(), expected_revision));
        if (!window) return;

        QMetaObject::invokeMethod(window.data(), [window, outcome, request_id, append,
                                                   normalized_query, effective_limit, offset, db_path, signature, folder_scope] {
            if (!window) return;
            auto* self = window.data();
            self->search_in_flight_ = false;
            self->active_search_cancellation_.reset();
            if (self->closing_) return;
            if (self->storage_maintenance_) {
                self->finishSearchLog(request_id, QStringLiteral("取消"), QStringLiteral("索引存储维护开始"),
                                      outcome->elapsed_ms, 0, 0);
                self->tryStartStorageMaintenance();
                return;
            }

            // textEdited/scope changes invalidate request_id immediately. Old
            // queries may finish, but are never allowed to flash stale results.
            const bool same_configuration = signature == self->currentSearchSignature();
            const bool still_current = request_id == self->search_request_id_
                && same_configuration && !outcome->cancelled;
            if (!still_current) {
                self->finishSearchLog(request_id, QStringLiteral("取消"),
                                      QStringLiteral("已被新搜索或筛选条件替代"), outcome->elapsed_ms, 0, 0);
                searchTrace("GUI-SEARCH", "discarded stale background result request_id="
                    + std::to_string(request_id)
                    + " current_request_id=" + std::to_string(self->search_request_id_));
                if (request_id == self->search_request_id_ && !same_configuration) {
                    self->resetPaging();
                    if (!self->search_ime_composing_ && !self->search_timer_->isActive())
                        self->search_timer_->start(kSearchIdleDelayMs);
                }
            } else if (outcome->snapshot_changed) {
                self->finishSearchLog(request_id, QStringLiteral("待重试"), QStringLiteral("索引版本已变化"),
                                      outcome->elapsed_ms, 0, 0);
                self->resetPaging();
                self->model_->clearResults();
                self->last_search_elapsed_ms_ = outcome->elapsed_ms;
                self->last_search_raw_result_count_ = 0;
                self->last_search_result_count_ = 0;
                self->status_label_->setText(QStringLiteral("索引已变化，等待重新加载…"));
                self->search_metrics_label_->setText(QStringLiteral("索引变化"));
                // Reuse the user-search debounce so repeated commits coalesce
                // and cannot cause an immediate restart loop on a busy index.
                self->search_timer_->start(kSearchIdleDelayMs);
            } else if (!outcome->error.empty()) {
                self->finishSearchLog(request_id, QStringLiteral("失败"), QString::fromUtf8(outcome->error.c_str()),
                                      outcome->elapsed_ms, 0, self->model_->rowCount());
                self->resetPaging();
                self->last_search_elapsed_ms_ = outcome->elapsed_ms;
                self->last_search_raw_result_count_ = 0;
                self->last_search_result_count_ = self->model_->rowCount();
                searchTrace("GUI-SEARCH", "BACKGROUND EXCEPTION: " + outcome->error);
                self->status_label_->setText(QStringLiteral("搜索错误：%1")
                    .arg(QString::fromUtf8(outcome->error.c_str())));
                self->search_metrics_label_->setText(QStringLiteral("搜索失败"));
            } else {
                self->last_search_raw_result_count_ = static_cast<int>(outcome->results.size());
                searchTrace("GUI-SEARCH", "background SearchService returned="
                    + std::to_string(outcome->results.size()));

                self->results_have_more_ = outcome->results.size() > kPageSize;
                if (self->results_have_more_) outcome->results.resize(kPageSize);
                // The look-ahead row is not consumed. Display deduplication
                // and local sorting never determine the next SQL offset.
                self->next_search_offset_ = offset + outcome->results.size();
                self->search_revision_ = outcome->revision;
                self->pagination_signature_ = signature;

                if (append) self->model_->appendResults(std::move(outcome->results));
                else self->model_->setResults(std::move(outcome->results));

                self->last_search_elapsed_ms_ = outcome->elapsed_ms;
                self->last_search_result_count_ = self->model_->rowCount();
                self->finishSearchLog(request_id, QStringLiteral("完成"), {}, outcome->elapsed_ms,
                                      self->last_search_raw_result_count_, self->last_search_result_count_);
                searchTrace("GUI-MODEL", "model_rows_after=" + std::to_string(self->model_->rowCount())
                    + " have_more=" + std::to_string(self->results_have_more_ ? 1 : 0)
                    + " elapsed_ms=" + std::to_string(self->last_search_elapsed_ms_));

                const auto loaded = self->model_->rowCount();
                self->status_label_->setText(QStringLiteral("搜索完成"));
                self->search_metrics_label_->setText(QStringLiteral("搜索结果 %1%2 条 · %3 ms")
                    .arg(loaded)
                    .arg(self->results_have_more_ ? QStringLiteral("+") : QString{})
                    .arg(self->last_search_elapsed_ms_, 0, 'f', 1));

                // Search trace mode intentionally keeps the independent CLI
                // comparison. Normal builds never execute this branch.
                runCliProbe(db_path, normalized_query, folder_scope, effective_limit, offset);
                searchTrace("GUI-SEARCH", "END request_id=" + std::to_string(request_id));
            }

            // If the 800 ms timer fired while the worker was busy, do one new
            // query using the latest UI state. Multiple edits collapse to this
            // single latest search instead of building a backlog.
            if (self->search_pending_ && !self->search_ime_composing_ && !self->search_timer_->isActive()) {
                self->search_pending_ = false;
                QTimer::singleShot(0, self, [self] {
                    if (!self->closing_ && !self->search_ime_composing_ && !self->search_timer_->isActive()) self->runSearch();
                });
            }
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void MainWindow::cancelActiveSearch() {
    if (active_search_cancellation_) active_search_cancellation_->store(true, std::memory_order_relaxed);
}

void MainWindow::startWorker(std::function<void()> job, const QString& start_message, bool refresh_search_on_finish,
                             std::function<void(const QString&)> on_finished) {
    if (worker_thread_ || storage_maintenance_ || closing_) return;
    setBusy(true, start_message);
    struct Stats {
        std::uint64_t count = 0;
        bool available = false;
        bool ready = false;
        bool valid = false;
        QString error;
    };
    auto stats = std::make_shared<Stats>();
    const auto db_path = db_path_;
    worker_thread_ = QThread::create([job = std::move(job), stats, db_path] {
        try {
            job();
        } catch (const std::exception& e) {
            stats->error = QString::fromUtf8(e.what());
        } catch (...) {
            stats->error = QStringLiteral("未知索引任务错误");
        }
        // Counting the full index belongs on this worker, never the GUI callback.
        try {
            Database db(toUtf8(db_path));
            db.initialize();
            stats->count = db.totalFileCount();
            stats->available = db.nameSearchIndexAvailable();
            stats->ready = db.nameSearchIndexReady();
            stats->valid = true;
        } catch (const std::exception&) {
            // This optional count refresh does not change whether the index
            // job succeeded. Keep the previous count and report only job errors.
        }
    });
    connect(worker_thread_, &QThread::finished, this, [this, refresh_search_on_finish, stats, on_finished=std::move(on_finished)] {
        worker_thread_->deleteLater();
        worker_thread_ = nullptr;
        setBusy(false);
        if (closing_) return;
        const bool count_changed = stats->valid && indexed_count_ != stats->count;
        if (stats->valid) {
            indexed_count_ = stats->count;
            name_search_available_ = stats->available;
            name_search_ready_ = stats->ready;
        }
        if (!stats->error.isEmpty() && !on_finished)
            status_label_->setText(QStringLiteral("索引任务未完成：%1").arg(stats->error));
        if(on_finished) on_finished(stats->error);
        if(storage_maintenance_) {tryStartStorageMaintenance();return;}
        if (refresh_search_on_finish || count_changed) updateCachedStatus();
        // Manual rebuilds and event batches share the same user-query protection.
        if (refresh_search_on_finish) scheduleFilesystemResultRefresh();
        // Continue only queues whose own deadlines have already elapsed.
        // A completed worker never advances a fresh normal event or retry.
        if (normal_events_due_ || !ready_sync_retries_.isEmpty()) {
            QTimer::singleShot(0, this, &MainWindow::dispatchSyncJobs);
        }
    });
    worker_thread_->start();
}

void MainWindow::showExclusionSettings() {
    if(worker_thread_ || storage_maintenance_) {
        statusBar()->showMessage(QStringLiteral("索引任务正在运行，请稍后再修改排除规则。"),8000);
        return;
    }
    try {
        Database db(toUtf8(db_path_));db.initialize();
        const auto previous_rules=db.exclusionPolicy().rules();
        auto* dialog=new ExclusionSettingsDialog(previous_rules,this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        QPointer<ExclusionSettingsDialog> safe(dialog);
        dialog->saveRequested=[this,safe,previous_rules](ExclusionRules rules){
            if(worker_thread_ || storage_maintenance_) {
                if(safe) safe->showError(QStringLiteral("索引任务正在运行，规则未保存。请稍后重试。"));
                return;
            }
            if(safe) safe->setSaving(true);
            auto removed=std::make_shared<std::uint64_t>(0);
            auto elapsed=std::make_shared<QElapsedTimer>();elapsed->start();
            const auto path=toUtf8(db_path_);
            cancelActiveSearch();++search_request_id_;resetPaging();search_timer_->stop();
            startWorker([this,path,rules,removed,safe,elapsed]{
                Database db(path);db.initialize();
                qint64 last_report=-1000;
                *removed=db.saveExclusionPolicy(rules,[this,safe,elapsed,&last_report](std::uint64_t checked,std::uint64_t total) {
                    const auto now=elapsed->elapsed();
                    if(checked && checked<total && now-last_report<150)return;
                    last_report=now;
                    QMetaObject::invokeMethod(this,[safe,checked,total,seconds=static_cast<int>(now/1000)] {
                        if(safe)safe->setSaveProgress(checked,total,seconds);
                    },Qt::QueuedConnection);
                });
            },QStringLiteral("正在保存排除规则…"),true,[this,removed,safe,previous_rules,rules](const QString& error){
                if(!error.isEmpty()) {
                    if(safe) {safe->setSaving(false);safe->showError(QStringLiteral("保存失败：%1").arg(error));}
                    statusBar()->showMessage(QStringLiteral("排除规则保存失败：%1").arg(error),15000);
                    return;
                }
                if(safe) safe->finishSaved();
                statusBar()->showMessage(QStringLiteral("排除规则已保存，清理 %1 项。放宽规则后请重建索引。").arg(*removed),15000);
                if(model_->rowCount() || !search_edit_->text().isEmpty()) runSearch();
                if(!previous_rules.index_app_bundle_contents && rules.index_app_bundle_contents)
                    QTimer::singleShot(0,this,&MainWindow::rebuildIndex);
            });
        };
        dialog->show();
    } catch(const std::exception& error) {
        statusBar()->showMessage(QStringLiteral("读取排除规则失败：%1").arg(QString::fromUtf8(error.what())),15000);
    }
}

void MainWindow::showIndexStorage() {
    if(storage_dialog_) {storage_dialog_->show();storage_dialog_->raise();return;}
    auto* dialog=new IndexStorageDialog(db_path_,this);storage_dialog_=dialog;
    dialog->maintenanceRequested=[this](bool optimize){requestStorageMaintenance(optimize);};
    dialog->cleanupRequested=[this](const std::vector<std::string>& selected){requestStorageMaintenance(false,selected);};
    dialog->cancellationRequested=[this]{if(storage_cancellation_)storage_cancellation_->store(true);};
    dialog->show();
}

void MainWindow::requestStorageMaintenance(bool optimize,const std::vector<std::string>& cleanup_selection) {
    if(closing_ || storage_maintenance_)return;
    storage_maintenance_=true;storage_optimize_=optimize;storage_cleanup_selection_=cleanup_selection;
    storage_cancellation_=std::make_shared<std::atomic_bool>(false);
    cancelActiveSearch();++search_request_id_;resetPaging();
    search_timer_->stop();filesystem_refresh_timer_->stop();
    search_pending_=true;pending_search_query_=normalizeGuiQuery(search_edit_->text());
    if(storage_dialog_)storage_dialog_->beginOperation();
    tryStartStorageMaintenance();
}

void MainWindow::tryStartStorageMaintenance() {
    if(closing_ || !storage_maintenance_ || worker_thread_ || search_in_flight_)return;
    struct Outcome {IndexStorage before,after;QString message;bool sizes=false,vacuum_completed=false;};
    auto result=std::make_shared<Outcome>();
    const auto path=toUtf8(db_path_);
    const auto cancellation=storage_cancellation_;const bool optimize=storage_optimize_;const auto cleanup_selection=storage_cleanup_selection_;
    QPointer<MainWindow> window(this);
    worker_thread_=QThread::create([window,result,path,cancellation,optimize,cleanup_selection]{
        const bool compact_operation=!optimize && cleanup_selection.empty();
        auto phase=[window,result,compact_operation](IndexStoragePhase phase){
            // These facts are captured synchronously on the maintenance worker.
            // Queued labels are presentation only and cannot determine results.
            if(compact_operation && phase==IndexStoragePhase::Verify)result->vacuum_completed=true;
            if(window)QMetaObject::invokeMethod(window.data(),[window,phase]{if(window && window->storage_dialog_)window->storage_dialog_->setPhase(phase);},Qt::QueuedConnection);
        };
        try {
            if(cancellation->load())throw std::runtime_error("storage: cancelled before maintenance");
            result->before=readIndexStorage(path);
            if(!cleanup_selection.empty()) {
                phase(IndexStoragePhase::Verify);
                const auto report=removeIndexStorageBackups(path,cleanup_selection,cancellation.get(),[window](std::size_t count){
                    if(window)QMetaObject::invokeMethod(window.data(),[window,count]{if(window && window->storage_dialog_)window->storage_dialog_->setWindowTitle(QStringLiteral("索引存储 — 已删除 %1 份备份").arg(count));},Qt::QueuedConnection);
                },true);
                result->message=QStringLiteral("已删除 %1 份备份，释放 %2 字节。").arg(report.removed_paths.size()).arg(report.removed_bytes);
                for(const auto& removed:report.removed_paths)result->message+=QLatin1Char('\n')+QFileInfo(fromUtf8(removed)).fileName();
                for(const auto& orphan:report.orphan_receipts)result->message+=QStringLiteral("\n遗留来源凭证：%1").arg(QFileInfo(fromUtf8(orphan)).fileName());
                if(!report.error.empty())result->message+=QStringLiteral("\n%1：%2；此前完成的删除已保留。").arg(report.cancelled?QStringLiteral("清理已取消"):QStringLiteral("清理未完成"),fromUtf8(report.error));
                else result->message+=QStringLiteral("\n清理完成。当前索引仍可用；已删除的备份不可恢复。");
                result->after=readIndexStorage(path);result->sizes=true;
            }else if(optimize){
                const auto report=migrateIndexStorageToDirectory(path,"",true,cancellation.get(),phase);
                result->before=report.before;result->after=report.after;result->sizes=report.before.page_size>0 && report.after.page_size>0;
                if(!report.error.empty())result->message=report.migration_committed
                    ?QStringLiteral("格式已迁移；空间整理或后续验证未完成：%1。").arg(fromUtf8(report.error))
                    :QStringLiteral("格式优化失败：%1。").arg(fromUtf8(report.error));
                else if(report.already_compact)result->message=QStringLiteral("已经使用紧凑格式；本次未迁移。可用“仅整理空间”回收空闲页。");
                else result->message=QStringLiteral("格式迁移与空间整理完成；本次未创建备份。");
                result->message+=QStringLiteral("\n迁移已提交：%1；VACUUM 已完成：%2。").arg(report.migration_committed?QStringLiteral("是"):QStringLiteral("否"),report.vacuum_completed?QStringLiteral("是"):QStringLiteral("否"));
            }else{
                const auto report=compactIndexStorage(path,"",cancellation.get(),phase);
                result->before=report.before;result->after=report.after;result->sizes=true;
                result->message=QStringLiteral("空间整理完成；本次未创建备份。");
            }
        }catch(const std::exception& e){
            result->message+=QStringLiteral("\n索引维护未完成：%1。").arg(QString::fromUtf8(e.what()));
            try{result->after=readIndexStorage(path);result->sizes=result->before.page_size>0 && result->after.page_size>0;}catch(const std::exception&){}
        }
        if(compact_operation) {
            result->message+=QStringLiteral("\nVACUUM 已完成：%1。").arg(result->vacuum_completed?QStringLiteral("是"):QStringLiteral("否"));
        }
    });
    connect(worker_thread_,&QThread::finished,this,[this,result]{
        worker_thread_->deleteLater();worker_thread_=nullptr;
        storage_maintenance_=false;storage_cancellation_.reset();storage_cleanup_selection_.clear();
        if(closing_)return;
        if(storage_dialog_)storage_dialog_->finishOperation(result->message,result->sizes?&result->before:nullptr,result->sizes?&result->after:nullptr);
        // Keep the latest controls and watcher/retry queues; never replay an old query.
        search_pending_=false;resetPaging();
        if(!search_ime_composing_)search_timer_->start(kSearchIdleDelayMs);
        if(normal_events_due_ || !ready_sync_retries_.isEmpty())QTimer::singleShot(0,this,&MainWindow::dispatchSyncJobs);
    });worker_thread_->start();
}

void MainWindow::scheduleFilesystemResultRefresh() {
    if (!filesystem_refresh_timer_ || closing_) return;
    filesystem_results_dirty_ = true;
    if (!filesystem_refresh_timer_->isActive()) filesystem_refresh_timer_->start(1800);
}

void MainWindow::rebuildIndex() {
    if (closing_) return;
    if (worker_thread_ || storage_maintenance_) {
        statusBar()->showMessage(QStringLiteral("索引任务正在运行，请稍后重建。"),8000);
        return;
    }
    if (roots_.isEmpty()) {
        editRoots();
        if (roots_.isEmpty()) return;
    }
    for (auto it = sync_failure_scopes_.begin(); it != sync_failure_scopes_.end(); ++it) {
        if (roots_.contains(it.value().root)) {
            sync_attempts_[it.key()] = 0;
            retry_deadlines_.remove(it.key());
            ready_sync_retries_.remove(it.key());
            // An explicit manual attempt renews this configured source's budget.
            // Stale dormant markers must not suppress its new bounded retries.
            if (it.value().rescan) dormant_rescan_roots_.remove(it.value().root);
            else dormant_paths_.remove(it.value().path);
        }
    }
    armRetryTimer();
    const auto roots = roots_;
    const auto db_path = db_path_;
    QStringList configured;
    for(const auto& root:roots) {
        const auto normalized=fromUtf8(normalizePath(toUtf8(root)));
        if(!configured.contains(normalized))configured.push_back(normalized);
    }
    std::stable_sort(configured.begin(),configured.end(),[](const QString& a,const QString& b){return a.toUtf8().size()>b.toUtf8().size();});
    auto errors=std::make_shared<QStringList>();
    auto elapsed=std::make_shared<QElapsedTimer>();elapsed->start();
    auto scanned=std::make_shared<std::atomic<std::uint64_t>>(0);
    auto root_index=std::make_shared<std::atomic<int>>(0);
    auto* progress=new QProgressDialog(this);
    progress->setObjectName(QStringLiteral("rebuildProgressDialog"));
    progress->setWindowTitle(QStringLiteral("重建索引"));
    progress->setCancelButton(nullptr);
    progress->setRange(0,0);progress->setMinimumDuration(0);
    progress->setWindowModality(Qt::NonModal);
    progress->setAutoClose(false);progress->setAutoReset(false);
    progress->setLabelText(QStringLiteral("已处理 0 项 · 已用 0 秒"));
    progress->show();
    QPointer<QProgressDialog> safe_progress(progress);
    auto* progress_timer=new QTimer(progress);
    connect(progress_timer,&QTimer::timeout,this,[safe_progress,configured,elapsed,scanned,root_index] {
        if(!safe_progress)return;
        const auto number=root_index->load(std::memory_order_relaxed);
        const auto folder=number<configured.size()?configured.at(number):QStringLiteral("完成扫描");
        safe_progress->setLabelText(QStringLiteral("目录 %1/%2：%3\n已处理 %4 项 · 已用 %5 秒（总量未知）")
            .arg(std::min(number+1,static_cast<int>(configured.size()))).arg(configured.size()).arg(folder)
            .arg(scanned->load(std::memory_order_relaxed)).arg(elapsed->elapsed()/1000));
    });
    progress_timer->start(250);
    statusBar()->showMessage(QStringLiteral("正在重建索引：%1 个目录…").arg(roots.size()));
    startWorker([this, configured, db_path, errors, scanned, root_index] {
        IndexManager manager(toUtf8(db_path));
        // Any registration error aborts before traversing a partial topology.
        for(const auto& root:configured)manager.database().ensureSourceForRoot(toUtf8(root));
        std::uint64_t completed_scanned=0;
        for (int i=0;i<configured.size();++i) {
            root_index->store(i,std::memory_order_relaxed);
            const auto& root=configured.at(i);
            QString error;
            try {
                const auto stats = manager.rebuildRoot(toUtf8(root), [scanned,completed_scanned](const std::string&, std::uint64_t count) {
                    scanned->store(completed_scanned+count,std::memory_order_relaxed);
                });
                completed_scanned+=stats.scanned;
                scanned->store(completed_scanned,std::memory_order_relaxed);
                if (!stats.completed) error = stats.cancelled ? QStringLiteral("同步已取消")
                    : stats.error.empty() ? QStringLiteral("扫描未完整完成") : fromUtf8(stats.error);
            } catch (const std::exception& e) { error = QString::fromUtf8(e.what()); }
            catch (...) { error = QStringLiteral("未知同步错误"); }
            if(!error.isEmpty())errors->push_back(QStringLiteral("%1：%2").arg(root,error));
            QMetaObject::invokeMethod(this, [this, root, error] { if (!closing_) reportSyncOutcome(root, root, true, error); }, Qt::QueuedConnection);
        }
        root_index->store(configured.size(),std::memory_order_relaxed);
    }, name_search_ready_ ? QStringLiteral("正在准备索引…")
                          : QStringLiteral("正在准备索引；完成后将一次性建立名称加速索引…"),true,
        [this,errors,safe_progress](const QString& error){
            if(safe_progress){safe_progress->close();safe_progress->deleteLater();}
            if(!error.isEmpty() || !errors->isEmpty())
                statusBar()->showMessage(QStringLiteral("重建未完成：%1").arg(!error.isEmpty()?error:errors->join(QStringLiteral("；"))),15000);
            else statusBar()->showMessage(QStringLiteral("重建完成：索引现有 %1 项。").arg(indexed_count_),15000);
        });
}

void MainWindow::editRoots() {
    if(storage_maintenance_){status_label_->setText(QStringLiteral("索引维护进行中；完成后可修改索引目录。"));return;}
    RootSettingsDialog dialog(roots_, this);
    if (dialog.exec() != QDialog::Accepted) return;
    roots_ = dialog.roots();
    saveSettings();
    updateScopeLabel();
    restartWatcher();
    status_label_->setText(QStringLiteral("索引目录已修改。请从“工具 → 重建索引”同步数据库。"));
}

void MainWindow::showIndexStatus() {
    if(storage_maintenance_){status_label_->setText(QStringLiteral("索引维护进行中；完成后可读取索引状态。"));return;}
    QStringList details;
    details << QStringLiteral("版本：%1").arg(QString::fromLatin1(EVERYTHING_LITE_VERSION));
    details << QStringLiteral("数据库：%1").arg(db_path_);
    details << QStringLiteral("文件记录（最近统计）：%1").arg(indexed_count_);
    details << QStringLiteral("名称索引状态：%1").arg(
        !name_search_available_ ? QStringLiteral("unavailable") :
        !name_search_ready_ ? QStringLiteral("not ready") : QStringLiteral("ready"));
    if (name_search_ready_) {
        details << QStringLiteral("深度一致性：可运行 CLI 的 check-search-index（大量索引项可能需要一些时间）");
    }
    details << QStringLiteral("监听后端：%1").arg(watcher_ ? fromUtf8(watcher_->backendName()) : QStringLiteral("未启动"));
    if (!watcher_error_.isEmpty()) details << QStringLiteral("监听错误：%1").arg(watcher_error_);
    details << QStringLiteral("同步状态：%1").arg(sync_error_label_->text());
    if (!sync_error_label_->toolTip().isEmpty())
        details << QStringLiteral("同步详情：\n%1").arg(sync_error_label_->toolTip());
    if (!last_search_query_.isNull()) {
        details << QStringLiteral("最近查询：%1").arg(last_search_query_.isEmpty() ? QStringLiteral("<全部>") : last_search_query_);
        details << QStringLiteral("最近查询耗时：%1 ms").arg(last_search_elapsed_ms_, 0, 'f', 1);
        details << QStringLiteral("当前已加载结果：%1").arg(last_search_result_count_);
    }
    details << QStringLiteral("索引目录：\n  %1").arg(roots_.join(QStringLiteral("\n  ")));
    QMessageBox::information(this, QStringLiteral("索引状态"), details.join(QStringLiteral("\n")));
}

void MainWindow::showSettingsOverview() {
    auto settings = makeAppSettings();
    QStringList details{
        QStringLiteral("当前索引目录："),
        roots_.isEmpty() ? QStringLiteral("  <未设置>") : QStringLiteral("  ") + roots_.join(QStringLiteral("\n  ")),
        QStringLiteral("当前搜索文件夹：%1").arg(folder_scope_.isEmpty() ? QStringLiteral("<全部已索引目录>") : folder_scope_),
        QStringLiteral("当前搜索范围：%1").arg(scope_combo_->currentText()),
        QStringLiteral("匹配路径：%1").arg(match_path_check_->isChecked() ? QStringLiteral("是") : QStringLiteral("否")),
        QStringLiteral("搜索隐藏文件和文件夹：%1").arg(include_hidden_check_->isChecked() ? QStringLiteral("是") : QStringLiteral("否")),
        QStringLiteral("状态栏：%1；索引范围栏：%2；过滤器栏：%3")
            .arg(statusBar()->isVisible() ? QStringLiteral("显示") : QStringLiteral("隐藏"),
                 scope_label_->isVisible() ? QStringLiteral("显示") : QStringLiteral("隐藏"),
                 scope_combo_->isVisible() ? QStringLiteral("显示") : QStringLiteral("隐藏")),
        QString{}, QStringLiteral("偏好设置文件：%1").arg(settings.fileName()),
        QStringLiteral("数据库：%1").arg(db_path_),
        QString{}, QStringLiteral("已保存的设置：")};
    const auto keys = settings.allKeys();
    if (keys.isEmpty()) details << QStringLiteral("  <无>");
    for (const auto& key : keys) {
        const auto value = settings.value(key);
        QString display;
        if (value.metaType().id() == QMetaType::QByteArray)
            display = QStringLiteral("<二进制布局，%1 字节>").arg(value.toByteArray().size());
        else if (value.metaType().id() == QMetaType::QStringList)
            display = value.toStringList().join(QStringLiteral("；"));
        else display = value.toString();
        details << QStringLiteral("  %1 = %2").arg(key, display);
    }
    details << QString{} << QStringLiteral("排除规则：");
    try {
        Database db(toUtf8(db_path_));
        const auto policy = db.exclusionPolicy();
        const auto& rules = policy.rules();
        details << QStringLiteral("  排除目录：%1").arg(rules.directories.empty() ? QStringLiteral("<无>") : fromUtf8(rules.directories.front()));
        for (std::size_t i = 1; i < rules.directories.size(); ++i) details << QStringLiteral("    %1").arg(fromUtf8(rules.directories[i]));
        details << QStringLiteral("  排除文件：%1").arg(rules.files.empty() ? QStringLiteral("<无>") : fromUtf8(rules.files.front()));
        for (std::size_t i = 1; i < rules.files.size(); ++i) details << QStringLiteral("    %1").arg(fromUtf8(rules.files[i]));
        details << QStringLiteral("  目录过滤器：%1").arg(fromUtf8(rules.folder_filter));
        details << QStringLiteral("  文件过滤器：%1").arg(fromUtf8(rules.file_filter));
        details << QString{} << QStringLiteral("数据库中的索引来源：");
        for (const auto& source : db.sources()) details << QStringLiteral("  %1").arg(fromUtf8(source.root));
    } catch (const std::exception& error) {
        details << QStringLiteral("  读取失败：%1").arg(QString::fromUtf8(error.what()));
    }
    details << QString{} << QStringLiteral("数据库相关文件：");
    const QFileInfo db_info(db_path_);
    const auto files = QDir(db_info.absolutePath()).entryInfoList({db_info.fileName() + QStringLiteral("*")}, QDir::Files | QDir::Hidden, QDir::Name);
    for (const auto& file : files) details << QStringLiteral("  %1（%2 字节）").arg(file.absoluteFilePath()).arg(file.size());

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("设置总览"));
    dialog.resize(760, 620);
    auto* layout = new QVBoxLayout(&dialog);
    auto* text = new QPlainTextEdit(&dialog);
    text->setObjectName(QStringLiteral("settingsOverviewText"));
    text->setReadOnly(true);
    text->setPlainText(details.join(QStringLiteral("\n")));
    layout->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
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

void MainWindow::finishSearchLog(std::uint64_t request_id, const QString& state, const QString& detail,
                                 double elapsed_ms, int returned_count, int loaded_count) {
    const auto it = std::find_if(search_log_entries_.begin(), search_log_entries_.end(),
                                 [request_id](const SearchLogEntry& entry) { return entry.request_id == request_id; });
    if (it == search_log_entries_.end()) return;
    it->state = state;
    it->detail = detail;
    it->elapsed_ms = elapsed_ms;
    it->returned_count = returned_count;
    it->loaded_count = loaded_count;
    refreshSearchLogDialog();
}

void MainWindow::refreshSearchLogDialog() {
    if (!search_log_dialog_) return;
    auto* list = search_log_dialog_->findChild<QListWidget*>(QStringLiteral("searchLogList"));
    if (!list) return;
    const auto selected_id = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toULongLong() : 0;
    list->clear();
    int selected_row = 0;
    for (auto it = search_log_entries_.rbegin(); it != search_log_entries_.rend(); ++it) {
        auto* item = new QListWidgetItem(QStringLiteral("%1  %2  %3")
            .arg(it->started_at, it->state, it->query.isEmpty() ? QStringLiteral("<全部>") : it->query));
        item->setData(Qt::UserRole, static_cast<qulonglong>(it->request_id));
        list->addItem(item);
        if (it->request_id == selected_id) selected_row = list->count() - 1;
    }
    if (list->count()) list->setCurrentRow(selected_row);
    else if (auto* details = search_log_dialog_->findChild<QPlainTextEdit*>(QStringLiteral("searchLogDetails")))
        details->clear();
}

void MainWindow::showSearchLog() {
    if (search_log_dialog_) {
        search_log_dialog_->show();
        search_log_dialog_->raise();
        search_log_dialog_->activateWindow();
        return;
    }
    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("searchLogDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("搜索日志"));
    dialog->resize(780, 470);
    search_log_dialog_ = dialog;
    auto* layout = new QVBoxLayout(dialog);
    auto* note = new QLabel(QStringLiteral("仅保留本窗口最近 100 次搜索；关闭窗口后清除，不写入磁盘。"), dialog);
    layout->addWidget(note);
    auto* content = new QHBoxLayout;
    auto* list = new QListWidget(dialog);
    list->setObjectName(QStringLiteral("searchLogList"));
    auto* details = new QPlainTextEdit(dialog);
    details->setObjectName(QStringLiteral("searchLogDetails"));
    details->setReadOnly(true);
    content->addWidget(list, 2);
    content->addWidget(details, 3);
    layout->addLayout(content);
    connect(list, &QListWidget::currentRowChanged, dialog, [this, list, details](int row) {
        if (row < 0 || static_cast<std::size_t>(row) >= search_log_entries_.size()) {
            details->clear();
            return;
        }
        const auto& entry = search_log_entries_[search_log_entries_.size() - 1 - static_cast<std::size_t>(row)];
        QStringList lines;
        lines << QStringLiteral("开始时间：%1").arg(entry.started_at)
              << QStringLiteral("查询：%1").arg(entry.query.isEmpty() ? QStringLiteral("<全部>") : entry.query)
              << QStringLiteral("文件夹：%1").arg(entry.folder.isEmpty() ? QStringLiteral("<所有已索引目录>") : entry.folder)
              << QStringLiteral("范围：%1").arg(entry.scope == 1 ? QStringLiteral("文件") : entry.scope == 2 ? QStringLiteral("文件夹") : QStringLiteral("全部"))
              << QStringLiteral("匹配路径：%1").arg(entry.match_path ? QStringLiteral("是") : QStringLiteral("否"))
              << QStringLiteral("包含隐藏项：%1").arg(entry.include_hidden ? QStringLiteral("是") : QStringLiteral("否"))
              << QStringLiteral("分页偏移：%1").arg(static_cast<qulonglong>(entry.offset))
              << QStringLiteral("状态：%1").arg(entry.state)
              << QStringLiteral("耗时：%1 ms").arg(entry.elapsed_ms, 0, 'f', 1)
              << QStringLiteral("核心返回：%1").arg(entry.returned_count)
              << QStringLiteral("当前已加载：%1").arg(entry.loaded_count)
              << (entry.sync_failure_count > 0
                      ? QStringLiteral("索引更新：%1 个范围正在重试或未更新；缓存结果可能过期，详情见“工具 → 索引状态”").arg(entry.sync_failure_count)
                      : QStringLiteral("索引同步：搜索开始时无失败记录"));
        if (!entry.detail.isEmpty()) lines << QStringLiteral("详情：%1").arg(entry.detail);
        details->setPlainText(lines.join(QLatin1Char('\n')));
    });
    auto* buttons = new QHBoxLayout;
    auto* clear = new QPushButton(QStringLiteral("清空日志"), dialog);
    clear->setObjectName(QStringLiteral("clearSearchLog"));
    connect(clear, &QPushButton::clicked, dialog, [this] {
        search_log_entries_.clear();
        refreshSearchLogDialog();
    });
    buttons->addWidget(clear);
    buttons->addStretch();
    auto* close = new QPushButton(QStringLiteral("关闭"), dialog);
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    buttons->addWidget(close);
    layout->addLayout(buttons);
    refreshSearchLogDialog();
    dialog->show();
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

bool MainWindow::allowCachedAction(int row) {
    const auto reason = model_->actionBlockAt(row);
    if (reason.isEmpty()) return true;
    statusBar()->showMessage(reason, 10000);
    return false;
}

QStringList MainWindow::actionableSelectedPaths() {
    QStringList paths;
    if (!table_->selectionModel()) return paths;
    auto rows = table_->selectionModel()->selectedRows(0);
    std::sort(rows.begin(), rows.end(), [](const QModelIndex& a, const QModelIndex& b) { return a.row() < b.row(); });
    for (const auto& row : rows) if (allowCachedAction(row.row())) paths << model_->pathAt(row.row());
    return paths;
}

void MainWindow::openCurrent() {
    const auto paths = actionableSelectedPaths();
    if (paths.isEmpty()) return;
    for (const auto& path : paths.mid(0, 20)) desktop_actions_->open(path);
    if (paths.size() > 20) statusBar()->showMessage(QStringLiteral("一次最多打开前 20 个所选项目。"), 4000);
}

void MainWindow::openWithCurrent() {
    const auto paths = actionableSelectedPaths();
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
    if (!current.isValid() || !allowCachedAction(current.row())) return;
    const auto path = model_->pathAt(current.row());
    if (path.isEmpty()) return;
    desktop_actions_->reveal(path);
}

void MainWindow::quickLookCurrent() {
    const auto current = table_->currentIndex();
    if (!current.isValid() || !allowCachedAction(current.row())) return;
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
    out << "Name,Path,Size,Modified,Source,Status\n";
    for (int row = 0; row < model_->rowCount(); ++row) {
        out << csvQuote(model_->data(model_->index(row, 0), Qt::DisplayRole).toString()) << ','
            << csvQuote(model_->pathAt(row)) << ','
            << csvQuote(model_->data(model_->index(row, 2), Qt::DisplayRole).toString()) << ','
            << csvQuote(model_->data(model_->index(row, 3), Qt::DisplayRole).toString()) << ','
            << csvQuote(model_->data(model_->index(row, 4), Qt::DisplayRole).toString()) << ','
            << csvQuote(model_->data(model_->index(row, 5), Qt::DisplayRole).toString()) << '\n';
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

void MainWindow::restartWatcher(bool refresh_stats) {
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
    watcher_error_label_->setText(watcher_error_.isEmpty() ? QStringLiteral("目录监听正常") : QStringLiteral("目录监听异常"));
    watcher_error_label_->setToolTip(watcher_error_.isEmpty()
        ? QStringLiteral("监听 %1 个索引目录的文件变化；后端：%2").arg(roots_.size()).arg(fromUtf8(watcher_->backendName()))
        : watcher_error_);
    watcher_error_label_->setStyleSheet(watcher_error_.isEmpty() ? QString{} : QStringLiteral("color: #b33"));
    if (refresh_stats) refreshStats();
    else updateCachedStatus();
}

void MainWindow::receiveWatcherEvent(std::uint64_t generation, const FileEvent& event) {
    if (generation != watcher_generation_) return;
    scheduleFileEvent(event);
}

void MainWindow::setBusy(bool busy, const QString& message) {
    indexing_ = busy;
    if (!message.isEmpty() && !closing_ && !userSearchOwnsStatus()) status_label_->setText(message);
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
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Interactive);
    table_->setColumnWidth(4, 150);
    table_->setColumnWidth(5, 170);
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
    bookmark.folder_scope = folder_scope_;
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
    setFolderScope(bookmark.folder_scope);
    table_->sortByColumn(std::clamp(bookmark.sort_column, 0, model_->columnCount() - 1), bookmark.sort_order);
    search_timer_->stop();
    updateSearchStateLabel();
    runSearch();
}

void MainWindow::showSearchSyntax() {
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("搜索语法"));
    box.setIcon(QMessageBox::Information);
    box.setText(QStringLiteral(
        "Everything Lite 搜索语法\n\n"
        "report                 名称包含 report\n"
        "ext:pdf                PDF 文件/文件夹名后缀过滤\n"
        "path:sample            完整路径包含 sample\n"
        "type:file              仅文件\n"
        "type:dir               仅文件夹\n"
        "size:>100m             大于 100 MB\n"
        "modified:7d            最近 7 天修改\n"
        "\"Example Collection\"   短语\n\n"
        "多个条件可组合。普通关键词默认只匹配名称；“搜索 → 匹配路径”开启后才匹配完整路径。\n"
        "“指定文件夹…”只搜索该目录下已有索引；排除规则在“工具”菜单设置。\n"
        "搜索框不支持通用正则；regex: 仅用于排除过滤器。"));
    box.exec();
}

void MainWindow::showAbout() {
    QMessageBox::about(this, QStringLiteral("关于 Everything Lite"),
        QStringLiteral("<b>Everything Lite %1</b><br><br>"
                       "跨平台本地文件名搜索工具。<br>"
                       "核心：C++17 + SQLite + PCRE2；界面：Qt 6；macOS 实时更新：FSEvents。<br><br>"
                       "Vibe Coding 项目；源码采用 MIT License。")
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
