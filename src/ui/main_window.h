#pragma once

#include "core/file_record.h"
#include "platform/file_watcher.h"

#include <QMainWindow>
#include <QLabel>
#include <QPointer>
#include <QSet>
#include <QHash>
#include <QDeadlineTimer>
#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QAction;
class QActionGroup;
class QCheckBox;
class QCloseEvent;
class QDialog;
class QEvent;
class QObject;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QMenu;
class QPoint;
class QTableView;
class QThread;
class QTimer;
class QWidget;
class QPaintEvent;

namespace everything_lite {

class ElidedLabel final : public QLabel {
public:
    using QLabel::QLabel;
    void setText(const QString& value) {
        QLabel::setText(value);
        setToolTip(value);
    }
    QString displayText() const;
protected:
    void paintEvent(QPaintEvent* event) override;
};

class DesktopActions;
class SearchResultModel;
class SearchWorker;
class IndexStorageDialog;


struct SearchBookmark {
    QString name;
    QString query;
    QString folder_scope;
    int scope = 0;
    bool match_path = false;
    int sort_column = 0;
    Qt::SortOrder sort_order = Qt::AscendingOrder;
};

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    friend class UiTest;
    static constexpr std::size_t kPageSize = 1000;
    static constexpr int kSearchIdleDelayMs = 800;
    static constexpr std::size_t kMaxSearchLogEntries = 100;

    struct SearchLogEntry {
        std::uint64_t request_id = 0;
        QString started_at;
        QString query;
        QString folder;
        int scope = 0;
        bool match_path = false;
        bool include_hidden = false;
        std::size_t offset = 0;
        QString state;
        QString detail;
        double elapsed_ms = 0.0;
        int returned_count = 0;
        int loaded_count = 0;
        int sync_failure_count = 0;
    };
    std::vector<SearchLogEntry> search_log_entries_;
    QPointer<QDialog> search_log_dialog_;

    QString db_path_;
    QStringList roots_;
    std::unique_ptr<FileWatcher> watcher_;
    SearchWorker* search_worker_ = nullptr;
    QThread* search_thread_ = nullptr;

    QLineEdit* search_edit_ = nullptr;
    QString folder_scope_;
    bool folder_scope_index_pending_ = false;
    bool folder_scope_index_failed_ = false;
    QLabel* folder_scope_label_ = nullptr;
    QLabel* folder_scope_status_ = nullptr;
    QPushButton* clear_folder_scope_ = nullptr;
    QComboBox* scope_combo_ = nullptr;
    QCheckBox* match_path_check_ = nullptr;
    QCheckBox* include_hidden_check_ = nullptr;
    QLabel* scope_label_ = nullptr;
    ElidedLabel* status_label_ = nullptr;
    ElidedLabel* search_metrics_label_ = nullptr;
    ElidedLabel* watcher_error_label_ = nullptr;
    QString watcher_error_;
    ElidedLabel* sync_error_label_ = nullptr;
    struct SyncFailureScope {
        QString root;
        QString path;
        bool rescan = false;
    };
    QHash<QString, SyncFailureScope> sync_failure_scopes_;
    QHash<QString, int> sync_attempts_;
    QHash<QString, QDeadlineTimer> retry_deadlines_;
    QSet<QString> ready_sync_retries_;
    QHash<QString, QString> sync_errors_;
    QSet<QString> dormant_paths_;
    QSet<QString> dormant_rescan_roots_;
    std::uint64_t watcher_generation_ = 0;
    DesktopActions* desktop_actions_ = nullptr;
    ElidedLabel* search_state_label_ = nullptr;
    QTableView* table_ = nullptr;
    SearchResultModel* model_ = nullptr;
    QTimer* search_timer_ = nullptr;
    QTimer* event_timer_ = nullptr;
    QTimer* retry_timer_ = nullptr;
    QTimer* filesystem_refresh_timer_ = nullptr;
    QThread* worker_thread_ = nullptr;
    QPointer<IndexStorageDialog> storage_dialog_;
    bool storage_maintenance_=false;
    bool storage_optimize_=false;
    std::vector<std::string> storage_cleanup_selection_;
    std::shared_ptr<std::atomic_bool> storage_cancellation_;
    void showIndexStorage();
    void showExclusionSettings();
    void requestStorageMaintenance(bool optimize,const std::vector<std::string>& cleanup_selection={});
    void tryStartStorageMaintenance();

    QMenu* bookmarks_menu_ = nullptr;
    QAction* match_path_action_ = nullptr;
    QAction* scope_all_action_ = nullptr;
    QAction* scope_files_action_ = nullptr;
    QAction* scope_folders_action_ = nullptr;
    QAction* show_status_bar_action_ = nullptr;
    QAction* show_scope_bar_action_ = nullptr;
    QAction* show_filter_bar_action_ = nullptr;
    QAction* add_bookmark_action_ = nullptr;
    QAction* organize_bookmarks_action_ = nullptr;
    QActionGroup* scope_action_group_ = nullptr;

    std::vector<SearchBookmark> bookmarks_;
    QSet<QString> pending_paths_;
    QSet<QString> pending_rescan_roots_;
    QString pending_search_query_;
    bool results_have_more_ = false;
    std::size_t next_search_offset_ = 0;
    std::optional<std::uint64_t> search_revision_;
    QString pagination_signature_;
    bool search_ime_composing_ = false;
    bool search_in_flight_ = false;
    bool search_pending_ = false;
    bool filesystem_results_dirty_ = false;
    bool closing_ = false;
    bool normal_events_due_ = false;
    std::shared_ptr<std::atomic_bool> active_search_cancellation_;
    bool name_search_available_ = false;
    bool name_search_ready_ = false;
    std::atomic_bool indexing_{false};
    std::uint64_t indexed_count_ = 0;
    std::uint64_t search_request_id_ = 0;
    QString last_search_query_;
    double last_search_elapsed_ms_ = 0.0;
    int last_search_result_count_ = 0;
    int last_search_raw_result_count_ = 0;
    QString last_search_utf8_hex_;
    QString last_search_execution_mode_;
    std::uint64_t last_search_request_id_ = 0;

    void buildUi();
    void buildMenus();
    void loadSettings();
    void saveSettings();
    void loadBookmarks();
    void saveBookmarks();
    void rebuildBookmarksMenu();
    void updateScopeLabel();
    void setFolderScope(const QString& path);
    void selectFolderScope(const QString& path);
    void updateFolderScopeLabel();
    void updateSearchStateLabel();
    void refreshStats();
    void updateCachedStatus();
    bool userSearchOwnsStatus() const;
    void runSearch();
    void launchSearch(const QString& query, std::size_t offset = 0, bool append = false);
    void cancelActiveSearch();
    void loadMoreResults();
    void resetPaging();
    QString currentSearchSignature() const;
    void rebuildIndex();
    void editRoots();
    void showIndexStatus();
    void showSettingsOverview();
    void showSearchDiagnostics();
    void showSearchLog();
    void refreshSearchLogDialog();
    void finishSearchLog(std::uint64_t request_id, const QString& state, const QString& detail,
                         double elapsed_ms, int returned_count, int loaded_count);
    void openCurrent();
    void openWithCurrent();
    void revealCurrent();
    void quickLookCurrent();
    void copyCurrentPath();
    void copyCurrentName();
    void exportLoadedResults();
    void showContextMenu(const QPoint& pos);
    void restartWatcher(bool refresh_stats = true);
    void receiveWatcherEvent(std::uint64_t generation, const FileEvent& event);
    void scheduleFileEvent(const FileEvent& event);
    void processPendingEvents();
    void dispatchSyncJobs();
    void processDueRetries();
    void armRetryTimer();
    void reportSyncOutcome(const QString& root, const QString& path, bool rescan, const QString& error);
    QString rootForPath(const QString& path) const;
    void setBusy(bool busy, const QString& message = {});
    void startWorker(std::function<void()> job, const QString& start_message, bool refresh_search_on_finish = true,
                     std::function<void(const QString&)> on_finished = {});
    void scheduleFilesystemResultRefresh();
    void newWindow();
    void resetColumns();
    void addCurrentBookmark();
    void organizeBookmarks();
    void applyBookmark(const SearchBookmark& bookmark);
    void showSearchSyntax();
    void showAbout();
    QStringList selectedPaths() const;
    QStringList actionableSelectedPaths();
    bool allowCachedAction(int row);
    QStringList selectedNames() const;
    void setScopeFromMenu(int scope_index);
    void syncSearchControlsFromMenus();
};

} // namespace everything_lite
