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

class QCloseEvent;
class QLabel;
class QLineEdit;
class QPoint;
class QPushButton;
class QTableView;
class QThread;
class QTimer;
template <typename T> class QFutureWatcher;

namespace everything_lite {

class SearchResultModel;

struct SearchOutcome {
    std::vector<SearchResult> results;
    QString query;
    QString error;
    double elapsed_ms = 0.0;
};

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    QString db_path_;
    QStringList roots_;
    std::unique_ptr<FileWatcher> watcher_;

    QLineEdit* search_edit_ = nullptr;
    QPushButton* roots_button_ = nullptr;
    QPushButton* rebuild_button_ = nullptr;
    QLabel* scope_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QTableView* table_ = nullptr;
    SearchResultModel* model_ = nullptr;
    QTimer* search_timer_ = nullptr;
    QTimer* event_timer_ = nullptr;
    QThread* worker_thread_ = nullptr;
    QFutureWatcher<SearchOutcome>* search_watcher_ = nullptr;

    QSet<QString> pending_paths_;
    QSet<QString> pending_rescan_roots_;
    QString pending_search_query_;
    bool search_pending_ = false;
    std::atomic_bool indexing_{false};
    std::uint64_t indexed_count_ = 0;

    void buildUi();
    void loadSettings();
    void saveSettings();
    void updateScopeLabel();
    void refreshStats();
    void runSearch();
    void launchSearch(const QString& query);
    void rebuildIndex();
    void editRoots();
    void openCurrent();
    void revealCurrent();
    void copyCurrentPath();
    void copyCurrentName();
    void showContextMenu(const QPoint& pos);
    void restartWatcher();
    void scheduleFileEvent(const FileEvent& event);
    void processPendingEvents();
    QString rootForPath(const QString& path) const;
    void setBusy(bool busy, const QString& message = {});
    void startWorker(std::function<void()> job, const QString& start_message);
};

} // namespace everything_lite
