#ifndef MENUBARMENU_H
#define MENUBARMENU_H

#include <QMenu>

#include <vector>

#include "../../playback/cavstream.hpp"

class MenuBarMenu final: public QObject{
    Q_OBJECT

private:
    QMenu* m_fileMenu = nullptr, *m_playbackMenu = nullptr;
    QMenu* sstreams_menu = nullptr, *astreams_menu = nullptr, *vstreams_menu = nullptr;
    std::vector<QAction*> video_streams, audio_streams, sub_streams;

public:
    explicit MenuBarMenu(QWidget* parent);
    ~MenuBarMenu();

    QVector<QMenu*> getTopLevelMenus() const;

    Q_SLOT void getURLs();

signals:
    void stopPlayback();
    void pausePlayback();
    void resumePlayback();
    void submitURLs(const QStringList& urls);
    void streamSwitch(int idx);

public slots:
    void updateStreams(std::vector<CAVStream> streams);
    void streamSwitchRequested(QAction* triggered);
};

#endif // MENUBARMENU_H
