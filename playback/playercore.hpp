#ifndef PLAYERCORE_HPP
#define PLAYERCORE_HPP

#include "../src/GUI/videodisplaywidget.hpp"
#include "../src/GUI/LoggerWidget.h"
#include "PacketQueue.hpp"
#include "clock.hpp"
#include "decoder.hpp"
#include "sdlrenderer.hpp"
#include "formatcontext.hpp"

#include <thread>
#include <condition_variable>
#include <QUrl>

struct VideoState final {
    std::thread demux_thr;
    std::mutex demux_mutex;
    std::condition_variable continue_read_thread;
    bool seek_req = false, pause_req = false, athr_eos = false, vthr_eos = false;
    SeekInfo seek_info;
    int64_t last_seek_pos = 0, last_seek_rel = 0;

    std::thread audio_render_thr;
    bool flush_athr = false, athr_seek_ready = false, athr_quit = false, athr_pause_req = false,
        has_astream = false, athr_params_modified = false;
    PacketQueue audioq;
    int64_t last_audio_pos = -1;
    double last_audio_pts = 0.0;

    std::thread video_render_thr;
    bool flush_vthr = false, vthr_seek_ready = false, vthr_quit = false, vthr_pause_req = false,
        has_vstream = false, has_sub_stream = false, vthr_params_modified = false;
    PacketQueue videoq, subtitleq;
    int64_t last_video_pos = -1;
    double last_video_pts = 0.0;

    Clock audclk;

    std::unique_ptr<Decoder> auddec, viddec, subdec;

    float audio_volume = 1.0f;

    double max_frame_duration = 0.0;// maximum duration of a frame - above this, we consider the jump a timestamp discontinuity

    QString url;
};

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
    std::unique_ptr<VideoState> player_ctx;
    float audio_vol = 1.0f;
    double stream_duration = 0.0, cur_pos = 0.0;

    void toggle_pause();
    bool is_active();

public:
   PlayerCore(QObject* parent, VideoDisplayWidget* video_dw, LoggerWidget* logW);
   static PlayerCore& instance();

   inline SDLRenderer* sdlRenderer(){return video_renderer;}
   void createSDLRenderer();
   void destroySDLRenderer();
   void log(const char* fmt, ...);

   public slots:
        void openURL(QUrl url);
        void stopPlayback();
        void pausePlayback();
        void resumePlayback();
        void togglePause();
        void requestSeekPercent(double percent);
        void requestSeekIncr(double incr);
        void reportStreamDuration(double dur);
        void updateStreamPos(double pos);

    signals:
        void sigReportStreamDuration(double duration);
        void sigUpdateStreamPos(double pos);
        void updatePlaybackPos(double pos, double dur);
        void setControlsActive(bool active);
};

#define playerCore PlayerCore::instance()

#endif // PLAYERCORE_HPP
