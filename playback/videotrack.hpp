#ifndef VIDEOTRACK_HPP
#define VIDEOTRACK_HPP

#include "avtrack.hpp"
#include "framequeue.hpp"
#include "cavframe.h"
#include "clock.hpp"

class VideoTrack : public AVTrack
{
private:
    FrameQueue<CAVFrame> frame_pool;
    std::vector<AVPixelFormat> supported_pix_fmts;
    Clock clk;

    AVFilterGraph* vgraph = nullptr;
    AVFilterContext* in_video_filter = nullptr, *out_video_filter = nullptr;

private:
    int get_video_frame(AVFrame *frame);
    int configure_video_filters(AVFilterGraph *graph, const char *vfilters, const AVFrame *frame);
    int queue_picture(AVFrame *src_frame, double pts, double duration, int64_t pos, int serial);

    void run();

public:
    VideoTrack() = delete;
    VideoTrack(const CAVStream& st, std::condition_variable&, const std::vector<AVPixelFormat>&);
    ~VideoTrack();

    int framesAvailable();
    CAVFrame& getLastPicture();
    CAVFrame& peekCurrentPicture();
    CAVFrame& peekNextPicture();
    bool canDisplay();
    void nextFrame();
    bool isAttachedPic();
    int64_t lastPos();

    double getClockVal() const;
    double clockUpdateTime();
    double curPts() const;
    void updateClock(double pts);
    void setPauseStatus(bool p);

    void flush();

};

#endif // VIDEOTRACK_HPP
