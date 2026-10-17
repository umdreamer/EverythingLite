#pragma once
#include "core/index_storage.h"
#include <QDialog>
#include <functional>
class QLabel;
class QPushButton;
class QProgressBar;
class QThread;
class QListWidget;
namespace everything_lite {
// The MainWindow owns maintenance jobs; this dialog owns only its read-only report.
class IndexStorageDialog final : public QDialog {
public:
    explicit IndexStorageDialog(QString database_path, QWidget* parent=nullptr);
    ~IndexStorageDialog() override;
    std::function<void(bool)> maintenanceRequested;
    std::function<void()> cancellationRequested;
    std::function<void(const std::vector<std::string>&)> cleanupRequested;
    void refreshReport(bool update_summary=true);
    void beginOperation();
    void setPhase(IndexStoragePhase phase);
    void finishOperation(const QString& message,const IndexStorage* before=nullptr,const IndexStorage* after=nullptr);
protected:
    void reject() override;
    void closeEvent(QCloseEvent* event) override;
private:
    QString database_path_;
    QLabel* report_=nullptr;
    QLabel* outcome_=nullptr;
    QProgressBar* progress_=nullptr;
    QPushButton* optimize_=nullptr;
    QPushButton* compact_=nullptr;
    QPushButton* refresh_=nullptr;
    QPushButton* cancel_=nullptr;
    QPushButton* cleanup_=nullptr;
    QListWidget* backups_=nullptr;
    void requestCleanup();
    QThread* report_thread_=nullptr;
    std::uint64_t report_generation_=0;
    bool operation_running_=false;
    bool close_when_finished_=false;
    void requestMaintenance(bool optimize);
};
QString indexStorageSummary(const IndexStorage& value);
} // namespace everything_lite
