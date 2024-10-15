#ifndef PLAYLIST_HPP
#define PLAYLIST_HPP

#include "../../src/utils.hpp"

#include <QListWidget>

class Playlist : public QWidget
{
    Q_OBJECT

    QListWidget* list = nullptr;
    const QString settings_path = Utils::getApplicationDir() + "/settings/playlist.settings";

private:
    void restore();
    void save();

public:
    explicit Playlist(QWidget *parent = nullptr);
    ~Playlist();

    Q_SLOT void appendURLs(const QStringList& urls);
    Q_SLOT void itemOpened(QListWidgetItem* item);
    Q_SIGNAL void openURL(QUrl url);

signals:
};

#endif // PLAYLIST_HPP
