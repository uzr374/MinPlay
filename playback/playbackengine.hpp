#ifndef PLAYBACKENGINE_H
#define PLAYBACKENGINE_H

#include "../src/GUI/videodisplaywidget.hpp"
#include "../src/GUI/LoggerWidget.h"

#include "cavstream.hpp"

#include <QUrl>
#include <QTimer>

class PlayerCore final : public QObject
{
    Q_OBJECT;
    Q_DISABLE_COPY_MOVE(PlayerCore);
private:
    PlayerCore() = delete;
    ~PlayerCore();

    VideoDisplayWidget* video_dw = nullptr;
    LoggerWidget* loggerW = nullptr;
    SDLRenderer* video_renderer = nullptr;
    std::unique_ptr<struct PlayerContext> player_ctx;
    float audio_vol = 1.0f;
    double stream_duration = 0.0, cur_pos = 0.0;
    QTimer refresh_timer;

private:
    void handleStreamsUpdate();
    void updateGUI();

signals:
    void sigUpdateStreams(std::vector<CAVStream> streams);
    void updatePlaybackPos(double pos, double dur);
    void setControlsActive(bool active);
    void resetGUI();
    void setPlayerTitle(QString title);

public:
   PlayerCore(QObject* parent, VideoDisplayWidget* video_dw, LoggerWidget* logW);
   void log(const char* fmt, ...);

   void updateTitle(std::string title);

   public slots:
        void openURL(QUrl url);
        void stopPlayback();
        void togglePause();
        void requestSeekPercent(double percent);
        void requestSeekIncr(double incr);
        void refreshPlayback();
        void streamSwitch(int idx);
};

#endif // PLAYBACKENGINE_H
