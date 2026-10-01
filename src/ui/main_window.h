#pragma once

#include "core/file_record.h"
#include "platform/file_watcher.h"

#include <QMainWindow>
#include <QSet>
#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class QAction;
class QActionGroup;
class QCheckBox;
class QCloseEvent;
class QEvent;
class QObject;
class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QPoint;
class QTableView;
class QThread;
class QTimer;
class QWidget;

namespace everything_lite {

class SearchResultModel;
class SearchWorker;


struct SearchBookmark {
    QString name;
    QString query;
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
    static constexpr std::size_t kPageSize = 1000;
    static constexpr int kSearchIdleDelayMs = 800;

    QString db_path_;
    QStringList roots_;
    std::unique_ptr<FileWatcher> watcher_;
    SearchWorker* search_worker_ = nullptr;
    QThread* search_thread_ = nullptr;

    QLineEdit* search_edit_ = nullptr;
    QComboBox* scope_combo_ = nullptr;
    QCheckBox* match_path_check_ = nullptr;
    QLabel* scope_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* search_state_label_ = nullptr;
    QTableView* table_ = nullptr;
    SearchResultModel* model_ = nullptr;
    QTimer* search_timer_ = nullptr;
    QTimer* event_timer_ = nullptr;
    QTimer* filesystem_refresh_timer_ = nullptr;
    QThread* worker_thread_ = nullptr;

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
    bool search_ime_composing_ = false;
    bool search_in_flight_ = false;
    bool search_pending_ = false;
    bool name_search_available_ = false;
    bool name_search_ready_ = false;
    std::atomic_bool indexing_{false};
    std::uint64_t indexed_count_ = 0;
    std::uint64_t name_search_count_ = 0;
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
    void updateSearchStateLabel();
    void refreshStats();
    void runSearch();
    void launchSearch(const QString& query, std::size_t offset = 0, bool append = false);
    void cancelActiveSearch();
    void loadMoreResults();
    QString currentSearchSignature() const;
    void rebuildIndex();
    void editRoots();
    void showIndexStatus();
    void showSearchDiagnostics();
    void openCurrent();
    void openWithCurrent();
    void revealCurrent();
    void quickLookCurrent();
    void copyCurrentPath();
    void copyCurrentName();
    void exportLoadedResults();
    void showContextMenu(const QPoint& pos);
    void restartWatcher();
    void scheduleFileEvent(const FileEvent& event);
    void processPendingEvents();
    QString rootForPath(const QString& path) const;
    void setBusy(bool busy, const QString& message = {});
    void startWorker(std::function<void()> job, const QString& start_message, bool refresh_search_on_finish = true);
    void scheduleFilesystemResultRefresh();
    void newWindow();
    void resetColumns();
    void addCurrentBookmark();
    void organizeBookmarks();
    void applyBookmark(const SearchBookmark& bookmark);
    void showSearchSyntax();
    void showAbout();
    QStringList selectedPaths() const;
    QStringList selectedNames() const;
    void setScopeFromMenu(int scope_index);
    void syncSearchControlsFromMenus();
};

} // namespace everything_lite
