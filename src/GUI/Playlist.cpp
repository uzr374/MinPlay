#include "Playlist.hpp"

#include <QUrl>
#include <QFileInfo>
#include <QSettings>

Playlist::Playlist(QWidget *parent)
    : QWidget{parent}, list(new QListWidget(this))
{
    connect(list, &QListWidget::itemDoubleClicked, this, &Playlist::itemOpened);

    restore();
}

Playlist::~Playlist(){
    save();
}

void Playlist::appendURLs(const QStringList& urls){
    for(const auto& urlStr: urls){
        auto title = urlStr;
        QUrl url(urlStr);
        QFileInfo info(url.path());
        if(info.exists()){
            title = info.fileName();
        }
        auto item = new QListWidgetItem(title, list);
        item->setData(Qt::UserRole, url);
    }
}

void Playlist::itemOpened(QListWidgetItem* it){
    const auto url = it->data(Qt::UserRole).toUrl();
    emit openURL(url);
}

void Playlist::save(){
    QSettings sets(settings_path);
    sets.beginGroup("playlistItems");
    const auto count = list->count();
    sets.setValue("count", count);
    for(int i = 0; i < count; ++i){
        const auto item = list->item(i);
        sets.setValue(QString("item_%1_url").arg(i), item->data(Qt::UserRole).toUrl());
        sets.setValue(QString("item_%1_text").arg(i), item->text());
        sets.setValue(QString("item_%1_checked").arg(i), item->checkState());
    }
    sets.endGroup();
}

void Playlist::restore(){
    QSettings sets(settings_path);
    sets.beginGroup("playlistItems");
    const auto count = sets.value("count", 0).toInt();
    for(int i = 0; i < count; ++i){
        const auto url = sets.value(QString("item_%1_url").arg(i), QUrl()).toUrl();
        const auto text = sets.value(QString("item_%1_text").arg(i), QString()).toString();
        const auto checked = static_cast<Qt::CheckState>(sets.value(QString("item_%1_checked").arg(i), Qt::Unchecked).toInt());
        auto item = new QListWidgetItem(text, list);
        item->setData(Qt::UserRole, url);
        item->setCheckState(checked);
    }
    sets.endGroup();
}
