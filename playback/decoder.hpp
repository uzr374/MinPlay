#ifndef DECODER_HPP
#define DECODER_HPP

#include <QtGlobal>
#include <list>
#include <vector>

#include "cavpacket.hpp"
#include "cavframe.h"
#include "cavstream.hpp"
#include "cavchannellayout.hpp"

extern "C"{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include "libavfilter/avfilter.h"
}

struct AudioParams final {
    int freq = 0;
    CAVChannelLayout ch_layout;
    AVSampleFormat fmt = AV_SAMPLE_FMT_NONE;

    AudioParams() = default;
    AudioParams(const AVChannelLayout& lout, AVSampleFormat format, int rate):
        ch_layout(lout), fmt(format), freq(rate) {}
};

struct FrameData final {
    int64_t pkt_pos = -1LL;
};

class Decoder
{
    Q_DISABLE_COPY_MOVE(Decoder);
private:
    AVCodecContext* avctx = nullptr;
    bool eof_reached = false;
    CAVStream stream;
    AVRational start_pts_tb{}, next_pts_tb{};
    int64_t start_pts = AV_NOPTS_VALUE, next_pts = 0;

    //AVFilter-related
    std::vector<AVPixelFormat> supported_pix_fmts;
    AudioParams audio_tgt, audio_filter_src;
    int last_w = 0, last_h = 0;
    AVPixelFormat last_pix_fmt = AVPixelFormat(-2);
    AVSampleFormat last_sample_fmt = AV_SAMPLE_FMT_NONE;
    AVFilterContext* in_filter = nullptr, *out_filter = nullptr;
    AVFilterGraph* graph = nullptr;
    bool flush_filters = false, filters_eof = false;

    bool filterVideoFrame(CAVFrame& src, std::list<CAVFrame>& filtered);
    bool filterAudioFrame(CAVFrame& src, std::list<CAVFrame>& filtered);
    void preparePacket(CAVPacket& pkt);
public:
    Decoder() = delete;
    Decoder(const CAVStream& st);
    ~Decoder();

    bool decodeVideoPacket(CAVPacket& pkt, std::list<CAVFrame>& decoded);
    bool decodeAudioPacket(CAVPacket& pkt, std::list<CAVFrame>& decoded);
    //bool decodeSubPacket(const CAVPacket& pkt, QList<CSubtitle> decoded);

    void flush();
    void setSupportedPixFmts(const std::vector<AVPixelFormat>& supported_pix_fmts);
    AudioParams outputAudioFormat() const;
    bool eofReached() const;
};

#endif // DECODER_HPP
