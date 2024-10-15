#include "Playlist.hpp"

#include <QUrl>
#include <QFileInfo>

Playlist::Playlist(QWidget *parent)
    : QWidget{parent}, list(new QListWidget(this))
{
    connect(list, &QListWidget::itemDoubleClicked, this, &Playlist::itemOpened);
}

Playlist::~Playlist(){

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
