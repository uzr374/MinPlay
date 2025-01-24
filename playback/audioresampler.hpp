#ifndef AUDIORESAMPLER_HPP
#define AUDIORESAMPLER_HPP

#include <QtGlobal>

#include <vector>

#include "cavchannellayout.hpp"
#include "avframeview.hpp"

extern "C"{
#include <libswresample/swresample.h>
}

class AudioResampler
{
    Q_DISABLE_COPY_MOVE(AudioResampler);
private:
    AVSampleFormat out_fmt = AV_SAMPLE_FMT_NONE, src_fmt = AV_SAMPLE_FMT_NONE;
    int out_rate = 0, src_rate = 0;
    CAVChannelLayout out_ch_layout, src_ch_layout;

    SwrContext* swr_ctx = nullptr;

public:
    AudioResampler();
    ~AudioResampler();

    void setOutputFmt(int out_rate, const CAVChannelLayout& out_lout, AVSampleFormat out_fmt);

    /*Converts aframe and puts the converted data into dst
     @param aframe - frame to convert*/
    bool convert(AVFrameView aframe, std::vector<uint8_t>& dst, int wanted_nb_samples, bool final);
};

#endif // AUDIORESAMPLER_HPP
