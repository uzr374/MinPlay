#ifndef DECODER_HPP
#define DECODER_HPP

#include "framequeue.hpp"
#include "cavstream.hpp"

#include <thread>

extern "C"{
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
}

struct FrameData {
    int64_t pkt_pos = -1LL;
};

struct Decoder
{
    Q_DISABLE_COPY_MOVE(Decoder);
    static const std::array<AVColorSpace, 3> sdl_supported_color_spaces;

    enum class DecRes{ERROR = -2, ABORT, TRY_AGAIN, SUCCESS};

    CAVPacket pkt;
    PacketQueue& queue;
    AVCodecContext *avctx = nullptr;
    int pkt_serial = 0, finished_serial = 0;
    bool packet_pending = false;
    std::condition_variable& empty_queue_cond;
    int64_t start_pts = 0;
    AVRational start_pts_tb{};
    int64_t next_pts = 0;
    AVRational next_pts_tb{};
    std::thread decoder_thr;

    Decoder(const CAVStream& st, PacketQueue &queue, std::condition_variable &empty_queue_cond);
    int decode_frame(AVFrame *frame, AVSubtitle *sub);

    void destroy();

    template<class T>
    void abort(FrameQueue<T>& fq)
    {
        queue.abort();
        fq.notify();
        if(decoder_thr.joinable())
            decoder_thr.join();
        queue.flush();
    }

public:
    Decoder();
    ~Decoder();
};

int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                          AVFilterContext *source_ctx, AVFilterContext *sink_ctx);

#endif // DECODER_HPP
