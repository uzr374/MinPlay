#ifndef PLAYBACKENGINE_H
#define PLAYBACKENGINE_H

#include "../src/GUI/videodisplaywidget.hpp"
#include "../src/GUI/LoggerWidget.h"

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
    struct PlayerContext* player_ctx = nullptr;
    float audio_vol = 1.0f;
    double stream_duration = 0.0, cur_pos = 0.0;
    QTimer refresh_timer;

private:
    void handleStreamsUpdate();
    void updateGUI();

signals:
    void sigUpdateStreams(std::vector<class CAVStream> streams);

public:
   PlayerCore(QObject* parent, VideoDisplayWidget* video_dw, LoggerWidget* logW);
   void log(const char* fmt, ...);

   public slots:
        void openURL(QUrl url);
        void stopPlayback();
        void pausePlayback();
        void resumePlayback();
        void togglePause();
        void requestSeekPercent(double percent);
        void requestSeekIncr(double incr);
        void refreshPlayback();

    signals:
        void updatePlaybackPos(double pos, double dur);
        void setControlsActive(bool active);
};

#endif // PLAYBACKENGINE_H
