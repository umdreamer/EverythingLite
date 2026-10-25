#include "ui/main_window.h"
#include "ui/index_update_journal.h"

#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace everything_lite {

void MainWindow::recordIndexUpdate(const QString& kind, const QString& root, const QString& path,
                                   const QString& reason, int attempt) {
    if (!index_journal_) return;
    const IndexUpdateEvent event{kind, root, path, reason, attempt, QDateTime::currentDateTime()};
    QString error;
    if (!index_journal_->append(event, &error)) index_journal_error_ = error;
    else index_journal_error_.clear();
    refreshIndexUpdateLogDialog();
}

void MainWindow::refreshIndexUpdateLogDialog() {
    if (!index_update_log_dialog_ || !index_journal_) return;
    auto* dates = index_update_log_dialog_->findChild<QComboBox*>(QStringLiteral("indexUpdateDates"));
    auto* current = index_update_log_dialog_->findChild<QPlainTextEdit*>(QStringLiteral("indexUpdateCurrent"));
    auto* history = index_update_log_dialog_->findChild<QPlainTextEdit*>(QStringLiteral("indexUpdateHistory"));
    if (!dates || !current || !history) return;

    QStringList pending;
    for (const auto& value : sync_errors_) pending << value;
    pending.sort();
    if (pending.isEmpty()) pending << QStringLiteral("当前无待更新范围");
    if (!index_journal_error_.isEmpty())
        pending << QStringLiteral("日志写入未完成：%1").arg(index_journal_error_);
    current->setPlainText(pending.join(QLatin1Char('\n')));

    const auto selected = dates->currentText();
    const auto available = index_journal_->dates();
    dates->blockSignals(true);
    dates->clear();
    dates->addItems(available);
    const auto selected_index = dates->findText(selected);
    if (selected_index >= 0) dates->setCurrentIndex(selected_index);
    dates->blockSignals(false);

    QStringList lines;
    const auto date = QDate::fromString(dates->currentText(), Qt::ISODate);
    for (const auto& event : index_journal_->read(date)) {
        QString line = QStringLiteral("%1  %2  %3").arg(event.at.toString(QStringLiteral("HH:mm:ss")),
                                                        event.kind, event.path);
        if (event.attempt > 0) line += QStringLiteral("  第 %1 次尝试").arg(event.attempt + 1);
        if (!event.reason.isEmpty()) line += QStringLiteral("  原因：%1").arg(event.reason);
        lines << line;
    }
    history->setPlainText(lines.isEmpty() ? QStringLiteral("此日期没有索引更新记录") : lines.join(QLatin1Char('\n')));
}

void MainWindow::showIndexUpdateLog() {
    if (index_update_log_dialog_) {
        refreshIndexUpdateLogDialog();
        index_update_log_dialog_->show();
        index_update_log_dialog_->raise();
        index_update_log_dialog_->activateWindow();
        return;
    }
    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("indexUpdateLogDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("索引更新日志"));
    dialog->resize(760, 540);
    index_update_log_dialog_ = dialog;
    auto* layout = new QVBoxLayout(dialog);
    layout->addWidget(new QLabel(QStringLiteral("当前待更新范围"), dialog));
    auto* current = new QPlainTextEdit(dialog);
    current->setObjectName(QStringLiteral("indexUpdateCurrent"));
    current->setReadOnly(true);
    current->setMaximumHeight(110);
    layout->addWidget(current);
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("历史日期："), dialog));
    auto* dates = new QComboBox(dialog);
    dates->setObjectName(QStringLiteral("indexUpdateDates"));
    row->addWidget(dates, 1);
    auto* refresh = new QPushButton(QStringLiteral("刷新"), dialog);
    row->addWidget(refresh);
    layout->addLayout(row);
    auto* history = new QPlainTextEdit(dialog);
    history->setObjectName(QStringLiteral("indexUpdateHistory"));
    history->setReadOnly(true);
    layout->addWidget(history, 1);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    auto* close = new QPushButton(QStringLiteral("关闭"), dialog);
    buttons->addWidget(close);
    layout->addLayout(buttons);
    connect(dates, &QComboBox::currentIndexChanged, dialog, [this] { refreshIndexUpdateLogDialog(); });
    connect(refresh, &QPushButton::clicked, dialog, [this] { refreshIndexUpdateLogDialog(); });
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    refreshIndexUpdateLogDialog();
    dialog->show();
}

} // namespace everything_lite
