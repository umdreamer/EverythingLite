#include "ui/main_window.h"
#include "core/index_manager.h"
#include "core/path_utils.h"
#include "ui/index_update_journal.h"
#include "ui/open_history_store.h"
#include "ui/app_settings.h"

#include <QDir>
#include <QDateTime>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMetaObject>
#include <QStatusBar>
#include <QTimer>
#include <exception>
#include <algorithm>
#include <limits>
#include <utility>

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

QStringList ownBuildDirectories() {
    QStringList result;
    const auto executable=QDir::cleanPath(QCoreApplication::applicationFilePath());
    const auto marker=executable.indexOf(QStringLiteral(".app/Contents/"));
    if(marker>=0)result<<executable.left(marker+4);
    QDir directory(QCoreApplication::applicationDirPath());
    for(int i=0;i<5;++i) {
        QFile cache(directory.filePath(QStringLiteral("CMakeCache.txt")));
        if(cache.open(QIODevice::ReadOnly) && cache.read(1024*1024).contains("CMAKE_PROJECT_NAME:STATIC=EverythingLite")) {
            result<<QDir::cleanPath(directory.absolutePath());break;
        }
        if(!directory.cdUp())break;
    }
    return result;
}
} // namespace

void MainWindow::scheduleFileEvent(const FileEvent& event) {
    if (closing_) return;
    const auto error = !event.error.empty() ? QString::fromUtf8(event.error.c_str())
        : watcher_ ? QString::fromUtf8(watcher_->lastError().c_str()) : QString{};
    if (!error.isEmpty()) {
        watcher_error_ = error;
        watcher_error_label_->setText(QStringLiteral("目录监听异常"));
        watcher_error_label_->setToolTip(error);
        watcher_error_label_->setStyleSheet(QStringLiteral("color: #b33"));
    }
    if (event.path.empty()) return;
    const auto path = QString::fromUtf8(event.path.c_str());
    const auto normalized_path = QDir::cleanPath(path);
    if (!event.needs_full_rescan && index_journal_ && index_journal_->containsPath(normalized_path)) return;
    const auto own_open_history = QDir::cleanPath(OpenHistoryStore::defaultPath());
    if (!event.needs_full_rescan && (normalized_path == own_open_history || normalized_path.startsWith(own_open_history + QLatin1Char('-')))) return;
    const auto own_database = QDir::cleanPath(db_path_);
    const auto deferred_database=own_database+QStringLiteral(".deferred.sqlite");
    if (!event.needs_full_rescan &&
        (normalized_path == own_database || normalized_path.startsWith(own_database + QLatin1Char('-')) ||
         normalized_path == deferred_database || normalized_path.startsWith(deferred_database + QLatin1Char('-')))) return;
    if(!event.needs_full_rescan) {
        const auto settings_path=QDir::cleanPath(makeAppSettings().fileName());
        if(normalized_path==settings_path || normalized_path==settings_path+QStringLiteral(".lock"))return;
        static const auto own_directories=ownBuildDirectories();
        for(const auto& own:own_directories)
            if(pathIsWithin(native(normalized_path),native(own)))return;
    }
    const auto root = rootForPath(path);
    if (root.isEmpty()) return;
    if(!event.needs_full_rescan && normalized_path!=QDir::cleanPath(root)) {
        try {
            const UpdatePolicy policy(normalizePath(native(QDir::homePath())),update_rules_,{});
            if(policy.classify(normalizePath(native(normalized_path)))==UpdateCadence::Periodic) {
                if(!deferred_updates_)deferred_updates_=std::make_unique<DeferredUpdates>(native(deferred_database));
                const auto parent=QFileInfo(normalized_path).absolutePath();
                const auto scope=pathIsWithin(native(parent),native(root))?parent:normalized_path;
                deferred_updates_->enqueue(normalizePath(native(root)),normalizePath(native(scope)),
                    QDateTime::currentMSecsSinceEpoch()+static_cast<qint64>(periodic_minutes_)*60000);
                armDeferredTimer();return;
            }
        } catch(const std::exception& error) {
            statusBar()->showMessage(QStringLiteral("周期队列不可用，正在直接更新：%1").arg(QString::fromUtf8(error.what())),15000);
        }
    }
    if (event.needs_full_rescan && error.isEmpty()) {
        watcher_error_.clear();
        watcher_error_label_->setText(QStringLiteral("目录监听正常"));
        watcher_error_label_->setToolTip(QStringLiteral("监听 %1 个索引目录的文件变化；后端：%2")
            .arg(roots_.size()).arg(watcher_ ? QString::fromUtf8(watcher_->backendName().c_str()) : QStringLiteral("未知")));
        watcher_error_label_->setStyleSheet(QString{});
    }

    const auto queuePath = [this, &root](const QString& candidate) {
        if (pending_rescan_roots_.contains(root)) return;
        for (auto it = pending_paths_.begin(); it != pending_paths_.end();) {
            // Ancestors cover only jobs belonging to the same nested source.
            if (rootForPath(*it) != root) { ++it; continue; }
            if (pathIsWithin(native(candidate), native(*it))) return;
            if (pathIsWithin(native(*it), native(candidate))) it = pending_paths_.erase(it);
            else ++it;
        }
        pending_paths_.insert(candidate);
    };

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
            dormant_rescan_roots_.remove(scope.root);
            pending_rescan_roots_.insert(scope.root);
        } else {
            dormant_paths_.remove(scope.path);
            // Cancelling a backoff deadline must retain its complete scope,
            // even when the new event covers only one descendant file.
            queuePath(scope.path);
        }
    }
    armRetryTimer();
    // Exhausted full-source work already retains every descendant change.
    // Only an explicit related root/full event above reactivates it; unrelated
    // sibling churn and overflow cannot create a fresh full-source job.
    if (dormant_rescan_roots_.contains(root)) return;
    if (event.needs_full_rescan || root_event) {
        pending_rescan_roots_.insert(root);
    } else if (!pending_rescan_roots_.contains(root)) {
        queuePath(path);
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

void MainWindow::armDeferredTimer() {
    if(!deferred_timer_ || closing_)return;
    deferred_timer_->stop();
    if(!deferred_updates_)return;
    try {
        const auto all=deferred_updates_->due(std::numeric_limits<std::int64_t>::max());
        const auto now=QDateTime::currentMSecsSinceEpoch();
        for(const auto& item:all) {
            const bool configured=std::any_of(roots_.begin(),roots_.end(),[&](const QString& root){return normalizePath(native(root))==item.root;});
            if(!configured)continue;
            const auto delay=std::max<qint64>(0,item.due_ms-now);
            deferred_timer_->start(static_cast<int>(std::min<qint64>(delay,std::numeric_limits<int>::max())));return;
        }
    } catch(const std::exception& error) {
        statusBar()->showMessage(QStringLiteral("无法读取周期队列：%1").arg(QString::fromUtf8(error.what())),15000);
    }
}

QStringList MainWindow::deferredStatusLines() const {
    QStringList lines;
    if(!deferred_updates_)return lines;
    try {
        for(const auto& item:deferred_updates_->due(std::numeric_limits<std::int64_t>::max())) {
            const auto when=QDateTime::fromMSecsSinceEpoch(item.due_ms).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            lines<<QStringLiteral("周期待更新：%1；计划时间：%2").arg(QString::fromUtf8(item.path.c_str()),when);
        }
    } catch(const std::exception& error) {lines<<QStringLiteral("无法读取周期队列：%1").arg(QString::fromUtf8(error.what()));}
    return lines;
}

void MainWindow::processDeferredUpdates() {
    if(closing_ || !deferred_updates_)return;
    if(worker_thread_ || storage_maintenance_) {deferred_timer_->start(1000);return;}
    std::vector<DeferredUpdate> jobs;
    try {
        for(const auto& item:deferred_updates_->due(QDateTime::currentMSecsSinceEpoch())) {
            const bool configured=std::any_of(roots_.begin(),roots_.end(),[&](const QString& root){return normalizePath(native(root))==item.root;});
            if(configured)jobs.push_back(item);
            if(jobs.size()>=64)break;
        }
    } catch(const std::exception& error) {
        statusBar()->showMessage(QStringLiteral("无法读取周期队列：%1").arg(QString::fromUtf8(error.what())),15000);return;
    }
    if(jobs.empty()){armDeferredTimer();return;}
    const auto db_path=db_path_;const auto interval=static_cast<qint64>(periodic_minutes_)*60000;
    startWorker([this,db_path,jobs,interval] {
        DeferredUpdates queue(native(db_path+QStringLiteral(".deferred.sqlite")));
        IndexManager manager(native(db_path));
        for(const auto& job:jobs) {
            QString error;
            try {
                const auto stats=job.root==job.path?manager.rebuildRoots({job.root}):manager.applyPathChange(job.root,job.path);
                if(!stats.completed)error=stats.cancelled?QStringLiteral("同步已取消"):
                    stats.error.empty()?QStringLiteral("扫描未完整完成"):QString::fromUtf8(stats.error.c_str());
                if(error.isEmpty()) {
                    if(!queue.acknowledge(job))queue.postpone(job.root,job.path,QDateTime::currentMSecsSinceEpoch()+interval);
                } else queue.postpone(job.root,job.path,QDateTime::currentMSecsSinceEpoch()+interval);
            } catch(const std::exception& failure) {
                error=QString::fromUtf8(failure.what());
                queue.postpone(job.root,job.path,QDateTime::currentMSecsSinceEpoch()+interval);
            }
            const auto root=QString::fromUtf8(job.root.c_str()),path=QString::fromUtf8(job.path.c_str());
            QMetaObject::invokeMethod(this,[this,root,path,error] {if(!closing_)reportSyncOutcome(root,path,false,error,false);},Qt::QueuedConnection);
        }
    },{},false,[this,interval](const QString& error) {
        if(!error.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("周期更新未完成：%1").arg(error),15000);
            deferred_timer_->start(static_cast<int>(std::min<qint64>(interval,std::numeric_limits<int>::max())));
        } else armDeferredTimer();
    });
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
    if (closing_ || worker_thread_ || storage_maintenance_) return;
    QSet<QString> paths, rescans;
    if (normal_events_due_) {
        paths = pending_paths_;
        rescans = pending_rescan_roots_;
        pending_paths_.clear();
        pending_rescan_roots_.clear();
        normal_events_due_ = false;
    }
    std::vector<SyncFailureScope> jobs;
    const auto queueJob = [&jobs](SyncFailureScope candidate) {
        candidate.rescan = candidate.rescan || (pathIsWithin(native(candidate.path), native(candidate.root))
            && pathIsWithin(native(candidate.root), native(candidate.path)));
        if (candidate.rescan) candidate.path = candidate.root;
        for (auto it = jobs.begin(); it != jobs.end();) {
            if (it->root != candidate.root) { ++it; continue; }
            if (pathIsWithin(native(candidate.path), native(it->path))) return;
            if (pathIsWithin(native(it->path), native(candidate.path))) it = jobs.erase(it);
            else ++it;
        }
        jobs.push_back(std::move(candidate));
    };
    for (const auto& root : rescans) queueJob({root, root, true});
    for (const auto& path : paths) {
        const auto root = rootForPath(path);
        if (!root.isEmpty()) queueJob({root, path, false});
    }
    for (const auto& key : ready_sync_retries_) {
        if (!sync_failure_scopes_.contains(key) || sync_attempts_.value(key) >= 3) continue;
        const auto scope = sync_failure_scopes_.value(key);
        if (!roots_.contains(scope.root)) continue;
        if (dormant_rescan_roots_.contains(scope.root)) {
            if (!scope.rescan) dormant_paths_.insert(scope.path);
            continue;
        }
        // Normal changes and expired retries share one source-scoped ancestor
        // merge, so a batch cannot execute the same covered snapshot twice.
        queueJob(scope);
    }
    ready_sync_retries_.clear();
    if (jobs.empty()) return;
    for (const auto& job : jobs) {
        const auto key = failureKey(job.root, job.path, job.rescan);
        const auto attempt = sync_attempts_.value(key);
        recordIndexUpdate(attempt > 0 ? QStringLiteral("重试") : QStringLiteral("开始"),
                          job.root, job.path, {}, attempt);
    }
    const auto db_path = db_path_;
    startWorker([this, db_path, jobs] {
        auto process = [&](const QString& root, const QString& path, bool rescan) {
            QString error;
            try {
                IndexManager manager(native(db_path));
                const auto stats = rescan ? manager.rebuildRoots({native(root)})
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
        for (const auto& job : jobs) process(job.root, job.path, job.rescan);
    }, {}, false);
}

void MainWindow::reportSyncOutcome(const QString& root, const QString& path, bool requested_rescan, const QString& error, bool retry) {
    // A root-sized applyPathChange also scans the complete source. It shares
    // one canonical scope/budget with explicit full rescans and manual rebuilds.
    const bool rescan = requested_rescan || (pathIsWithin(native(path), native(root))
        && pathIsWithin(native(root), native(path)));
    if (rescan && root == folder_scope_ && folder_scope_index_pending_) {
        folder_scope_index_pending_ = false;
        folder_scope_index_failed_ = !error.isEmpty();
        updateFolderScopeLabel();
        if (error.isEmpty() && !search_ime_composing_) search_timer_->start(kSearchIdleDelayMs);
    }
    const auto key = failureKey(root, rescan ? root : path, rescan);
    const bool previously_failed = sync_failure_scopes_.contains(key);
    bool newly_exhausted = false;
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
        sync_failure_scopes_[key] = {root, rescan ? root : path, rescan};
        const auto attempts = retry ? ++sync_attempts_[key] : (sync_attempts_[key]=3);
        if (retry && attempts < 3) {
            ready_sync_retries_.remove(key);
            retry_deadlines_.insert(key, QDeadlineTimer(sync_retry_base_ms_ * attempts, Qt::PreciseTimer));
        } else {
            newly_exhausted = attempts == 3;
            retry_deadlines_.remove(key);
            ready_sync_retries_.remove(key);
            if (retry) {
                if (rescan) dormant_rescan_roots_.insert(root);
                else dormant_paths_.insert(path);
            }
        }
    }
    armRetryTimer();
    recordIndexUpdate(error.isEmpty()
                          ? (previously_failed ? QStringLiteral("恢复") : QStringLiteral("完成"))
                          : QStringLiteral("待更新"), root, path, error, sync_attempts_.value(key));
    const auto message = sync_errors_.isEmpty() ? QString{}
        : QStringLiteral("待更新范围（保留已有缓存；最多自动重试两次，相关新事件可重试）：%1")
            .arg(sync_errors_.values().join(QStringLiteral("；")));
    const auto label = sync_errors_.isEmpty() ? QStringLiteral("索引同步成功")
        : QStringLiteral("待更新 %1 个范围").arg(sync_errors_.size());
    if (sync_error_label_->text() != label) sync_error_label_->setText(label);
    sync_error_label_->setToolTip(message.isEmpty() ? QStringLiteral("最近一次索引同步已完成；当前无待更新范围") : message);
    sync_error_label_->setStyleSheet({});
    if (newly_exhausted) statusBar()->showMessage(QStringLiteral("有索引范围待更新；详情见“工具 → 索引状态”"), 10000);
    else if (sync_errors_.isEmpty() && statusBar()->currentMessage().startsWith(QStringLiteral("有索引范围待更新"))) statusBar()->clearMessage();
}

QString MainWindow::rootForPath(const QString& path) const {
    QString best;
    for (const auto& root : roots_) {
        if (pathIsWithin(native(path), native(root)) && root.size() > best.size()) best = root;
    }
    return best;
}
} // namespace everything_lite
