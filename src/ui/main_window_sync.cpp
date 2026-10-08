#include "ui/main_window.h"
#include "core/index_manager.h"
#include "core/path_utils.h"

#include <QLabel>
#include <QMetaObject>
#include <QStatusBar>
#include <QTimer>
#include <exception>
#include <algorithm>
#include <limits>

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
        retry_deadlines_.remove(it.key());
        ready_sync_retries_.remove(it.key());
        if (scope.rescan) {
            if (dormant_rescan_roots_.remove(scope.root)) pending_rescan_roots_.insert(scope.root);
        } else if (dormant_paths_.remove(scope.path)) {
            pending_paths_.insert(scope.path);
        }
    }
    armRetryTimer();
    // Exhausted full-source work already retains every descendant change.
    // Only an explicit related root/full event above reactivates it; unrelated
    // sibling churn and overflow cannot create a fresh full-source job.
    if (dormant_rescan_roots_.contains(root)) return;
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
            for (const auto& pending : pending_paths_) {
                const auto affected_root = rootForPath(pending);
                const bool failed_full_source = std::any_of(sync_failure_scopes_.cbegin(), sync_failure_scopes_.cend(),
                    [&](const SyncFailureScope& scope) { return scope.root == affected_root && scope.rescan; });
                // Its scheduled retry or dormant full-source scope already
                // retains coverage. Overflow must not bypass that backoff.
                if (!failed_full_source) pending_rescan_roots_.insert(affected_root);
            }
            pending_paths_.clear();
        }
    }
    // The first event fixes the deadline. Continuous events cannot starve sync.
    if (!event_timer_->isActive()) event_timer_->start(700);
}

void MainWindow::processPendingEvents() {
    if (closing_) return;
    event_timer_->stop();
    normal_events_due_ = true;
    dispatchSyncJobs();
}

void MainWindow::armRetryTimer() {
    if (!retry_timer_ || closing_) return;
    if (retry_deadlines_.isEmpty()) {
        retry_timer_->stop();
        return;
    }
    qint64 remaining = std::numeric_limits<int>::max();
    for (const auto& deadline : retry_deadlines_) remaining = std::min(remaining, deadline.remainingTime());
    retry_timer_->start(static_cast<int>(std::max<qint64>(0, remaining)));
}

void MainWindow::processDueRetries() {
    if (closing_) return;
    for (auto it = retry_deadlines_.begin(); it != retry_deadlines_.end();) {
        if (!it.value().hasExpired()) { ++it; continue; }
        ready_sync_retries_.insert(it.key());
        it = retry_deadlines_.erase(it);
    }
    armRetryTimer();
    dispatchSyncJobs();
}

void MainWindow::dispatchSyncJobs() {
    if (closing_ || worker_thread_) return;
    QSet<QString> paths, rescans;
    if (normal_events_due_) {
        paths = pending_paths_;
        rescans = pending_rescan_roots_;
        pending_paths_.clear();
        pending_rescan_roots_.clear();
        normal_events_due_ = false;
    }
    std::vector<SyncFailureScope> retries;
    for (const auto& key : ready_sync_retries_) {
        if (!sync_failure_scopes_.contains(key) || sync_attempts_.value(key) >= 3) continue;
        const auto scope = sync_failure_scopes_.value(key);
        if (!roots_.contains(scope.root)) continue;
        if (dormant_rescan_roots_.contains(scope.root)) {
            if (!scope.rescan) dormant_paths_.insert(scope.path);
            continue;
        }
        retries.push_back(scope);
    }
    ready_sync_retries_.clear();
    if (paths.isEmpty() && rescans.isEmpty() && retries.empty()) return;
    const auto db_path = db_path_;
    const auto roots = roots_;
    startWorker([this, db_path, paths, rescans, roots, retries] {
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
        for (const auto& retry : retries) {
            if (!rescans.contains(retry.root)) process(retry.root, retry.path, retry.rescan);
        }
    }, QStringLiteral("正在同步文件变化…"), false);
}

void MainWindow::reportSyncOutcome(const QString& root, const QString& path, bool rescan, const QString& error) {
    const auto key = failureKey(root, path, rescan);
    if (error.isEmpty()) {
        // A completed ancestor snapshot proves recovery for every failed scope
        // it covers in the same source. Unrelated and other-source failures stay.
        const auto completed_path = rescan ? root : path;
        for (auto it = sync_failure_scopes_.begin(); it != sync_failure_scopes_.end();) {
            const auto& failed = it.value();
            const auto failed_path = failed.rescan ? failed.root : failed.path;
            if (failed.root != root || !pathIsWithin(native(failed_path), native(completed_path))) {
                ++it;
                continue;
            }
            sync_attempts_.remove(it.key());
            sync_errors_.remove(it.key());
            retry_deadlines_.remove(it.key());
            ready_sync_retries_.remove(it.key());
            if (failed.rescan) dormant_rescan_roots_.remove(failed.root);
            else dormant_paths_.remove(failed.path);
            it = sync_failure_scopes_.erase(it);
        }
        sync_attempts_.remove(key);
        sync_errors_.remove(key);
        retry_deadlines_.remove(key);
        ready_sync_retries_.remove(key);
    } else {
        sync_errors_[key] = QStringLiteral("%1：%2").arg(path, error);
        sync_failure_scopes_[key] = {root, path, rescan};
        const auto attempts = ++sync_attempts_[key];
        if (attempts < 3) {
            retry_deadlines_.insert(key, QDeadlineTimer(1500 * attempts, Qt::PreciseTimer));
        } else {
            retry_deadlines_.remove(key);
            ready_sync_retries_.remove(key);
            if (rescan) dormant_rescan_roots_.insert(root);
            else dormant_paths_.insert(path);
        }
    }
    armRetryTimer();
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
