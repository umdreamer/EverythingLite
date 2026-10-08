#include "ui/main_window.h"
#include "core/index_manager.h"
#include "core/path_utils.h"

#include <QLabel>
#include <QMetaObject>
#include <QStatusBar>
#include <QTimer>
#include <exception>

namespace everything_lite {
namespace {
std::string native(const QString& text) {
    const auto bytes = text.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

QString failureKey(const QString& root, const QString& path, bool rescan) {
    // Root length makes this identity unambiguous even for legal separators
    // inside a filename. Scope association itself uses structured fields.
    return QStringLiteral("%1:%2:%3:%4").arg(root.size()).arg(root).arg(rescan ? 1 : 0).arg(path);
}
} // namespace

void MainWindow::scheduleFileEvent(const FileEvent& event) {
    if (closing_) return;
    const auto error = !event.error.empty() ? QString::fromUtf8(event.error.c_str())
        : watcher_ ? QString::fromUtf8(watcher_->lastError().c_str()) : QString{};
    if (!error.isEmpty()) {
        watcher_error_ = error;
        watcher_error_label_->setText(error);
        watcher_error_label_->show();
    }
    if (event.path.empty()) return;
    const auto path = QString::fromUtf8(event.path.c_str());
    const auto root = rootForPath(path);
    if (root.isEmpty()) return;
    if (event.needs_full_rescan && error.isEmpty()) {
        watcher_error_.clear();
        watcher_error_label_->clear();
        watcher_error_label_->hide();
    }

    // Only a related subtree event renews its failed job's retry budget.
    // A full-source failure needs an explicit full/root event (or manual retry),
    // so unrelated continuous changes cannot wake an offline source forever.
    const bool root_event = pathIsWithin(native(path), native(root))
        && pathIsWithin(native(root), native(path));
    for (auto it = sync_failure_scopes_.begin(); it != sync_failure_scopes_.end(); ++it) {
        const auto& scope = it.value();
        if (scope.root != root) continue;
        const bool related = event.needs_full_rescan || (scope.rescan ? root_event
            : pathIsWithin(native(path), native(scope.path)) || pathIsWithin(native(scope.path), native(path)));
        if (!related) continue;
        sync_attempts_[it.key()] = 0;
        if (scope.rescan) {
            if (dormant_rescan_roots_.remove(scope.root)) pending_rescan_roots_.insert(scope.root);
        } else if (dormant_paths_.remove(scope.path)) {
            pending_paths_.insert(scope.path);
        }
    }
    if (event.needs_full_rescan) {
        pending_rescan_roots_.insert(root);
    } else if (!pending_rescan_roots_.contains(root)) {
        bool covered = false;
        for (auto it = pending_paths_.begin(); it != pending_paths_.end();) {
            // Never let an ancestor from another nested source absorb its jobs.
            if (rootForPath(*it) != root) {
                ++it;
                continue;
            }
            if (pathIsWithin(native(path), native(*it))) {
                covered = true;
                break;
            }
            if (pathIsWithin(native(*it), native(path))) it = pending_paths_.erase(it);
            else ++it;
        }
        if (!covered) pending_paths_.insert(path);
        if (pending_paths_.size() > 400) {
            for (const auto& pending : pending_paths_) pending_rescan_roots_.insert(rootForPath(pending));
            pending_paths_.clear();
        }
    }
    // The first event fixes the deadline. Continuous events cannot starve sync.
    if (!event_timer_->isActive()) event_timer_->start(700);
}

void MainWindow::processPendingEvents() {
    if (closing_) return;
    if (worker_thread_) {
        events_due_while_busy_ = true;
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
        auto process = [&](const QString& root, const QString& path, bool rescan) {
            QString error;
            try {
                IndexManager manager(native(db_path));
                const auto stats = rescan ? manager.rebuildRoot(native(root))
                    : manager.applyPathChange(native(root), native(path));
                if (!stats.completed) {
                    error = stats.cancelled ? QStringLiteral("同步已取消")
                        : stats.error.empty() ? QStringLiteral("扫描未完整完成") : QString::fromUtf8(stats.error.c_str());
                }
            } catch (const std::exception& e) {
                error = QString::fromUtf8(e.what());
            } catch (...) {
                error = QStringLiteral("未知同步错误");
            }
            QMetaObject::invokeMethod(this, [this, root, path, rescan, error] {
                if (!closing_) reportSyncOutcome(root, path, rescan, error);
            }, Qt::QueuedConnection);
        };
        for (const auto& root : rescans) process(root, root, true);
        for (const auto& path : paths) {
            QString best;
            for (const auto& root : roots) {
                if (pathIsWithin(native(path), native(root)) && root.size() > best.size()) best = root;
            }
            if (!best.isEmpty() && !rescans.contains(best)) process(best, path, false);
        }
    }, QStringLiteral("正在同步文件变化…"), false);
}

void MainWindow::reportSyncOutcome(const QString& root, const QString& path, bool rescan, const QString& error) {
    const auto key = failureKey(root, path, rescan);
    if (error.isEmpty()) {
        sync_attempts_.remove(key);
        sync_errors_.remove(key);
        sync_failure_scopes_.remove(key);
        if (rescan) {
            // A complete source snapshot also resolves failed subtree jobs.
            for (auto it = sync_errors_.begin(); it != sync_errors_.end();) {
                if (sync_failure_scopes_.value(it.key()).root == root) {
                    sync_attempts_.remove(it.key());
                    sync_failure_scopes_.remove(it.key());
                    it = sync_errors_.erase(it);
                } else ++it;
            }
            dormant_rescan_roots_.remove(root);
            for (auto it = dormant_paths_.begin(); it != dormant_paths_.end();) {
                if (rootForPath(*it) == root) it = dormant_paths_.erase(it);
                else ++it;
            }
        }
    } else {
        sync_errors_[key] = QStringLiteral("%1：%2").arg(path, error);
        sync_failure_scopes_[key] = {root, path, rescan};
        const auto attempts = ++sync_attempts_[key];
        if (attempts < 3) {
            if (rescan) pending_rescan_roots_.insert(root);
            else pending_paths_.insert(path);
            if (!event_timer_->isActive()) event_timer_->start(1500 * attempts);
        } else {
            if (rescan) dormant_rescan_roots_.insert(root);
            else dormant_paths_.insert(path);
        }
    }
    const auto message = sync_errors_.isEmpty() ? QString{}
        : QStringLiteral("同步失败（保留缓存；最多自动重试两次，新事件可重试）：%1")
            .arg(sync_errors_.values().join(QStringLiteral("；")));
    sync_error_label_->setText(message);
    sync_error_label_->setVisible(!message.isEmpty());
    if (!message.isEmpty()) statusBar()->showMessage(message);
    else statusBar()->clearMessage();
}

QString MainWindow::rootForPath(const QString& path) const {
    QString best;
    for (const auto& root : roots_) {
        if (pathIsWithin(native(path), native(root)) && root.size() > best.size()) best = root;
    }
    return best;
}
} // namespace everything_lite
