#ifndef PLAYLIST_HPP
#define PLAYLIST_HPP

#include <QListWidget>

class Playlist : public QWidget
{
    Q_OBJECT

    QListWidget* list = nullptr;
public:
    explicit Playlist(QWidget *parent = nullptr);
    ~Playlist();

    Q_SLOT void appendURLs(const QStringList& urls);
    Q_SLOT void itemOpened(QListWidgetItem* item);
    Q_SIGNAL void openURL(QUrl url);

signals:
};

#endif // PLAYLIST_HPP
