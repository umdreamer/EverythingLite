#include "ui/settings_dialog.h"
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
namespace everything_lite {
SettingsDialog::SettingsDialog(QWidget* parent):QDialog(parent) {
    setObjectName(QStringLiteral("settingsDialog"));setWindowTitle(QStringLiteral("Everything Lite 设置"));
    resize(1000,740);setMinimumSize(760,480);
    auto* layout=new QVBoxLayout(this);auto* row=new QHBoxLayout;
    navigation_=new QTreeWidget(this);navigation_->setObjectName(QStringLiteral("settingsNavigation"));
    navigation_->setHeaderHidden(true);navigation_->setRootIsDecorated(false);navigation_->setMaximumWidth(190);
    stack_=new QStackedWidget(this);stack_->setObjectName(QStringLiteral("settingsPages"));
    row->addWidget(navigation_);row->addWidget(stack_,1);layout->addLayout(row,1);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Close,this);buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(navigation_,&QTreeWidget::currentItemChanged,this,[this](auto* item,auto*){activatePage(item);});
}
void SettingsDialog::addPage(const QString& id,const QString& title,Factory factory) {
    auto* item=new QTreeWidgetItem(navigation_,{title});item->setData(0,Qt::UserRole,id);
    pages_.insert(id,{item,std::move(factory),nullptr});
}
void SettingsDialog::selectPage(const QString& id) {
    const auto found=pages_.find(id);if(found==pages_.end())return;
    if(navigation_->currentItem()==found->item)activatePage(found->item);
    else navigation_->setCurrentItem(found->item);
}
QString SettingsDialog::currentPage() const {
    return navigation_->currentItem()?navigation_->currentItem()->data(0,Qt::UserRole).toString():QString{};
}
void SettingsDialog::activatePage(QTreeWidgetItem* item) {
    if(!item)return;auto& page=pages_[item->data(0,Qt::UserRole).toString()];
    const bool newly_created=!page.widget;
    if(!page.widget) {
        auto* scroll=new QScrollArea(stack_);scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(page.factory(scroll));page.widget=scroll;stack_->addWidget(scroll);
    }
    stack_->setCurrentWidget(page.widget);
    emit pageActivated(item->data(0,Qt::UserRole).toString(),newly_created);
}
void SettingsDialog::embedDialog(QDialog* dialog,QWidget* parent) {
    dialog->setAttribute(Qt::WA_DeleteOnClose,false);dialog->setParent(parent,Qt::Widget);
    dialog->installEventFilter(this);
    for(auto* box:dialog->findChildren<QDialogButtonBox*>()) {
        for(auto standard:{QDialogButtonBox::Close,QDialogButtonBox::Cancel})if(auto* button=box->button(standard))button->hide();
    }
    for(auto* button:dialog->findChildren<QPushButton*>())if(button->text()==QStringLiteral("关闭"))button->hide();
}
bool SettingsDialog::eventFilter(QObject* watched,QEvent* event) {
    if(event->type()==QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key()==Qt::Key_Escape) {reject();return true;}
    return QDialog::eventFilter(watched,event);
}
}
