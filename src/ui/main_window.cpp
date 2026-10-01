#include "ui/main_window.h"

#include "core/database.h"
#include "core/index_manager.h"
#include "core/path_utils.h"
#include "core/search_engine.h"
#include "core/search_query.h"
#include "platform/watcher_factory.h"
#include "ui/root_settings_dialog.h"
#include "ui/search_result_model.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFont>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableView>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrent>

#include <algorithm>
#include <exception>
#include <filesystem>
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

QStringList defaultRoots() {
    QStringList roots;
    const auto home = QDir::homePath();
    for (const auto& name : {QStringLiteral("Desktop"), QStringLiteral("Documents"), QStringLiteral("Downloads")}) {
        const auto path = QDir(home).filePath(name);
        if (QFileInfo::exists(path)) roots << QDir::cleanPath(path);
    }
    if (roots.isEmpty()) roots << home;
    return roots;
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    const auto data_dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(data_dir);
    db_path_ = QDir(data_dir).filePath(QStringLiteral("everything-lite.db"));

    buildUi();
    loadSettings();
    updateScopeLabel();

    search_timer_ = new QTimer(this);
    search_timer_->setSingleShot(true);
    search_timer_->setInterval(100);
    connect(search_timer_, &QTimer::timeout, this, &MainWindow::runSearch);
    connect(search_edit_, &QLineEdit::textChanged, this, [this] { search_timer_->start(); });
    connect(scope_combo_, &QComboBox::currentIndexChanged, this, [this] { search_timer_->start(); });
    connect(match_path_check_, &QCheckBox::toggled, this, [this] { search_timer_->start(); });

    event_timer_ = new QTimer(this);
    event_timer_->setSingleShot(true);
    event_timer_->setInterval(700);
    connect(event_timer_, &QTimer::timeout, this, &MainWindow::processPendingEvents);

    search_watcher_ = new QFutureWatcher<SearchOutcome>(this);
    connect(search_watcher_, &QFutureWatcher<SearchOutcome>::finished, this, [this] {
        auto outcome = search_watcher_->result();
        const bool still_current = outcome.signature == currentSearchSignature();
        if (!outcome.error.isEmpty()) {
            if (still_current) status_label_->setText(QStringLiteral("搜索错误：%1").arg(outcome.error));
        } else if (!search_pending_ && still_current) {
            if (outcome.append) model_->appendResults(std::move(outcome.results));
            else model_->setResults(std::move(outcome.results));
            results_have_more_ = outcome.has_more;
            const auto loaded = model_->rowCount();
            status_label_->setText(QStringLiteral("索引 %1 项    ·    已加载 %2%3    ·    %4 ms    ·    %5%6")
                .arg(indexed_count_)
                .arg(loaded)
                .arg(results_have_more_ ? QStringLiteral("+") : QString{})
                .arg(outcome.elapsed_ms, 0, 'f', 1)
                .arg(watcher_ ? fromUtf8(watcher_->backendName()) : QStringLiteral("未监听"))
                .arg(name_search_ready_ ? QString{} : QStringLiteral("    ·    名称加速索引未就绪")));
        }

        if (search_pending_ || !still_current) {
            search_pending_ = false;
            launchSearch(search_edit_->text(), 0, false);
        }
    });

    connect(rebuild_button_, &QPushButton::clicked, this, &MainWindow::rebuildIndex);
    connect(roots_button_, &QPushButton::clicked, this, &MainWindow::editRoots);
    connect(table_, &QTableView::doubleClicked, this, [this] { openCurrent(); });
    connect(table_, &QWidget::customContextMenuRequested, this, &MainWindow::showContextMenu);
    connect(table_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        auto* bar = table_->verticalScrollBar();
        if (results_have_more_ && value >= bar->maximum() - 8) loadMoreResults();
    });

    auto* findShortcut = new QShortcut(QKeySequence::Find, this);
    connect(findShortcut, &QShortcut::activated, this, [this] {
        search_edit_->setFocus();
        search_edit_->selectAll();
    });

    auto* openShortcut = new QShortcut(QKeySequence(Qt::Key_Return), table_);
    connect(openShortcut, &QShortcut::activated, this, &MainWindow::openCurrent);

    refreshStats();
    restartWatcher();

    // v0.3 upgrades an existing v0.2 database in-place. Building the trigram
    // index reads the existing SQLite rows only; it does not rescan the disk.
    if (indexed_count_ > 0 && name_search_available_ && !name_search_ready_) {
        const auto db_path = db_path_;
        startWorker([db_path] {
            Database db(toUtf8(db_path));
            db.initialize();
            db.rebuildNameSearchIndex();
        }, QStringLiteral("首次升级：正在从现有数据库建立名称加速索引（无需重新扫描磁盘）…"));
    }

    search_edit_->setFocus();
    runSearch();
}

MainWindow::~MainWindow() {
    if (watcher_) watcher_->stop();
    if (search_watcher_ && search_watcher_->isRunning()) search_watcher_->waitForFinished();
    if (worker_thread_) {
        worker_thread_->quit();
        worker_thread_->wait();
        delete worker_thread_;
        worker_thread_ = nullptr;
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveSettings();
    QMainWindow::closeEvent(event);
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("Everything Lite"));
    resize(1160, 740);
    setMinimumSize(820, 480);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(14, 12, 14, 8);
    layout->setSpacing(8);

    auto* top = new QHBoxLayout();
    top->setSpacing(8);

    search_edit_ = new QLineEdit(this);
    search_edit_->setPlaceholderText(QStringLiteral("搜索名称…  例：report  ext:pdf  path:sample  size:>10m  modified:7d"));
    search_edit_->setClearButtonEnabled(true);
    search_edit_->setMinimumHeight(38);
    QFont search_font = search_edit_->font();
    search_font.setPointSizeF(search_font.pointSizeF() + 1.5);
    search_edit_->setFont(search_font);

    scope_combo_ = new QComboBox(this);
    scope_combo_->addItem(QStringLiteral("全部"));
    scope_combo_->addItem(QStringLiteral("仅文件"));
    scope_combo_->addItem(QStringLiteral("仅文件夹"));
    scope_combo_->setMinimumHeight(36);
    scope_combo_->setMinimumWidth(92);

    match_path_check_ = new QCheckBox(QStringLiteral("匹配路径"), this);
    match_path_check_->setToolTip(QStringLiteral("关闭时普通关键词只匹配文件/文件夹名称；打开后匹配完整路径"));

    roots_button_ = new QPushButton(QStringLiteral("索引目录…"), this);
    rebuild_button_ = new QPushButton(QStringLiteral("重建索引"), this);
    roots_button_->setMinimumHeight(36);
    rebuild_button_->setMinimumHeight(36);

    top->addWidget(search_edit_, 1);
    top->addWidget(scope_combo_);
    top->addWidget(match_path_check_);
    top->addWidget(roots_button_);
    top->addWidget(rebuild_button_);
    layout->addLayout(top);

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
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setAlternatingRowColors(true);
    table_->setSortingEnabled(true);
    table_->sortByColumn(0, Qt::AscendingOrder);
    table_->setShowGrid(false);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(26);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionsMovable(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    table_->setColumnWidth(0, 300);
    table_->setColumnWidth(2, 90);
    table_->setColumnWidth(3, 150);
    layout->addWidget(table_, 1);

    setCentralWidget(central);

    status_label_ = new QLabel(this);
    statusBar()->addPermanentWidget(status_label_, 1);
    statusBar()->setSizeGripEnabled(false);
}

void MainWindow::loadSettings() {
    QSettings settings(QStringLiteral("CICHI"), QStringLiteral("EverythingLite"));
    roots_ = settings.value(QStringLiteral("index/roots")).toStringList();
    if (roots_.isEmpty()) roots_ = defaultRoots();
    scope_combo_->setCurrentIndex(std::clamp(settings.value(QStringLiteral("search/itemScope"), 0).toInt(), 0, 2));
    match_path_check_->setChecked(settings.value(QStringLiteral("search/matchPath"), false).toBool());
    const auto geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    const auto header_state = settings.value(QStringLiteral("table/header")).toByteArray();
    if (!header_state.isEmpty()) table_->horizontalHeader()->restoreState(header_state);
    const int sort_column = settings.value(QStringLiteral("table/sortColumn"), 0).toInt();
    const auto sort_order = static_cast<Qt::SortOrder>(settings.value(QStringLiteral("table/sortOrder"), static_cast<int>(Qt::AscendingOrder)).toInt());
    table_->sortByColumn(sort_column, sort_order);
}

void MainWindow::saveSettings() {
    QSettings settings(QStringLiteral("CICHI"), QStringLiteral("EverythingLite"));
    settings.setValue(QStringLiteral("index/roots"), roots_);
    settings.setValue(QStringLiteral("search/itemScope"), scope_combo_->currentIndex());
    settings.setValue(QStringLiteral("search/matchPath"), match_path_check_->isChecked());
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("table/header"), table_->horizontalHeader()->saveState());
    settings.setValue(QStringLiteral("table/sortColumn"), table_->horizontalHeader()->sortIndicatorSection());
    settings.setValue(QStringLiteral("table/sortOrder"), static_cast<int>(table_->horizontalHeader()->sortIndicatorOrder()));
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

void MainWindow::refreshStats() {
    try {
        Database db(toUtf8(db_path_));
        db.initialize();
        indexed_count_ = db.totalFileCount();
        name_search_available_ = db.nameSearchIndexAvailable();
        name_search_ready_ = db.nameSearchIndexReady();
        QString acceleration;
        if (!name_search_available_) acceleration = QStringLiteral("    ·    当前 SQLite 未提供 FTS5 trigram，使用兼容搜索");
        else if (!name_search_ready_) acceleration = QStringLiteral("    ·    名称加速索引未就绪");
        status_label_->setText(QStringLiteral("索引 %1 项    ·    %2%3")
            .arg(indexed_count_)
            .arg(watcher_ ? fromUtf8(watcher_->backendName()) : QStringLiteral("等待启动"))
            .arg(acceleration));
    } catch (const std::exception& e) {
        status_label_->setText(QStringLiteral("数据库错误：%1").arg(QString::fromUtf8(e.what())));
    }
}

QString MainWindow::currentSearchSignature() const {
    return QStringLiteral("%1\x1f%2\x1f%3")
        .arg(search_edit_->text())
        .arg(scope_combo_->currentIndex())
        .arg(match_path_check_->isChecked() ? 1 : 0);
}

void MainWindow::runSearch() {
    if (indexing_.load() && indexed_count_ == 0) return;
    pending_search_query_ = search_edit_->text();
    results_have_more_ = false;
    if (search_watcher_->isRunning()) {
        search_pending_ = true;
        return;
    }
    launchSearch(pending_search_query_, 0, false);
}

void MainWindow::loadMoreResults() {
    if (!results_have_more_ || search_pending_ || search_watcher_->isRunning()) return;
    launchSearch(search_edit_->text(), static_cast<std::size_t>(model_->rowCount()), true);
}

void MainWindow::launchSearch(const QString& query, std::size_t offset, bool append) {
    if (search_watcher_->isRunning()) {
        if (!append) {
            pending_search_query_ = query;
            search_pending_ = true;
        }
        return;
    }

    if (!append) {
        search_pending_ = false;
        pending_search_query_ = query;
    }

    const auto db_path = db_path_;
    const auto signature = currentSearchSignature();
    const int item_scope = scope_combo_->currentIndex();
    const bool match_path = match_path_check_->isChecked();

    search_watcher_->setFuture(QtConcurrent::run([db_path, query, signature, offset, append, item_scope, match_path] {
        SearchOutcome outcome;
        outcome.query = query;
        outcome.signature = signature;
        outcome.offset = offset;
        outcome.append = append;
        try {
            QElapsedTimer timer;
            timer.start();
            SearchEngine engine(toUtf8(db_path));
            auto parsed = parseSearchQuery(toUtf8(query));

            // Explicit type:file/type:dir in the query wins over the toolbar.
            if (!parsed.files_only && !parsed.directories_only) {
                if (item_scope == 1) parsed.files_only = true;
                else if (item_scope == 2) parsed.directories_only = true;
            }
            if (match_path) parsed.match_path = true;

            outcome.results = engine.search(parsed, kPageSize + 1, offset);
            if (outcome.results.size() > kPageSize) {
                outcome.has_more = true;
                outcome.results.resize(kPageSize);
            }
            outcome.elapsed_ms = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
        } catch (const std::exception& e) {
            outcome.error = QString::fromUtf8(e.what());
        }
        return outcome;
    }));
}

void MainWindow::startWorker(std::function<void()> job, const QString& start_message) {
    if (worker_thread_) return;
    setBusy(true, start_message);
    worker_thread_ = QThread::create([job = std::move(job)] { job(); });
    connect(worker_thread_, &QThread::finished, this, [this] {
        worker_thread_->deleteLater();
        worker_thread_ = nullptr;
        setBusy(false);
        refreshStats();
        runSearch();
        if (!pending_paths_.isEmpty() || !pending_rescan_roots_.isEmpty()) event_timer_->start();
    });
    worker_thread_->start();
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
    status_label_->setText(QStringLiteral("索引目录已修改。请点击“重建索引”同步数据库。"));
}

void MainWindow::openCurrent() {
    const auto current = table_->currentIndex();
    if (!current.isValid()) return;
    const auto path = model_->pathAt(current.row());
    if (!path.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::revealCurrent() {
    const auto current = table_->currentIndex();
    if (!current.isValid()) return;
    const auto path = model_->pathAt(current.row());
    if (path.isEmpty()) return;
#ifdef Q_OS_MACOS
    QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), path});
#else
    const auto target = model_->isDirectoryAt(current.row()) ? path : QFileInfo(path).absolutePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(target));
#endif
}

void MainWindow::copyCurrentPath() {
    const auto current = table_->currentIndex();
    if (!current.isValid()) return;
    const auto path = model_->pathAt(current.row());
    if (!path.isEmpty()) QApplication::clipboard()->setText(path);
}

void MainWindow::copyCurrentName() {
    const auto current = table_->currentIndex();
    if (!current.isValid()) return;
    const auto name = model_->nameAt(current.row());
    if (!name.isEmpty()) QApplication::clipboard()->setText(name);
}

void MainWindow::showContextMenu(const QPoint& pos) {
    const auto index = table_->indexAt(pos);
    if (!index.isValid()) return;
    table_->setCurrentIndex(index);
    table_->selectRow(index.row());

    QMenu menu(this);
    auto* open = menu.addAction(QStringLiteral("打开"));
    auto* reveal = menu.addAction(QStringLiteral("在 Finder 中显示"));
    menu.addSeparator();
    auto* copy_path = menu.addAction(QStringLiteral("复制完整路径"));
    auto* copy_name = menu.addAction(QStringLiteral("复制文件名"));
    const auto* selected = menu.exec(table_->viewport()->mapToGlobal(pos));
    if (selected == open) openCurrent();
    else if (selected == reveal) revealCurrent();
    else if (selected == copy_path) copyCurrentPath();
    else if (selected == copy_name) copyCurrentName();
}

void MainWindow::restartWatcher() {
    if (watcher_) watcher_->stop();
    watcher_ = createPlatformWatcher();
    std::vector<std::string> native_roots;
    for (const auto& root : roots_) native_roots.push_back(toUtf8(root));
    watcher_->setRoots(std::move(native_roots));
    watcher_->setCallback([this](const FileEvent& event) {
        QMetaObject::invokeMethod(this, [this, event] { scheduleFileEvent(event); }, Qt::QueuedConnection);
    });
    watcher_->start();
    refreshStats();
}

void MainWindow::scheduleFileEvent(const FileEvent& event) {
    if (event.path.empty()) return;
    const auto path = fromUtf8(event.path);
    const auto root = rootForPath(path);
    if (root.isEmpty()) return;
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
    }, QStringLiteral("正在同步文件变化…"));
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
    rebuild_button_->setEnabled(!busy);
    roots_button_->setEnabled(!busy);
    if (!message.isEmpty()) status_label_->setText(message);
}

} // namespace everything_lite
