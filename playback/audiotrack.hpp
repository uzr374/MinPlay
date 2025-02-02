#ifndef AUDIOTRACK_HPP
#define AUDIOTRACK_HPP

#include "avtrack.hpp"
#include "framequeue.hpp"
#include "cavframe.h"
#include "clock.hpp"

class AudioTrack final : public AVTrack
{
    struct AudioParams {
        int freq = 0;
        AVChannelLayout ch_layout{};
        AVSampleFormat fmt = AV_SAMPLE_FMT_NONE;
    };
private:
    FrameQueue<CAVFrame> frame_pool;
    Clock clk;

    AudioParams audio_filter_src;
    AVFilterGraph* agraph = nullptr;
    AVFilterContext* in_audio_filter = nullptr, *out_audio_filter = nullptr;

private:
    int configure_audio_filters(const char *afilters);
    void run();

public:
    AudioTrack() = delete;
    AudioTrack(const CAVStream& st, std::condition_variable&);
    ~AudioTrack();

    CAVFrame* getFrame();
    void nextFrame();
    int64_t lastPos();

    double getClockVal() const;
    void updateClock(double pts);
    void setPauseStatus(bool p);

    void flush();
};

#endif // AUDIOTRACK_HPP
