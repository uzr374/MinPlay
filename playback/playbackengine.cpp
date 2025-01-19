#include "playbackengine.hpp"
#include "formatcontext.hpp"
#include "avframeview.hpp"
#include "cavchannellayout.hpp"
#include "packetqueue.hpp"

#include <QApplication>
#include <cstdarg>
#include <mutex>
#include <condition_variable>
#include <thread>

extern "C"{

#include <math.h>
#include <stdint.h>

#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/dict.h"
#include "libavutil/fifo.h"
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"
#include "libavutil/bprint.h"
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavutil/display.h"
#include "libswresample/swresample.h"

#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
}

#include <SDL3/SDL.h>

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN  0.900
#define EXTERNAL_CLOCK_SPEED_MAX  1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB   20

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01
#define SDL_AUDIO_BUFLEN 0.15 /*in seconds*/

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

typedef struct AudioParams {
    int freq = 0;
    AVChannelLayout ch_layout{};
    enum AVSampleFormat fmt = AV_SAMPLE_FMT_NONE;
    int frame_size = 0;
    int bytes_per_sec = 0;
} AudioParams;

typedef struct Clock {
    double pts = 0.0;           /* clock base */
    double pts_drift = 0.0;     /* clock base minus time at which we updated the clock */
    double last_updated = 0.0;
    double speed = 1.0;
    int serial = 0;           /* clock is based on a packet with this serial */
    bool paused = false;
    PacketQueue* queue = nullptr;    /* pointer to the corresponding packet queue, used for obsolete clock detection */
} Clock;

typedef struct FrameData {
    int64_t pkt_pos = -1LL;
} FrameData;

/* Common struct for handling all types of decoded data and allocated render buffers. */
typedef struct Frame {
    AVFrame *frame = nullptr;
    AVSubtitle sub{};
    int serial = -1;
    double pts = 0.0;           /* presentation timestamp for the frame */
    double duration = 0.0;      /* estimated duration of the frame */
    int64_t pos = -1LL;          /* byte position of the frame in the input file */
    int width = 0;
    int height = 0;
    int format = -1;
    AVRational sar{};
    bool uploaded = false;
    bool flip_v = false;
} Frame;

typedef struct FrameQueue {
    Frame queue[FRAME_QUEUE_SIZE];
    int rindex = 0;
    int windex = 0;
    int size = 0;
    int max_size = 0;
    int keep_last = 0;
    int rindex_shown = 0;
    std::mutex mutex;
    std::condition_variable cond;
    PacketQueue *pktq = nullptr;
} FrameQueue;

enum SyncType {
    AV_SYNC_AUDIO_MASTER, /* default choice */
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

typedef struct Decoder {
    AVPacket *pkt = nullptr;
    PacketQueue *queue = nullptr;
    AVCodecContext *avctx = nullptr;
    int pkt_serial = 0;
    int finished_serial = 0;
    bool packet_pending = false;
    std::condition_variable *empty_queue_cond = nullptr;
    int64_t start_pts = 0;
    AVRational start_pts_tb{};
    int64_t next_pts = 0;
    AVRational next_pts_tb{};
    std::thread decoder_thr;
} Decoder;

struct VideoState {
    SDLRenderer* sdl_renderer = nullptr;

    std::thread read_thr;
    const AVInputFormat *iformat = nullptr;
    bool abort_request = false;
    bool force_refresh = false;
    bool paused = false;
    bool last_paused = false;
    bool queue_attachments_req = false;
    bool seek_req = false;
    int seek_flags = 0;
    int64_t seek_pos = 0;
    int64_t seek_rel = 0;
    int read_pause_return = 0;
    //AVFormatContext *ic = nullptr;
    bool realtime = false;

    Clock audclk;
    Clock vidclk;
    Clock extclk;

    FrameQueue pictq;
    FrameQueue subpq;
    FrameQueue sampq;

    Decoder auddec;
    Decoder viddec;
    Decoder subdec;

    int audio_stream = -1;

    SyncType av_sync_type = AV_SYNC_AUDIO_MASTER;

    double audio_clock = 0.0;
    int audio_clock_serial = -1;
    double audio_diff_cum = 0.0; /* used for AV difference average computation */
    int audio_diff_avg_count = 0;
    AVStream *audio_st = nullptr;
    PacketQueue audioq;
    std::vector<uint8_t> audio_buf;

    std::mutex ao_mutex;
    int ao_rate = 0, requested_ao_rate = 0;
    int ao_channels = 0, requested_ao_channels = 0;
    SDL_AudioStream* astream = nullptr;

    bool muted = false;
    AudioParams audio_src;
    AudioParams audio_filter_src;
    AudioParams audio_tgt;
    SwrContext *swr_ctx = nullptr;
    int frame_drops_early = 0;
    int frame_drops_late = 0;

    int subtitle_stream = -1;
    AVStream *subtitle_st = nullptr;
    PacketQueue subtitleq;

    double frame_timer = 0.0;
    double frame_last_returned_time = 0.0;
    double frame_last_filter_delay = 0.0;

    int video_stream = -1;
    AVStream *video_st = nullptr;
    PacketQueue videoq;

    double max_frame_duration = 0.0;      // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
    SwsContext *sub_convert_ctx = nullptr;
    bool eof = false;

    char *filename = nullptr;
    bool step = false;

    AVFilterContext *in_video_filter = nullptr;   // the first filter in the video chain
    AVFilterContext *out_video_filter = nullptr;  // the last filter in the video chain
    AVFilterContext *in_audio_filter = nullptr;   // the first filter in the audio chain
    AVFilterContext *out_audio_filter = nullptr;  // the last filter in the audio chain
    AVFilterGraph *agraph = nullptr;              // audio filter graph

    int last_video_stream = -1, last_audio_stream = -1, last_subtitle_stream = -1;

    std::condition_variable continue_read_thread;
    AVFormatContext* fmt_ctx = nullptr;
};

static inline
    int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                   enum AVSampleFormat fmt2, int64_t channel_count2)
{
    /* If channel count == 1, planar and non-planar formats are the same */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}


static int decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, std::condition_variable *empty_queue_cond) {
    d->packet_pending = false;
    d->finished_serial = 0;
    d->next_pts = 0LL;
    d->next_pts_tb = d->start_pts_tb = {};
    d->pkt = av_packet_alloc();
    if (!d->pkt)
        return AVERROR(ENOMEM);
    d->avctx = avctx;
    d->queue = queue;
    d->empty_queue_cond = empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;
    d->pkt_serial = -1;
    return 0;
}

static int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub) {
    int ret = AVERROR(EAGAIN);

    for (;;) {
        if (d->queue->serial() == d->pkt_serial) {
            do {
                if (d->queue->isAborted())
                    return -1;

                switch (d->avctx->codec_type) {
                case AVMEDIA_TYPE_VIDEO:
                    ret = avcodec_receive_frame(d->avctx, frame);
                    if (ret >= 0) {
                        frame->pts = frame->best_effort_timestamp;
                    }
                    break;
                case AVMEDIA_TYPE_AUDIO:
                    ret = avcodec_receive_frame(d->avctx, frame);
                    if (ret >= 0) {
                        const AVRational tb {1, frame->sample_rate};
                        if (frame->pts != AV_NOPTS_VALUE)
                            frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
                        else if (d->next_pts != AV_NOPTS_VALUE)
                            frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                        if (frame->pts != AV_NOPTS_VALUE) {
                            d->next_pts = frame->pts + frame->nb_samples;
                            d->next_pts_tb = tb;
                        }
                    }
                    break;
                }
                if (ret == AVERROR_EOF) {
                    d->finished_serial = d->pkt_serial;
                    avcodec_flush_buffers(d->avctx);
                    return 0;
                }
                if (ret >= 0)
                    return 1;
            } while (ret != AVERROR(EAGAIN));
        }

        do {
            const auto [size, nb_packets, dur] = d->queue->getParams();
            if (nb_packets == 0)
                d->empty_queue_cond->notify_one();
            if (d->packet_pending) {
                d->packet_pending = 0;
            } else {
                const auto old_serial = d->pkt_serial;
                if (d->queue->get(d->pkt, 1, &d->pkt_serial) < 0)
                    return -1;
                if (old_serial != d->pkt_serial) {
                    avcodec_flush_buffers(d->avctx);
                    d->finished_serial = 0;
                    d->next_pts = d->start_pts;
                    d->next_pts_tb = d->start_pts_tb;
                }
            }

            if (d->queue->serial() == d->pkt_serial){
                break;
            }
            av_packet_unref(d->pkt);
        } while (true);

        if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            int got_frame = 0;
            ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, d->pkt);
            if (ret < 0) {
                ret = AVERROR(EAGAIN);
            } else {
                if (got_frame && !d->pkt->data) {
                    d->packet_pending = 1;
                }
                ret = got_frame ? 0 : (d->pkt->data ? AVERROR(EAGAIN) : AVERROR_EOF);
            }
            av_packet_unref(d->pkt);
        } else {
            if (d->pkt->buf && !d->pkt->opaque_ref) {
                FrameData *fd;

                d->pkt->opaque_ref = av_buffer_allocz(sizeof(*fd));
                if (!d->pkt->opaque_ref)
                    return AVERROR(ENOMEM);
                fd = (FrameData*)d->pkt->opaque_ref->data;
                fd->pkt_pos = d->pkt->pos;
            }

            if (avcodec_send_packet(d->avctx, d->pkt) == AVERROR(EAGAIN)) {
                av_log(d->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                d->packet_pending = 1;
            } else {
                av_packet_unref(d->pkt);
            }
        }
    }
}

static void decoder_destroy(Decoder *d) {
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->avctx);
}

static void frame_queue_unref_item(Frame *vp)
{
    av_frame_unref(vp->frame);
    avsubtitle_free(&vp->sub);
}

static int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last)
{
    int i;
    f->pktq = pktq;
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    for (i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    return 0;
}

static void frame_queue_destroy(FrameQueue *f)
{
    int i;
    for (i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
    }
}

static void frame_queue_signal(FrameQueue *f)
{
    std::scoped_lock lck(f->mutex);
    f->cond.notify_one();
}

static Frame *frame_queue_peek(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static Frame *frame_queue_peek_next(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

static Frame *frame_queue_peek_last(FrameQueue *f)
{
    return &f->queue[f->rindex];
}

static Frame *frame_queue_peek_writable(FrameQueue *f)
{
    /* wait until we have space to put a new frame */
    std::unique_lock lck(f->mutex);
    while (f->size >= f->max_size &&
           !f->pktq->isAborted()) {
        f->cond.wait(lck);
    }

    if (f->pktq->isAborted())
        return NULL;

    return &f->queue[f->windex];
}

static Frame *frame_queue_peek_readable(FrameQueue *f)
{
    /* wait until we have a readable a new frame */
    std::unique_lock lck(f->mutex);
    while (f->size - f->rindex_shown <= 0 &&
           !f->pktq->isAborted()) {
        f->cond.wait(lck);
    }

    if (f->pktq->isAborted())
        return NULL;

    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static void frame_queue_push(FrameQueue *f)
{
    if (++f->windex == f->max_size)
        f->windex = 0;
    std::scoped_lock lck(f->mutex);
    f->size++;
    f->cond.notify_one();
}

static void frame_queue_next(FrameQueue *f)
{
    if (f->keep_last && !f->rindex_shown) {
        f->rindex_shown = 1;
        return;
    }
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size)
        f->rindex = 0;
    std::scoped_lock lck(f->mutex);
    f->size--;
    f->cond.notify_one();
}

/* return the number of undisplayed frames in the queue */
static int frame_queue_nb_remaining(FrameQueue *f)
{
    std::scoped_lock lck(f->mutex);
    return f->size - f->rindex_shown;
}

/* return last shown position */
static int64_t frame_queue_last_pos(FrameQueue *f)
{
    std::scoped_lock lck(f->mutex);
    Frame *fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->pktq->serial())
        return fp->pos;
    else
        return -1;
}

static void decoder_abort(Decoder *d, FrameQueue *fq)
{
    d->queue->abort();
    frame_queue_signal(fq);
    if(d->decoder_thr.joinable())
        d->decoder_thr.join();
    d->queue->flush();
}

static enum AVColorSpace sdl_supported_color_spaces[] = {
    AVCOL_SPC_BT709,
    AVCOL_SPC_BT470BG,
    AVCOL_SPC_SMPTE170M,
};

static void video_image_display(VideoState *is)
{
    Frame *vp;
    Frame *sp = NULL;
    SDL_Rect rect;

    vp = frame_queue_peek_last(&is->pictq);

    if (is->subtitle_st) {
        if (frame_queue_nb_remaining(&is->subpq) > 0) {
            sp = frame_queue_peek(&is->subpq);

            if (vp->pts >= sp->pts + ((float) sp->sub.start_display_time / 1000)) {
                if (!sp->uploaded) {
                    uint8_t* pixels[4];
                    int pitch[4];
                    int i;
                    if (!sp->width || !sp->height) {
                        sp->width = vp->width;
                        sp->height = vp->height;
                    }
                    // if (realloc_texture(&is->sub_texture, SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height, SDL_BLENDMODE_BLEND, 1) < 0)
                    //     return;

                    // for (i = 0; i < sp->sub.num_rects; i++) {
                    //     AVSubtitleRect *sub_rect = sp->sub.rects[i];

                    //     sub_rect->x = av_clip(sub_rect->x, 0, sp->width );
                    //     sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
                    //     sub_rect->w = av_clip(sub_rect->w, 0, sp->width  - sub_rect->x);
                    //     sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

                    //     is->sub_convert_ctx = sws_getCachedContext(is->sub_convert_ctx,
                    //                                                sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                    //                                                sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
                    //                                                0, NULL, NULL, NULL);
                    //     if (!is->sub_convert_ctx) {
                    //         av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                    //         return;
                    //     }
                    //     if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)pixels, pitch)) {
                    //         sws_scale(is->sub_convert_ctx, (const uint8_t * const *)sub_rect->data, sub_rect->linesize,
                    //                   0, sub_rect->h, pixels, pitch);
                    //         SDL_UnlockTexture(is->sub_texture);
                    //     }
                    // }
                    sp->uploaded = 1;
                }
            } else
                sp = NULL;
        }
    }

    if (!vp->uploaded) {
        is->sdl_renderer->updateVideoTexture(AVFrameView(*vp->frame));
        is->sdl_renderer->refreshDisplay();
        vp->uploaded = true;
    }
}

static void request_ao_change(VideoState* ctx, int new_freq, int new_chn){
    std::scoped_lock lck(ctx->ao_mutex);
    ctx->requested_ao_rate = new_freq;
    ctx->requested_ao_channels = new_chn;
    qDebug() << "Requesting: " <<new_freq <<new_chn;
}

static void ao_close(VideoState* ctx){
    request_ao_change(ctx, 0, 0);
}

static void stream_component_close(VideoState *is, int stream_index, AVFormatContext* ic)
{
    AVCodecParameters *codecpar;

    if (!ic || stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        decoder_abort(&is->auddec, &is->sampq);
        ao_close(is);
        decoder_destroy(&is->auddec);
        swr_free(&is->swr_ctx);
        break;
    case AVMEDIA_TYPE_VIDEO:
        decoder_abort(&is->viddec, &is->pictq);
        decoder_destroy(&is->viddec);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        decoder_abort(&is->subdec, &is->subpq);
        decoder_destroy(&is->subdec);
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->audio_st = NULL;
        is->audio_stream = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_st = NULL;
        is->video_stream = -1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_st = NULL;
        is->subtitle_stream = -1;
        break;
    default:
        break;
    }
}

static void stream_close(VideoState *is)
{
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    if(is->read_thr.joinable())
        is->read_thr.join();

    if(is->sdl_renderer){
        is->sdl_renderer->clearDisplay();
    }

    /* free all pictures */
    frame_queue_destroy(&is->pictq);
    frame_queue_destroy(&is->sampq);
    frame_queue_destroy(&is->subpq);

    sws_freeContext(is->sub_convert_ctx);
    av_free(is->filename);

    if(is->astream){
        SDL_DestroyAudioStream(is->astream);
    }

    delete is;
}

static double get_clock(Clock *c)
{
    if (c->queue && c->queue->serial() != c->serial)
        return NAN;
    if (c->paused) {
        return c->pts;
    } else {
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

static void set_clock_at(Clock *c, double pts, int serial, double time)
{
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

static void set_clock(Clock *c, double pts, int serial)
{
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

static void set_clock_speed(Clock *c, double speed)
{
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
}

static void init_clock(Clock *c, PacketQueue *queue)
{
    c->speed = 1.0;
    c->paused = 0;
    c->queue = queue;
    set_clock(c, NAN, -1);
}

static void sync_clock_to_slave(Clock *c, Clock *slave)
{
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        set_clock(c, slave_clock, slave->serial);
}

static int get_master_sync_type(VideoState *is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    } else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

/* get the current master clock value */
static double get_master_clock(VideoState *is)
{
    double val;

    switch (get_master_sync_type(is)) {
    case AV_SYNC_VIDEO_MASTER:
        val = get_clock(&is->vidclk);
        break;
    case AV_SYNC_AUDIO_MASTER:
        val = get_clock(&is->audclk);
        break;
    default:
        val = get_clock(&is->extclk);
        break;
    }
    return val;
}

static void check_external_clock_speed(VideoState *is) {
    const auto [videoq_size, videoq_nb_pkts, videoq_dur] = is->videoq.getParams();
    const auto [audioq_size, audioq_nb_pkts, audioq_dur] = is->audioq.getParams();
    if (is->video_stream >= 0 && videoq_nb_pkts <= EXTERNAL_CLOCK_MIN_FRAMES ||
        is->audio_stream >= 0 && audioq_nb_pkts <= EXTERNAL_CLOCK_MIN_FRAMES) {
        set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
    } else if ((is->video_stream < 0 || videoq_nb_pkts > EXTERNAL_CLOCK_MAX_FRAMES) &&
               (is->audio_stream < 0 || audioq_nb_pkts > EXTERNAL_CLOCK_MAX_FRAMES)) {
        set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
    } else {
        double speed = is->extclk.speed;
        if (speed != 1.0)
            set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
    }
}

/* seek in the stream */
static void stream_seek(VideoState *is, int64_t pos, int64_t rel, int by_bytes)
{
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        is->seek_req = 1;
        is->continue_read_thread.notify_one();
    }
}

/* pause or resume the video */
static void stream_toggle_pause(VideoState *is)
{
    if (is->paused) {
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
        if (is->read_pause_return != AVERROR(ENOSYS)) {
            is->vidclk.paused = 0;
        }
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
    }
    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
}

static void toggle_pause(VideoState *is)
{
    stream_toggle_pause(is);
    is->step = 0;
}

static void toggle_mute(VideoState *is)
{
    is->muted = !is->muted;
}

static void step_to_next_frame(VideoState *is)
{
    /* if the stream is paused unpause it, then step */
    if (is->paused)
        stream_toggle_pause(is);
    is->step = 1;
}

static double compute_target_delay(double delay, VideoState *is)
{
    double sync_threshold, diff = 0;

    /* update delay to follow master synchronisation source */
    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
        /* if video is slave, we try to correct big delays by
           duplicating or deleting a frame */
        diff = get_clock(&is->vidclk) - get_master_clock(is);

        /* skip or repeat frame. We take into account the
           delay to compute the threshold. I still don't know
           if it is the best guess */
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration) {
            if (diff <= -sync_threshold)
                delay = FFMAX(0, delay + diff);
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay = delay + diff;
            else if (diff >= sync_threshold)
                delay = 2 * delay;
        }
    }

    av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n",
           delay, -diff);

    return delay;
}

static double vp_duration(VideoState *is, Frame *vp, Frame *nextvp) {
    if (vp->serial == nextvp->serial) {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
            return vp->duration;
        else
            return duration;
    } else {
        return 0.0;
    }
}

static void update_video_pts(VideoState *is, double pts, int serial)
{
    /* update current video pts */
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);
}

/* called to display each frame */
static void video_refresh(VideoState *is, double *remaining_time)
{
    double time;

    Frame *sp, *sp2;

    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
        check_external_clock_speed(is);

    if (is->video_st) {
    retry:
        if (frame_queue_nb_remaining(&is->pictq) == 0) {
            // nothing to do, no picture to display in the queue
        } else {
            double last_duration, duration, delay;
            Frame *vp, *lastvp;

            /* dequeue the picture */
            lastvp = frame_queue_peek_last(&is->pictq);
            vp = frame_queue_peek(&is->pictq);

            if (vp->serial != is->videoq.serial()) {
                frame_queue_next(&is->pictq);
                goto retry;
            }

            if (lastvp->serial != vp->serial)
                is->frame_timer = av_gettime_relative() / 1000000.0;

            if (is->paused)
                goto display;

            /* compute nominal last_duration */
            last_duration = vp_duration(is, lastvp, vp);
            delay = compute_target_delay(last_duration, is);

            time= av_gettime_relative()/1000000.0;
            if (time < is->frame_timer + delay) {
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
                goto display;
            }

            is->frame_timer += delay;
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
                is->frame_timer = time;

            if (!isnan(vp->pts))
                update_video_pts(is, vp->pts, vp->serial);

            if (frame_queue_nb_remaining(&is->pictq) > 1) {
                Frame *nextvp = frame_queue_peek_next(&is->pictq);
                duration = vp_duration(is, vp, nextvp);
                if(!is->step && ((get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) && time > is->frame_timer + duration){
                    is->frame_drops_late++;
                    frame_queue_next(&is->pictq);
                    goto retry;
                }
            }

            if (is->subtitle_st) {
                while (frame_queue_nb_remaining(&is->subpq) > 0) {
                    sp = frame_queue_peek(&is->subpq);

                    if (frame_queue_nb_remaining(&is->subpq) > 1)
                        sp2 = frame_queue_peek_next(&is->subpq);
                    else
                        sp2 = NULL;

                    if (sp->serial != is->subtitleq.serial()
                        || (is->vidclk.pts > (sp->pts + ((float) sp->sub.end_display_time / 1000)))
                        || (sp2 && is->vidclk.pts > (sp2->pts + ((float) sp2->sub.start_display_time / 1000))))
                    {
                        if (sp->uploaded) {
                            // int i;
                            // for (i = 0; i < sp->sub.num_rects; i++) {
                            //     AVSubtitleRect *sub_rect = sp->sub.rects[i];
                            //     uint8_t *pixels;
                            //     int pitch, j;

                            //     if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)&pixels, &pitch)) {
                            //         for (j = 0; j < sub_rect->h; j++, pixels += pitch)
                            //             memset(pixels, 0, sub_rect->w << 2);
                            //         SDL_UnlockTexture(is->sub_texture);
                            //     }
                            // }
                        }
                        frame_queue_next(&is->subpq);
                    } else {
                        break;
                    }
                }
            }

            frame_queue_next(&is->pictq);
            is->force_refresh = 1;

            if (is->step && !is->paused)
                stream_toggle_pause(is);
        }
    display:
        /* display picture */
        if (is->force_refresh && is->pictq.rindex_shown)
            video_image_display(is);
    }
    is->force_refresh = 0;
}

static int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    Frame *vp;

    if (!(vp = frame_queue_peek_writable(&is->pictq)))
        return -1;

    vp->sar = src_frame->sample_aspect_ratio;
    vp->uploaded = 0;

    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

    av_frame_move_ref(vp->frame, src_frame);
    frame_queue_push(&is->pictq);
    return 0;
}

static int get_video_frame(VideoState *is, AVFrame *frame)
{
    int got_picture;

    if ((got_picture = decoder_decode_frame(&is->viddec, frame, NULL)) < 0)
        return -1;

    if (got_picture) {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->fmt_ctx, is->video_st, frame);

        if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
            if (frame->pts != AV_NOPTS_VALUE) {
                const double diff = dpts - get_master_clock(is);
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - is->frame_last_filter_delay < 0 &&
                    is->viddec.pkt_serial == is->vidclk.serial &&
                    !is->videoq.isEmpty()) {
                    is->frame_drops_early++;
                    av_frame_unref(frame);
                    got_picture = 0;
                }
            }
        }
    }

    return got_picture;
}

static int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
    int ret, i;
    int nb_filters = graph->nb_filters;
    AVFilterInOut *outputs = NULL, *inputs = NULL;

    if (filtergraph) {
        outputs = avfilter_inout_alloc();
        inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        outputs->name       = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx    = 0;
        outputs->next       = NULL;

        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = sink_ctx;
        inputs->pad_idx     = 0;
        inputs->next        = NULL;

        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL)) < 0)
            goto fail;
    } else {
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
            goto fail;
    }

    /* Reorder the filters to ensure that inputs of the custom filters are merged first */
    for (i = 0; i < graph->nb_filters - nb_filters; i++)
        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

    ret = avfilter_graph_config(graph, NULL);
fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

static double get_rotation(const int32_t *displaymatrix)
{
    double theta = 0;
    if (displaymatrix)
        theta = -round(av_display_rotation_get(displaymatrix));

    theta -= 360*floor(theta/360 + 0.9/360);

    if (fabs(theta - 90*round(theta/90)) > 2)
        av_log(NULL, AV_LOG_WARNING, "Odd rotation angle.\n"
                                     "If you want to help, upload a sample "
                                     "of this file to https://streams.videolan.org/upload/ "
                                     "and contact the ffmpeg-devel mailing list. (ffmpeg-devel@ffmpeg.org)");

    return theta;
}

static int configure_video_filters(AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame)
{
    const auto pix_fmts = is->sdl_renderer->supportedFormats();
    char sws_flags_str[512]{};
    int ret;
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    AVCodecParameters *codecpar = is->video_st->codecpar;
    AVRational fr = av_guess_frame_rate(is->fmt_ctx, is->video_st, NULL);
    const AVDictionaryEntry *e = NULL;
    AVDictionary* sws_dict = nullptr;

    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();

    if (!par)
        return AVERROR(ENOMEM);

    while ((e = av_dict_iterate(sws_dict, e))) {
        if (!strcmp(e->key, "sws_flags")) {
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
        } else
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
    }
    if (strlen(sws_flags_str))
        sws_flags_str[strlen(sws_flags_str)-1] = '\0';

    graph->scale_sws_opts = av_strdup(sws_flags_str);


    filt_src = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffer"),
                                           "ffplay_buffer");
    if (!filt_src) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    par->format              = frame->format;
    par->time_base           = is->video_st->time_base;
    par->width               = frame->width;
    par->height              = frame->height;
    par->sample_aspect_ratio = codecpar->sample_aspect_ratio;
    par->color_space         = frame->colorspace;
    par->color_range         = frame->color_range;
    par->frame_rate          = fr;
    par->hw_frames_ctx = frame->hw_frames_ctx;
    ret = av_buffersrc_parameters_set(filt_src, par);
    if (ret < 0)
        goto fail;

    ret = avfilter_init_dict(filt_src, NULL);
    if (ret < 0)
        goto fail;

    filt_out = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffersink"),
                                           "ffplay_buffersink");
    if (!filt_out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if ((ret = av_opt_set_array(filt_out, "pixel_formats", AV_OPT_SEARCH_CHILDREN,
                                0, pix_fmts.size(), AV_OPT_TYPE_PIXEL_FMT, pix_fmts.data())) < 0)
        goto fail;
    if ((ret = av_opt_set_array(filt_out, "colorspaces", AV_OPT_SEARCH_CHILDREN,
                                0, FF_ARRAY_ELEMS(sdl_supported_color_spaces),
                                AV_OPT_TYPE_INT, sdl_supported_color_spaces)) < 0)
        goto fail;

    ret = avfilter_init_dict(filt_out, NULL);
    if (ret < 0)
        goto fail;

    last_filter = filt_out;

/* Note: this macro adds a filter before the lastly added filter, so the
 * processing order of the filters is in reverse */
#define INSERT_FILT(name, arg) do {                                          \
    AVFilterContext *filt_ctx;                                               \
                                                                             \
        ret = avfilter_graph_create_filter(&filt_ctx,                            \
                                       avfilter_get_by_name(name),           \
                                       "ffplay_" name, arg, NULL, graph);    \
        if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
        ret = avfilter_link(filt_ctx, 0, last_filter, 0);                        \
        if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
        last_filter = filt_ctx;                                                  \
} while (0)

    if (true) {
        double theta = 0.0;
        int32_t *displaymatrix = NULL;
        AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX);
        if (sd)
            displaymatrix = (int32_t *)sd->data;
        if (!displaymatrix) {
            const AVPacketSideData *psd = av_packet_side_data_get(is->video_st->codecpar->coded_side_data,
                                                                  is->video_st->codecpar->nb_coded_side_data,
                                                                  AV_PKT_DATA_DISPLAYMATRIX);
            if (psd)
                displaymatrix = (int32_t *)psd->data;
        }
        theta = get_rotation(displaymatrix);

        if (fabs(theta - 90) < 1.0) {
            INSERT_FILT("transpose", displaymatrix[3] > 0 ? "cclock_flip" : "clock");
        } else if (fabs(theta - 180) < 1.0) {
            if (displaymatrix[0] < 0)
                INSERT_FILT("hflip", NULL);
            if (displaymatrix[4] < 0)
                INSERT_FILT("vflip", NULL);
        } else if (fabs(theta - 270) < 1.0) {
            INSERT_FILT("transpose", displaymatrix[3] < 0 ? "clock_flip" : "cclock");
        } else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            INSERT_FILT("rotate", rotate_buf);
        } else {
            if (displaymatrix && displaymatrix[4] < 0)
                INSERT_FILT("vflip", NULL);
        }
    }

    if(frame->flags & AV_FRAME_FLAG_INTERLACED){
        INSERT_FILT("yadif", nullptr);
    }

if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
    goto fail;

is->in_video_filter  = filt_src;
is->out_video_filter = filt_out;

fail:
       av_freep(&par);
return ret;
}

static int configure_audio_filters(VideoState *is, const char *afilters)
{
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
    char aresample_swr_opts[512]{};
    const AVDictionaryEntry *e = NULL;
    AVDictionary* swr_opts = nullptr;
    AVBPrint bp{};
    char asrc_args[256]{};
    int ret;

    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);
    is->agraph->nb_threads = 1;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);

    while ((e = av_dict_iterate(swr_opts, e)))
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
    if (strlen(aresample_swr_opts))
        aresample_swr_opts[strlen(aresample_swr_opts)-1] = '\0';
    av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

    av_channel_layout_describe_bprint(&is->audio_filter_src.ch_layout, &bp);

    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:time_base=%d/%d:channel_layout=%s",
                   is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
                   1, is->audio_filter_src.freq, bp.str);

    ret = avfilter_graph_create_filter(&filt_asrc,
                                       avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                       asrc_args, NULL, is->agraph);
    if (ret < 0)
        goto end;

    filt_asink = avfilter_graph_alloc_filter(is->agraph, avfilter_get_by_name("abuffersink"),
                                             "ffplay_abuffersink");
    if (!filt_asink) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if ((ret = av_opt_set(filt_asink, "sample_formats", "fltp", AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;

    if (1) {
        if ((ret = av_opt_set_array(filt_asink, "channel_layouts", AV_OPT_SEARCH_CHILDREN,
                                    0, 1, AV_OPT_TYPE_CHLAYOUT, &is->auddec.avctx->ch_layout)) < 0)
            goto end;
        if ((ret = av_opt_set_array(filt_asink, "samplerates", AV_OPT_SEARCH_CHILDREN,
                                    0, 1, AV_OPT_TYPE_INT, &is->auddec.avctx->sample_rate)) < 0)
            goto end;
    }

    ret = avfilter_init_dict(filt_asink, NULL);
    if (ret < 0)
        goto end;

    if ((ret = configure_filtergraph(is->agraph, afilters, filt_asrc, filt_asink)) < 0)
        goto end;

    is->in_audio_filter  = filt_asrc;
    is->out_audio_filter = filt_asink;

end:
    if (ret < 0)
        avfilter_graph_free(&is->agraph);
    av_bprint_finalize(&bp, NULL);

    return ret;
}

static int audio_thread(VideoState *is)
{
    AVFrame *frame = av_frame_alloc();
    Frame *af;
    int last_serial = -1;
    int got_frame = 0;
    int ret = 0;

    if (!frame)
        return AVERROR(ENOMEM);

    do {
        if ((got_frame = decoder_decode_frame(&is->auddec, frame, NULL)) < 0)
            goto the_end;

        if (got_frame) {
            const bool reconfigure =
                cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.ch_layout.nb_channels,
                               AVSampleFormat(frame->format), frame->ch_layout.nb_channels)    ||
                av_channel_layout_compare(&is->audio_filter_src.ch_layout, &frame->ch_layout) ||
                is->audio_filter_src.freq           != frame->sample_rate ||
                is->auddec.pkt_serial               != last_serial;

            if (reconfigure) {
                char buf1[1024], buf2[1024];
                av_channel_layout_describe(&is->audio_filter_src.ch_layout, buf1, sizeof(buf1));
                av_channel_layout_describe(&frame->ch_layout, buf2, sizeof(buf2));
                av_log(NULL, AV_LOG_DEBUG,
                       "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                       is->audio_filter_src.freq, is->audio_filter_src.ch_layout.nb_channels, av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
                       frame->sample_rate, frame->ch_layout.nb_channels, av_get_sample_fmt_name(AVSampleFormat(frame->format)), buf2, is->auddec.pkt_serial);

                is->audio_filter_src.fmt            = AVSampleFormat(frame->format);
                ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &frame->ch_layout);
                if (ret < 0)
                    goto the_end;
                is->audio_filter_src.freq           = frame->sample_rate;
                last_serial                         = is->auddec.pkt_serial;

                if ((ret = configure_audio_filters(is, nullptr)) < 0)
                    goto the_end;
            }

            if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
                goto the_end;

            while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
                FrameData *fd = frame->opaque_ref ? (FrameData*)frame->opaque_ref->data : NULL;
                const auto tb = av_buffersink_get_time_base(is->out_audio_filter);
                if (!(af = frame_queue_peek_writable(&is->sampq)))
                    goto the_end;

                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                af->pos = fd ? fd->pkt_pos : -1;
                af->serial = is->auddec.pkt_serial;
                af->duration = av_q2d({frame->nb_samples, frame->sample_rate});

                av_frame_move_ref(af->frame, frame);
                frame_queue_push(&is->sampq);

                if (is->audioq.serial() != is->auddec.pkt_serial)
                    break;
            }
            if (ret == AVERROR_EOF)
                is->auddec.finished_serial = is->auddec.pkt_serial;
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);

the_end:
    avfilter_graph_free(&is->agraph);
    av_frame_free(&frame);

    return ret;
}

static bool decoder_start(Decoder *d, int (*fn)(VideoState*), VideoState* arg)
{
    d->queue->start();
    try{
        d->decoder_thr = std::thread(fn, arg);
    } catch(...){
        d->queue->abort();
        return false;
    }

    return true;
}

static int video_thread(VideoState *is)
{
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;

    AVFilterGraph *graph = NULL;
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = AV_PIX_FMT_NONE;
    int last_serial = -1;

    if (!frame)
        return AVERROR(ENOMEM);

    for (;;) {
        ret = get_video_frame(is, frame);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;

        if (   last_w != frame->width
            || last_h != frame->height
            || last_format != frame->format
            || last_serial != is->viddec.pkt_serial) {
            av_log(NULL, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
                   frame->width, frame->height,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(AVPixelFormat(frame->format)), "none"), is->viddec.pkt_serial);
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if (!graph) {
                goto the_end;
            }
            graph->nb_threads = 0;
            if (configure_video_filters(graph, is, nullptr, frame) < 0) {
                goto the_end;
            }
            filt_in  = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = AVPixelFormat(frame->format);
            last_serial = is->viddec.pkt_serial;
        }

        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;

        while (ret >= 0) {
            FrameData *fd;

            is->frame_last_returned_time = av_gettime_relative() / 1000000.0;

            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF)
                    is->viddec.finished_serial = is->viddec.pkt_serial;
                ret = 0;
                break;
            }

            fd = frame->opaque_ref ? (FrameData*)frame->opaque_ref->data : NULL;

            is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
            if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                is->frame_last_filter_delay = 0;
            const auto tb = av_buffersink_get_time_base(filt_out);
            const auto frame_rate = av_buffersink_get_frame_rate(filt_out);
            duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            ret = queue_picture(is, frame, pts, duration, fd ? fd->pkt_pos : -1, is->viddec.pkt_serial);
            av_frame_unref(frame);
            if (is->videoq.serial() != is->viddec.pkt_serial)
                break;
        }

        if (ret < 0)
            goto the_end;
    }
the_end:
    avfilter_graph_free(&graph);
    av_frame_free(&frame);
    return 0;
}

static int subtitle_thread(VideoState *is)
{
    Frame *sp;
    int got_subtitle;
    double pts;

    for (;;) {
        if (!(sp = frame_queue_peek_writable(&is->subpq)))
            return 0;

        if ((got_subtitle = decoder_decode_frame(&is->subdec, NULL, &sp->sub)) < 0)
            break;

        pts = 0;

        if (got_subtitle && sp->sub.format == 0) {
            if (sp->sub.pts != AV_NOPTS_VALUE)
                pts = sp->sub.pts / (double)AV_TIME_BASE;
            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial;
            sp->width = is->subdec.avctx->width;
            sp->height = is->subdec.avctx->height;
            sp->uploaded = 0;

            /* now we can update the picture count */
            frame_queue_push(&is->subpq);
        } else if (got_subtitle) {
            avsubtitle_free(&sp->sub);
        }
    }
    return 0;
}

static int synchronize_audio(VideoState *is, int nb_samples)
{
    int wanted_nb_samples = nb_samples;

    /* init averaging filter */
    const auto audio_diff_avg_coef  = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
    /* since we do not have a precise anough audio FIFO fullness,
           we correct audio sync only if larger than this threshold */
    const auto audio_diff_threshold = SDL_AUDIO_BUFLEN;

    /* if not master, then we try to remove or add samples to correct the clock */
    if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = get_clock(&is->audclk) - get_master_clock(is);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            is->audio_diff_cum = diff + audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                /* not enough measures to have a correct estimate */
                is->audio_diff_avg_count++;
            } else {
                /* estimate the A-V difference */
                avg_diff = is->audio_diff_cum * (1.0 - audio_diff_avg_coef);

                if (fabs(avg_diff) >= audio_diff_threshold) {
                    wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }
                av_log(NULL, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                       diff, avg_diff, wanted_nb_samples - nb_samples,
                       is->audio_clock, audio_diff_threshold);
            }
        } else {
            /* too big difference : may be initial PTS errors, so
               reset A-V filter */
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum       = 0;
        }
    }

    return wanted_nb_samples;
}

static int audio_decode_frame(VideoState *is)
{
    Frame *af;
    int resampled_data_size = 0;
    auto& dst = is->audio_buf;

    do {
        if(frame_queue_nb_remaining(&is->sampq) == 0){
            //qDebug() << "sampq empty";
            return -1;
        }
        if (!(af = frame_queue_peek_readable(&is->sampq))){
            //qDebug() << "sampq aborted";
            return -1;
        }
        frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial());

    const auto data_size = av_samples_get_buffer_size(NULL, af->frame->ch_layout.nb_channels,
                                           af->frame->nb_samples,
                                           AVSampleFormat(af->frame->format), 1);

    const auto wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

    if (af->frame->format        != is->audio_src.fmt            ||
        av_channel_layout_compare(&af->frame->ch_layout, &is->audio_src.ch_layout) ||
        af->frame->sample_rate   != is->audio_src.freq           ||
        (wanted_nb_samples       != af->frame->nb_samples && !is->swr_ctx)) {
        int ret;
        swr_free(&is->swr_ctx);
        ret = swr_alloc_set_opts2(&is->swr_ctx,
                                  &is->audio_tgt.ch_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
                                  &af->frame->ch_layout, AVSampleFormat(af->frame->format), af->frame->sample_rate,
                                  0, NULL);
        if (ret < 0 || swr_init(is->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                   af->frame->sample_rate, av_get_sample_fmt_name(AVSampleFormat(af->frame->format)), af->frame->ch_layout.nb_channels,
                   is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.ch_layout.nb_channels);
            swr_free(&is->swr_ctx);
            //qDebug() << "swr create failed";
            return -1;
        }
        if (av_channel_layout_copy(&is->audio_src.ch_layout, &af->frame->ch_layout) < 0)
            return -1;
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = AVSampleFormat(af->frame->format);
    }

    if (is->swr_ctx) {
        const uint8_t **in = (const uint8_t **)af->frame->extended_data;
        const int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
        const int out_size  = av_samples_get_buffer_size(NULL, is->audio_tgt.ch_layout.nb_channels, out_count, is->audio_tgt.fmt, 0);
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        if (wanted_nb_samples != af->frame->nb_samples) {
            if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
                                     wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }
        dst.resize(out_size);
        uint8_t* out[] = {dst.data()};
        const auto len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(is->swr_ctx) < 0)
                swr_free(&is->swr_ctx);
        }

        resampled_data_size = len2 * is->audio_tgt.ch_layout.nb_channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
    } else {
        dst.resize(data_size);
        std::copy(af->frame->data[0], af->frame->data[0] + data_size, dst.begin());;
        resampled_data_size = data_size;
    }

    /* update the audio clock with the pts */
    if (!isnan(af->pts))
        is->audio_clock = af->pts + af->duration;
    else
        is->audio_clock = NAN;
    is->audio_clock_serial = af->serial;

    return resampled_data_size;
}

static SDL_AudioStream* audio_open(VideoState* ctx, int wanted_nb_channels, int wanted_sample_rate, AudioParams& audio_hw_params)
{
    if (wanted_sample_rate <= 0 || wanted_nb_channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return nullptr;
    }

    const SDL_AudioSpec spec{.format = SDL_AUDIO_F32, .channels = wanted_nb_channels, .freq = wanted_sample_rate};
    auto astream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);

    if (astream) {
        SDL_ResumeAudioStreamDevice(astream);
        av_channel_layout_default(&audio_hw_params.ch_layout, wanted_nb_channels);
        audio_hw_params.fmt = AV_SAMPLE_FMT_FLT;
        audio_hw_params.freq = spec.freq;
        audio_hw_params.frame_size = av_samples_get_buffer_size(NULL, audio_hw_params.ch_layout.nb_channels, 1, audio_hw_params.fmt, 1);
        audio_hw_params.bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params.ch_layout.nb_channels, audio_hw_params.freq, audio_hw_params.fmt, 1);
    }

    return astream;
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(VideoState *is, int stream_index, AVFormatContext* ic)
{
    AVCodecContext* avctx = nullptr;
    int ret = 0;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;

    const auto st = ic->streams[stream_index];
    const auto codecpar = st->codecpar;
    const auto codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        av_log(NULL, AV_LOG_WARNING,
               "No decoder could be found for codec %s\n", avcodec_get_name(codecpar->codec_id));
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx = avcodec_alloc_context3(codec);
    if (!avctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(avctx, codecpar);
    if (ret < 0)
        goto fail;

    switch(codecpar->codec_type){
    case AVMEDIA_TYPE_AUDIO   : is->last_audio_stream    = stream_index; break;
    case AVMEDIA_TYPE_SUBTITLE: is->last_subtitle_stream = stream_index; break;
    case AVMEDIA_TYPE_VIDEO   : is->last_video_stream    = stream_index; break;
    }

    avctx->pkt_timebase = st->time_base;
    avctx->codec_id = codec->id;
    avctx->lowres = 0;
    avctx->thread_count = (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) ? 0 : 1;
    avctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;

    if (false)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;

    if ((ret = avcodec_open2(avctx, codec, nullptr)) < 0) {
        goto fail;
    }

    is->eof = 0;
    st->discard = AVDISCARD_DEFAULT;
    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        /* prepare audio output */
        request_ao_change(is, codecpar->sample_rate, codecpar->ch_layout.nb_channels);

        is->audio_stream = stream_index;
        is->audio_st = st;

        if ((ret = decoder_init(&is->auddec, avctx, &is->audioq, &is->continue_read_thread)) < 0)
            goto fail;
        if (ic->iformat->flags & AVFMT_NOTIMESTAMPS) {
            is->auddec.start_pts = st->start_time;
            is->auddec.start_pts_tb = st->time_base;
        }
        if (!decoder_start(&is->auddec, audio_thread, is))
            goto out;
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st = st;

        if ((ret = decoder_init(&is->viddec, avctx, &is->videoq, &is->continue_read_thread)) < 0)
            goto fail;
        if (!decoder_start(&is->viddec, video_thread, is))
            goto out;
        is->queue_attachments_req = true;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_stream = stream_index;
        is->subtitle_st = st;

        if ((ret = decoder_init(&is->subdec, avctx, &is->subtitleq, &is->continue_read_thread)) < 0)
            goto fail;
        if (!decoder_start(&is->subdec, subtitle_thread, is))
            goto out;
        break;
    default:
        break;
    }
    goto out;

fail:
    avcodec_free_context(&avctx);
out:

    return ret;
}

static int decode_interrupt_cb(void *ctx)
{
    VideoState *is = (VideoState*)ctx;
    return is->abort_request;
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue) {
    const auto [size, pkts, dur] = queue->getParams();
    return stream_id < 0 ||
           queue->isAborted() ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           pkts > MIN_FRAMES && ((dur == 0.0) || dur > 1.0);
}

static int is_realtime(AVFormatContext *s)
{
    if(   !strcmp(s->iformat->name, "rtp")
        || !strcmp(s->iformat->name, "rtsp")
        || !strcmp(s->iformat->name, "sdp")
        )
        return 1;

    if(s->pb && (   !strncmp(s->url, "rtp:", 4)
                  || !strncmp(s->url, "udp:", 4)
                  )
        )
        return 1;
    return 0;
}

/* this thread gets the stream from the disk or the network */
static int read_thread(VideoState *is)
{
    AVFormatContext *ic = NULL;
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];
    const AVDictionaryEntry *t = nullptr;
    AVDictionary* format_opts = nullptr;
    std::mutex wait_mutex;
    bool seek_by_bytes = false;

    memset(st_index, -1, sizeof(st_index));
    is->eof = 0;

    ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;
    av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);

    err = avformat_open_input(&ic, is->filename, is->iformat, &format_opts);
    if (err < 0) {
        qDebug() << "Failed to open URL: " << is->filename;
        ret = -1;
        goto fail;
    }

    ic->flags |= AVFMT_FLAG_GENPTS;

    if (true) {
        err = avformat_find_stream_info(ic, nullptr);

        if (err < 0) {
            av_log(NULL, AV_LOG_WARNING,
                   "%s: could not find codec parameters\n", is->filename);
            ret = -1;
            goto fail;
        }
    }

    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

    seek_by_bytes = !(ic->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
                        !!(ic->iformat->flags & AVFMT_TS_DISCONT) &&
                        strcmp("ogg", ic->iformat->name);

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    // if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
    //     window_title = av_asprintf("%s - %s", t->value, input_filename);

    is->realtime = is_realtime(ic);

    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        st->discard = AVDISCARD_ALL;
    }

    st_index[AVMEDIA_TYPE_VIDEO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                                st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    st_index[AVMEDIA_TYPE_AUDIO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                                st_index[AVMEDIA_TYPE_AUDIO],
                                st_index[AVMEDIA_TYPE_VIDEO],
                                NULL, 0);
    st_index[AVMEDIA_TYPE_SUBTITLE] =
            av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                                st_index[AVMEDIA_TYPE_SUBTITLE],
                                (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                                     st_index[AVMEDIA_TYPE_AUDIO] :
                                     st_index[AVMEDIA_TYPE_VIDEO]),
                                NULL, 0);

    /* open the streams */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO], ic);
    }

    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO], ic);
    }

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE], ic);
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s'\n",
               is->filename);
        ret = -1;
        goto fail;
    }

    is->fmt_ctx = ic;

    for (;;) {
        if (is->abort_request)
            break;
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(ic);
            else
                av_read_play(ic);
        }

        if (is->paused &&
            (!strcmp(ic->iformat->name, "rtsp") ||
             (ic->pb && !strncmp(ic->url, "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            SDL_Delay(10);
            continue;
        }

        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;
            // FIXME the +-2 is due to rounding being not done in the correct direction in generation
            //      of the seek_pos/seek_rel variables

            ret = avformat_seek_file(ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR,
                       "%s: error while seeking\n", ic->url);
            } else {
                if (is->audio_stream >= 0)
                    is->audioq.flush();
                if (is->subtitle_stream >= 0)
                    is->subtitleq.flush();
                if (is->video_stream >= 0)
                    is->videoq.flush();
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                    set_clock(&is->extclk, NAN, 0);
                } else {
                    set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
                }
            }
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;
            if (is->paused)
                step_to_next_frame(is);
        }
        if (is->queue_attachments_req) {
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                CAVPacket pkt;
                if ((ret = av_packet_ref(pkt.av(), &is->video_st->attached_pic)) < 0)
                    goto fail;
                pkt.setTb(is->video_st->time_base);
                is->videoq.put(std::move(pkt));
                is->videoq.put_nullpacket(is->video_stream);
            }
            is->queue_attachments_req = 0;
        }

        /* if the queue are full, no need to read more */
        if (!is->realtime &&
            (is->audioq.size() + is->videoq.size() > MAX_QUEUE_SIZE
             || (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
                 stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq)))) {
            /* wait 10 ms */
            std::unique_lock lck(wait_mutex);
            is->continue_read_thread.wait_for(lck, std::chrono::milliseconds(10));
            continue;
        }
        // if (!is->paused &&
        //     (!is->audio_st || (is->auddec.finished_serial == is->audioq.serial && frame_queue_nb_remaining(&is->sampq) == 0)) &&
        //     (!is->video_st || (is->viddec.finished_serial == is->videoq.serial && frame_queue_nb_remaining(&is->pictq) == 0))) {
        //     if (loop != 1 && (!loop || --loop)) {
        //         stream_seek(is, start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
        //     } else if (autoexit) {
        //         ret = AVERROR_EOF;
        //         goto fail;
        //     }
        // }

        CAVPacket pkt;
        ret = av_read_frame(ic, pkt.av());
        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                if (is->video_stream >= 0)
                    is->videoq.put_nullpacket(is->video_stream);
                if (is->audio_stream >= 0)
                    is->audioq.put_nullpacket(is->audio_stream);
                if (is->subtitle_stream >= 0)
                    is->subtitleq.put_nullpacket(is->subtitle_stream);
                is->eof = 1;
            }
            if (ic->pb && ic->pb->error) {
                break;
            }
            std::unique_lock lck(wait_mutex);
            is->continue_read_thread.wait_for(lck, std::chrono::milliseconds(10));
            continue;
        } else {
            is->eof = 0;
        }

        const auto pkt_st_index = pkt.streamIndex();
        pkt.setTb(ic->streams[pkt_st_index]->time_base);
        if (pkt_st_index == is->audio_stream) {
            is->audioq.put(std::move(pkt));
        } else if (pkt_st_index == is->video_stream
                   && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            is->videoq.put(std::move(pkt));
        } else if (pkt_st_index == is->subtitle_stream) {
            is->subtitleq.put(std::move(pkt));
        }
    }

fail:
    /* close each stream */
    stream_component_close(is, is->audio_stream, ic);
    stream_component_close(is, is->video_stream, ic);
    stream_component_close(is, is->subtitle_stream, ic);

    avformat_close_input(&ic);

    return 0;
}

static VideoState *stream_open(const char *filename, SDLRenderer* renderer)
{
    VideoState *is = new VideoState;

    is->filename = av_strdup(filename);
    if (!is->filename)
        goto fail;
    is->sdl_renderer = renderer;

    /* start video display */
    if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
        goto fail;
    if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
        goto fail;
    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;

    init_clock(&is->vidclk, &is->videoq);
    init_clock(&is->audclk, &is->audioq);
    init_clock(&is->extclk, nullptr);

    try{
    is->read_thr = std::thread(read_thread, is);
    }catch(...){
        goto fail;
    }

    return is;

fail:
    stream_close(is);
    return NULL;
}

static void stream_cycle_channel(VideoState *is, AVMediaType codec_type, AVFormatContext* ic)
{
    int start_index, stream_index;
    int old_index;
    AVStream *st;
    AVProgram *p = NULL;
    int nb_streams = ic->nb_streams;

    if (codec_type == AVMEDIA_TYPE_VIDEO) {
        start_index = is->last_video_stream;
        old_index = is->video_stream;
    } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
        start_index = is->last_audio_stream;
        old_index = is->audio_stream;
    } else {
        start_index = is->last_subtitle_stream;
        old_index = is->subtitle_stream;
    }
    stream_index = start_index;

    if (codec_type != AVMEDIA_TYPE_VIDEO && is->video_stream != -1) {
        p = av_find_program_from_stream(ic, NULL, is->video_stream);
        if (p) {
            nb_streams = p->nb_stream_indexes;
            for (start_index = 0; start_index < nb_streams; start_index++)
                if (p->stream_index[start_index] == stream_index)
                    break;
            if (start_index == nb_streams)
                start_index = -1;
            stream_index = start_index;
        }
    }

    for (;;) {
        if (++stream_index >= nb_streams)
        {
            if (codec_type == AVMEDIA_TYPE_SUBTITLE)
            {
                stream_index = -1;
                is->last_subtitle_stream = -1;
                goto the_end;
            }
            if (start_index == -1)
                return;
            stream_index = 0;
        }
        if (stream_index == start_index)
            return;
        st = ic->streams[p ? p->stream_index[stream_index] : stream_index];
        if (st->codecpar->codec_type == codec_type) {
            /* check that parameters are OK */
            switch (codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                if (st->codecpar->sample_rate != 0 &&
                    st->codecpar->ch_layout.nb_channels != 0)
                    goto the_end;
                break;
            case AVMEDIA_TYPE_VIDEO:
            case AVMEDIA_TYPE_SUBTITLE:
                goto the_end;
            default:
                break;
            }
        }
    }
the_end:
    if (p && stream_index != -1)
        stream_index = p->stream_index[stream_index];
    av_log(NULL, AV_LOG_INFO, "Switch %s stream from #%d to #%d\n",
           av_get_media_type_string(codec_type),
           old_index,
           stream_index);

    stream_component_close(is, old_index, ic);
    stream_component_open(is, stream_index, ic);
}

static void handle_ao_change(VideoState* ctx){
    std::scoped_lock lck(ctx->ao_mutex);

    const auto rate = ctx->requested_ao_rate, chn = ctx->requested_ao_channels;
    const bool close_ao = rate <= 0 && chn <= 0;
    //qDebug() << "Rate="<<rate<<", chn="<<chn;
    if(close_ao || (rate != ctx->ao_rate || chn != ctx->ao_channels)){
        if(ctx->astream){
            SDL_DestroyAudioStream(ctx->astream);
        }
        ctx->astream = nullptr;
        ctx->ao_rate = ctx->requested_ao_rate;
        ctx->ao_channels = ctx->requested_ao_channels;

        if(ctx->ao_rate > 0 && ctx->ao_channels > 0){
            ctx->astream = audio_open(ctx, ctx->ao_channels, ctx->ao_rate, ctx->audio_tgt);
            ctx->audio_src = ctx->audio_tgt;
            qDebug() << "Opened audio: " << ctx->ao_rate <<"Hz, "<<ctx->ao_channels;
        } else{
            ctx->audio_src = ctx->audio_tgt = AudioParams();
        }
    }
}

static double refresh_audio(VideoState* ctx){
    handle_ao_change(ctx);

    double remaining_time = REFRESH_RATE;

    if(!ctx->paused && ctx->astream){
        double buffered_time = double(SDL_GetAudioStreamQueued(ctx->astream) / ctx->audio_tgt.frame_size)/ctx->audio_tgt.freq;
        if(buffered_time > SDL_AUDIO_BUFLEN){
            remaining_time = buffered_time / 4;
        } else{
            const auto resampled_data_size = audio_decode_frame(ctx);
            if(resampled_data_size > 0){
                SDL_PutAudioStreamData(ctx->astream, ctx->audio_buf.data(), resampled_data_size);
                buffered_time = double(SDL_GetAudioStreamQueued(ctx->astream) / ctx->audio_tgt.frame_size)/ctx->audio_tgt.freq;
                if(!std::isnan(ctx->audio_clock)){
                    set_clock(&ctx->audclk, ctx->audio_clock - buffered_time, ctx->audio_clock_serial);
                    sync_clock_to_slave(&ctx->extclk, &ctx->audclk);
                }
                remaining_time = buffered_time < SDL_AUDIO_BUFLEN ? 0.0 : buffered_time / 4;
            }
        }
    }

    return remaining_time;
}

static double refresh_video(VideoState* ctx){
    double remaining_time = REFRESH_RATE;
    video_refresh(ctx, &remaining_time);
    return remaining_time;
}

static double playback_loop(VideoState* ctx){
    const auto audio_remaining_time = refresh_audio(ctx);
    const auto video_remaining_time = refresh_video(ctx);
    //check for potential errors here
    return std::min(audio_remaining_time, video_remaining_time);
}

static void seek_chapter(VideoState *is, int incr, AVFormatContext* ic)
{
    int64_t pos = get_master_clock(is) * AV_TIME_BASE;
    int i;

    if (!ic->nb_chapters)
        return;

    /* find the current chapter */
    for (i = 0; i < ic->nb_chapters; i++) {
        const AVChapter *ch = ic->chapters[i];
        if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
            i--;
            break;
        }
    }

    i += incr;
    i = FFMAX(i, 0);
    if (i >= ic->nb_chapters)
        return;

    av_log(NULL, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
    stream_seek(is, av_rescale_q(ic->chapters[i]->start, ic->chapters[i]->time_base,
                                 AV_TIME_BASE_Q), 0, 0);
}

// /* handle an event sent by the GUI */
// static void event_loop(VideoState *cur_stream)
// {
//     SDL_Event event;
//     double incr, pos, frac;

//     for (;;) {
//         double x;
//         refresh_loop_wait_event(cur_stream, &event);
//         switch (event.type) {
//         case SDL_KEYDOWN:
//             if (exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
//                 do_exit(cur_stream);
//                 break;
//             }
//             // If we don't yet have a window, skip all key events, because read_thread might still be initializing...
//             if (!cur_stream->width)
//                 continue;
//             switch (event.key.keysym.sym) {
//             case SDLK_f:
//                 toggle_full_screen(cur_stream);
//                 cur_stream->force_refresh = 1;
//                 break;
//             case SDLK_p:
//             case SDLK_SPACE:
//                 toggle_pause(cur_stream);
//                 break;
//             case SDLK_m:
//                 toggle_mute(cur_stream);
//                 break;
//             case SDLK_KP_MULTIPLY:
//             case SDLK_0:
//                 update_volume(cur_stream, 1, SDL_VOLUME_STEP);
//                 break;
//             case SDLK_KP_DIVIDE:
//             case SDLK_9:
//                 update_volume(cur_stream, -1, SDL_VOLUME_STEP);
//                 break;
//             case SDLK_s: // S: Step to next frame
//                 step_to_next_frame(cur_stream);
//                 break;
//             case SDLK_a:
//                 stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
//                 break;
//             case SDLK_v:
//                 stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
//                 break;
//             case SDLK_c:
//                 stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
//                 stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
//                 stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
//                 break;
//             case SDLK_t:
//                 stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
//                 break;
//             case SDLK_w:
//                 if (cur_stream->show_mode == SHOW_MODE_VIDEO && cur_stream->vfilter_idx < nb_vfilters - 1) {
//                     if (++cur_stream->vfilter_idx >= nb_vfilters)
//                         cur_stream->vfilter_idx = 0;
//                 } else {
//                     cur_stream->vfilter_idx = 0;
//                     toggle_audio_display(cur_stream);
//                 }
//                 break;
//             case SDLK_PAGEUP:
//                 if (cur_stream->ic->nb_chapters <= 1) {
//                     incr = 600.0;
//                     goto do_seek;
//                 }
//                 seek_chapter(cur_stream, 1);
//                 break;
//             case SDLK_PAGEDOWN:
//                 if (cur_stream->ic->nb_chapters <= 1) {
//                     incr = -600.0;
//                     goto do_seek;
//                 }
//                 seek_chapter(cur_stream, -1);
//                 break;
//             case SDLK_LEFT:
//                 incr = seek_interval ? -seek_interval : -10.0;
//                 goto do_seek;
//             case SDLK_RIGHT:
//                 incr = seek_interval ? seek_interval : 10.0;
//                 goto do_seek;
//             case SDLK_UP:
//                 incr = 60.0;
//                 goto do_seek;
//             case SDLK_DOWN:
//                 incr = -60.0;
//             do_seek:
//                 if (seek_by_bytes) {
//                     pos = -1;
//                     if (pos < 0 && cur_stream->video_stream >= 0)
//                         pos = frame_queue_last_pos(&cur_stream->pictq);
//                     if (pos < 0 && cur_stream->audio_stream >= 0)
//                         pos = frame_queue_last_pos(&cur_stream->sampq);
//                     if (pos < 0)
//                         pos = avio_tell(cur_stream->ic->pb);
//                     if (cur_stream->ic->bit_rate)
//                         incr *= cur_stream->ic->bit_rate / 8.0;
//                     else
//                         incr *= 180000.0;
//                     pos += incr;
//                     stream_seek(cur_stream, pos, incr, 1);
//                 } else {
//                     pos = get_master_clock(cur_stream);
//                     if (isnan(pos))
//                         pos = (double)cur_stream->seek_pos / AV_TIME_BASE;
//                     pos += incr;
//                     if (cur_stream->ic->start_time != AV_NOPTS_VALUE && pos < cur_stream->ic->start_time / (double)AV_TIME_BASE)
//                         pos = cur_stream->ic->start_time / (double)AV_TIME_BASE;
//                     stream_seek(cur_stream, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), 0);
//                 }
//                 break;
//             default:
//                 break;
//             }
//             break;
//         case SDL_MOUSEBUTTONDOWN:
//             if (exit_on_mousedown) {
//                 do_exit(cur_stream);
//                 break;
//             }
//             if (event.button.button == SDL_BUTTON_LEFT) {
//                 static int64_t last_mouse_left_click = 0;
//                 if (av_gettime_relative() - last_mouse_left_click <= 500000) {
//                     toggle_full_screen(cur_stream);
//                     cur_stream->force_refresh = 1;
//                     last_mouse_left_click = 0;
//                 } else {
//                     last_mouse_left_click = av_gettime_relative();
//                 }
//             }
//         case SDL_MOUSEMOTION:
//             if (cursor_hidden) {
//                 SDL_ShowCursor(1);
//                 cursor_hidden = 0;
//             }
//             cursor_last_shown = av_gettime_relative();
//             if (event.type == SDL_MOUSEBUTTONDOWN) {
//                 if (event.button.button != SDL_BUTTON_RIGHT)
//                     break;
//                 x = event.button.x;
//             } else {
//                 if (!(event.motion.state & SDL_BUTTON_RMASK))
//                     break;
//                 x = event.motion.x;
//             }
//             if (seek_by_bytes || cur_stream->ic->duration <= 0) {
//                 uint64_t size =  avio_size(cur_stream->ic->pb);
//                 stream_seek(cur_stream, size*x/cur_stream->width, 0, 1);
//             } else {
//                 int64_t ts;
//                 int ns, hh, mm, ss;
//                 int tns, thh, tmm, tss;
//                 tns  = cur_stream->ic->duration / 1000000LL;
//                 thh  = tns / 3600;
//                 tmm  = (tns % 3600) / 60;
//                 tss  = (tns % 60);
//                 frac = x / cur_stream->width;
//                 ns   = frac * tns;
//                 hh   = ns / 3600;
//                 mm   = (ns % 3600) / 60;
//                 ss   = (ns % 60);
//                 av_log(NULL, AV_LOG_INFO,
//                        "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n", frac*100,
//                        hh, mm, ss, thh, tmm, tss);
//                 ts = frac * cur_stream->ic->duration;
//                 if (cur_stream->ic->start_time != AV_NOPTS_VALUE)
//                     ts += cur_stream->ic->start_time;
//                 stream_seek(cur_stream, ts, 0, 0);
//             }
//             break;
//         case SDL_WINDOWEVENT:
//             switch (event.window.event) {
//             case SDL_WINDOWEVENT_SIZE_CHANGED:
//                 screen_width  = cur_stream->width  = event.window.data1;
//                 screen_height = cur_stream->height = event.window.data2;
//                 if (cur_stream->vis_texture) {
//                     SDL_DestroyTexture(cur_stream->vis_texture);
//                     cur_stream->vis_texture = NULL;
//                 }
//                 if (vk_renderer)
//                     vk_renderer_resize(vk_renderer, screen_width, screen_height);
//             case SDL_WINDOWEVENT_EXPOSED:
//                 cur_stream->force_refresh = 1;
//             }
//             break;
//         case SDL_QUIT:
//         case FF_QUIT_EVENT:
//             do_exit(cur_stream);
//             break;
//         default:
//             break;
//         }
//     }
// }


void PlayerCore::openURL(QUrl url){
    qDebug() << "Opening file";
    if(player_ctx){
        stream_close(player_ctx);
    }

    if((player_ctx = stream_open(url.toString().toStdString().c_str(), video_renderer))){
        refreshPlayback();
        qDebug() << "Opened";
    }

}

void PlayerCore::stopPlayback(){
    if(player_ctx){
        stream_close(player_ctx);
        player_ctx = nullptr;
    }
}

void PlayerCore::pausePlayback(){

}

void PlayerCore::resumePlayback(){

}
void PlayerCore::togglePause(){

}

bool PlayerCore::is_active() {

}

void PlayerCore::requestSeekPercent(double percent){

}

void PlayerCore::requestSeekIncr(double incr){

}

void PlayerCore::reportStreamDuration(double dur){
    stream_duration = dur;
}

void PlayerCore::updateStreamPos(double pos){
    cur_pos = pos;
    emit updatePlaybackPos(pos, stream_duration);
}

void PlayerCore::log(const char* fmt, ...){
    std::va_list args;
    va_start(args, fmt);
    const auto msg = QString::vasprintf(fmt, args);
    va_end(args);
    QMetaObject::invokeMethod(loggerW, &LoggerWidget::logMessage, msg);
}

static PlayerCore* plcore_inst = nullptr;

PlayerCore& PlayerCore::instance() {
    return *plcore_inst;
}

PlayerCore::PlayerCore(QObject* parent, VideoDisplayWidget* dw, LoggerWidget* lw): QObject(parent), video_dw(dw), loggerW(lw), refresh_timer(this){
    plcore_inst = this;

    video_renderer = dw->getSDLRenderer();

    connect(&refresh_timer, &QTimer::timeout, this, &PlayerCore::refreshPlayback);
    connect(this, &PlayerCore::sigReportStreamDuration, this, &PlayerCore::reportStreamDuration);
    connect(this, &PlayerCore::sigUpdateStreamPos, this, &PlayerCore::updateStreamPos);
}

PlayerCore::~PlayerCore(){
    stopPlayback();
}

void PlayerCore::refreshPlayback(){
    if(player_ctx){
        const auto remaining_time = playback_loop(player_ctx) * 1000;
        //qDebug() << "Remaining time: " << remaining_time;
        refresh_timer.setInterval(static_cast<int>(remaining_time));
        if(!refresh_timer.isActive())
            refresh_timer.start(Qt::PreciseTimer);
    } else{
        refresh_timer.stop();
    }
}


