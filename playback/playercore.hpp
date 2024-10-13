#ifndef PLAYERCORE_HPP
#define PLAYERCORE_HPP

#include "../src/GUI/videodisplaywidget.hpp"
#include "PacketQueue.hpp"
#include "clock.hpp"
#include "decoder.hpp"
#include "sdlrenderer.hpp"

#include <thread>
#include <condition_variable>

#include <QThread>

struct SeekInfo {
    enum SeekType{
        SEEK_NONE, SEEK_PERCENT, SEEK_INCREMENT, SEEK_CHAPTER
    };

    SeekType type = SEEK_NONE;
    double percent = 0.0, increment = 0.0;
};


struct VideoState final {
    std::thread demux_thr;
    std::mutex demux_mutex;
    std::condition_variable continue_read_thread;
    bool seek_req = false, pause_req = false;
    SeekInfo seek_info;
    int64_t last_seek_pos = 0, last_seek_rel = 0;

    std::thread audio_render_thr;
    bool flush_athr = false, athr_quit = false, athr_pause_req = false;
    int64_t last_audio_pos = -1;
    double last_audio_pts = 0.0;
    bool athr_eof = false;

    std::thread video_render_thr;
    bool flush_vthr = false, vthr_quit = false, vthr_pause_req = false;
    int64_t last_video_pos = -1;
    double last_video_pts = 0.0;
    bool vthr_eof = false;

    Clock audclk, vidclk;

    std::unique_ptr<Decoder> auddec, viddec, subdec;

    int audio_stream = -1;
    AVStream *audio_st = nullptr;
    PacketQueue audioq;

    float audio_volume = 1.0f;

    int subtitle_stream = -1;
    AVStream *subtitle_st = nullptr;
    PacketQueue subtitleq;

    int video_stream = -1;
    AVStream *video_st = nullptr;
    PacketQueue videoq;
    double max_frame_duration = 0.0;      // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity

    std::string url;

    int last_video_stream = -1, last_audio_stream = -1, last_subtitle_stream = -1;
};

class PlayerCore final : public QObject
{
    Q_OBJECT;
    Q_DISABLE_COPY_MOVE(PlayerCore);
private:
    PlayerCore() = delete;
    ~PlayerCore();

    VideoDisplayWidget* video_dw = nullptr;
    SDLRenderer* video_renderer = nullptr;
    std::unique_ptr<VideoState> player_ctx;
    float audio_vol = 1.0f;

    void toggle_pause();
    bool is_active();

public:
   PlayerCore(QObject* parent, VideoDisplayWidget* video_dw);
   static PlayerCore& instance();

   inline SDLRenderer* sdlRenderer(){return video_renderer;}
   void createSDLRenderer();
   void destroySDLRenderer();

   public slots:
        void openURL(QString url);
        void stopPlayback();
        void pausePlayback();
        void resumePlayback();
        void togglePause();
        void requestSeekPercent(double percent);
        void requestSeekIncr(double incr);
};

#define playerCore PlayerCore::instance()

#endif // PLAYERCORE_HPP
