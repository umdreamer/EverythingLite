#include "ui/main_window.h"

#include "core/database.h"
#include "core/index_manager.h"
#include "core/path_utils.h"
#include "core/search_engine.h"
#include "platform/watcher_factory.h"
#include "ui/root_settings_dialog.h"
#include "ui/search_result_model.h"

#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFont>
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

#include <algorithm>
#include <chrono>
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
    search_timer_->setInterval(120);
    connect(search_timer_, &QTimer::timeout, this, &MainWindow::runSearch);
    connect(search_edit_, &QLineEdit::textChanged, this, [this] { search_timer_->start(); });

    event_timer_ = new QTimer(this);
    event_timer_->setSingleShot(true);
    event_timer_->setInterval(700);
    connect(event_timer_, &QTimer::timeout, this, &MainWindow::processPendingEvents);

    connect(rebuild_button_, &QPushButton::clicked, this, &MainWindow::rebuildIndex);
    connect(roots_button_, &QPushButton::clicked, this, &MainWindow::editRoots);
    connect(table_, &QTableView::doubleClicked, this, [this] { openCurrent(); });

    auto* findShortcut = new QShortcut(QKeySequence::Find, this);
    connect(findShortcut, &QShortcut::activated, this, [this] {
        search_edit_->setFocus();
        search_edit_->selectAll();
    });

    auto* revealAction = new QAction(QStringLiteral("在 Finder 中显示"), this);
    table_->addAction(revealAction);
    table_->setContextMenuPolicy(Qt::ActionsContextMenu);
    connect(revealAction, &QAction::triggered, this, &MainWindow::revealCurrent);

    auto* openAction = new QAction(QStringLiteral("打开"), this);
    openAction->setShortcut(QKeySequence(Qt::Key_Return));
    openAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    table_->addAction(openAction);
    connect(openAction, &QAction::triggered, this, &MainWindow::openCurrent);

    refreshStats();
    restartWatcher();
    search_edit_->setFocus();
    runSearch();
}

MainWindow::~MainWindow() {
    if (watcher_) watcher_->stop();
    if (worker_thread_) {
        worker_thread_->quit();
        worker_thread_->wait();
        delete worker_thread_;
        worker_thread_ = nullptr;
    }
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("Everything Lite"));
    resize(1120, 720);
    setMinimumSize(760, 460);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(14, 12, 14, 8);
    layout->setSpacing(8);

    auto* top = new QHBoxLayout();
    top->setSpacing(8);

    search_edit_ = new QLineEdit(this);
    search_edit_->setPlaceholderText(QStringLiteral("搜索文件名或路径…  支持空格分词和 \"短语\""));
    search_edit_->setClearButtonEnabled(true);
    search_edit_->setMinimumHeight(38);
    QFont search_font = search_edit_->font();
    search_font.setPointSizeF(search_font.pointSizeF() + 1.5);
    search_edit_->setFont(search_font);

    roots_button_ = new QPushButton(QStringLiteral("索引目录…"), this);
    rebuild_button_ = new QPushButton(QStringLiteral("重建索引"), this);
    roots_button_->setMinimumHeight(36);
    rebuild_button_->setMinimumHeight(36);

    top->addWidget(search_edit_, 1);
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
    table_->setSortingEnabled(false);
    table_->setShowGrid(false);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(26);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionsMovable(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    table_->setColumnWidth(0, 280);
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
}

void MainWindow::saveSettings() {
    QSettings settings(QStringLiteral("CICHI"), QStringLiteral("EverythingLite"));
    settings.setValue(QStringLiteral("index/roots"), roots_);
}

void MainWindow::updateScopeLabel() {
    if (roots_.isEmpty()) {
        scope_label_->setText(QStringLiteral("未设置索引目录"));
        return;
    }
    const auto preview = roots_.size() <= 3 ? roots_.join(QStringLiteral("    "))
                                            : roots_.mid(0, 3).join(QStringLiteral("    ")) + QStringLiteral("    …");
    scope_label_->setText(QStringLiteral("索引范围：%1").arg(preview));
}

void MainWindow::refreshStats() {
    try {
        Database db(toUtf8(db_path_));
        db.initialize();
        indexed_count_ = db.totalFileCount();
        status_label_->setText(QStringLiteral("索引 %1 项    ·    %2")
            .arg(indexed_count_)
            .arg(watcher_ ? fromUtf8(watcher_->backendName()) : QStringLiteral("等待启动")));
    } catch (const std::exception& e) {
        status_label_->setText(QStringLiteral("数据库错误：%1").arg(QString::fromUtf8(e.what())));
    }
}

void MainWindow::runSearch() {
    if (indexing_.load() && indexed_count_ == 0) return;
    try {
        QElapsedTimer timer;
        timer.start();
        SearchEngine engine(toUtf8(db_path_));
        auto results = engine.search(toUtf8(search_edit_->text()), 500);
        const auto result_count = results.size();
        model_->setResults(std::move(results));
        const double ms = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
        status_label_->setText(QStringLiteral("索引 %1 项    ·    结果 %2    ·    %3 ms    ·    %4")
            .arg(indexed_count_)
            .arg(result_count)
            .arg(ms, 0, 'f', 1)
            .arg(watcher_ ? fromUtf8(watcher_->backendName()) : QStringLiteral("未监听")));
    } catch (const std::exception& e) {
        status_label_->setText(QStringLiteral("搜索错误：%1").arg(QString::fromUtf8(e.what())));
    }
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
                    status_label_->setText(QStringLiteral("正在索引：%1    %2 项")
                        .arg(fromUtf8(root)).arg(count));
                }, Qt::QueuedConnection);
            });
        } catch (const std::exception& e) {
            const auto message = QString::fromUtf8(e.what());
            QMetaObject::invokeMethod(this, [this, message] {
                QMessageBox::critical(this, QStringLiteral("索引失败"), message);
            }, Qt::QueuedConnection);
        }
    }, QStringLiteral("正在准备索引…"));
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
    startWorker([this, db_path, paths, rescans] {
        try {
            IndexManager manager(toUtf8(db_path));
            for (const auto& root : rescans) {
                manager.rebuildRoot(toUtf8(root));
            }
            for (const auto& path : paths) {
                const auto root = rootForPath(path);
                if (!root.isEmpty() && !rescans.contains(root)) {
                    manager.applyPathChange(toUtf8(root), toUtf8(path));
                }
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
