#pragma once
#include <QDialog>
#include <QHash>
#include <functional>
class QTreeWidget;
class QTreeWidgetItem;
class QStackedWidget;
namespace everything_lite {
class SettingsDialog final : public QDialog {
    Q_OBJECT
public:
    using Factory=std::function<QWidget*(QWidget*)>;
    explicit SettingsDialog(QWidget* parent=nullptr);
    void addPage(const QString& id,const QString& title,Factory factory);
    void selectPage(const QString& id);
    QString currentPage() const;
    void embedDialog(QDialog* dialog,QWidget* parent);
signals:
    void pageActivated(const QString& id,bool newly_created);
protected:
    bool eventFilter(QObject* watched,QEvent* event) override;
private:
    struct Page {QTreeWidgetItem* item=nullptr;Factory factory;QWidget* widget=nullptr;};
    QHash<QString,Page> pages_;
    QTreeWidget* navigation_;
    QStackedWidget* stack_;
    void activatePage(QTreeWidgetItem* item);
};
}
