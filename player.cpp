/*
 * Copyright (c) 2003 Fabrice Bellard
 *
 */

/**
 * @file
 * simple media player based on the FFmpeg libraries
 */

#include "src/player.hpp"
#include "src/GUI/VideoDock.hpp"
#include "playback/PacketQueue.hpp"
#include "playback/clock.hpp"

#include <QApplication>
#include <thread>

extern "C"{
#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/dict.h"
#include "libavutil/samplefmt.h"
#include "libavutil/bprint.h"
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libswresample/swresample.h"
#include "libavutil/display.h"
#include "libavutil/samplefmt.h"

#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
}

#include <SDL3/SDL.h>

#define MAX_QUEUE_SIZE (20 * 1024 * 1024)
#define MIN_FRAMES 30

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

#define CURSOR_HIDE_DELAY 1000000

#define USE_ONEPASS_SUBTITLE_RENDER 1

#define VIDEO_PICTURE_QUEUE_SIZE 4
#define SUBPICTURE_QUEUE_SIZE 32
#define SAMPLE_QUEUE_SIZE 12
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))


struct AudioParams {
    int freq = 0;
    AVChannelLayout ch_layout{};
    AVSampleFormat fmt = AV_SAMPLE_FMT_NONE;
};

struct FrameData {
    int64_t pkt_pos = -1LL;
};

/* Common struct for handling all types of decoded data and allocated render buffers. */
struct Frame {
    AVFrame *frame = nullptr;
    AVSubtitle sub{};
    int serial = -1;
    double pts = 0.0;           /* presentation timestamp for the frame */
    double duration = 0.0;      /* estimated duration of the frame */
    int64_t pos = -1LL;          /* byte position of the frame in the input file */
    int width = 0;
    int height = 0;
    int format = 0;
    AVRational sar{};
    bool uploaded = false;
    bool flip_v = false;
    int64_t pkt_pos = AV_NOPTS_VALUE;
};

struct FrameQueue {
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
};

struct Decoder {
    AVPacket *pkt = nullptr;
    PacketQueue *queue = nullptr;
    AVCodecContext *avctx = nullptr;
    int pkt_serial = -1;
    int finished = 0;
    int packet_pending = 0;
    std::condition_variable *empty_queue_cond = nullptr;
    int64_t start_pts = 0;
    AVRational start_pts_tb{};
    int64_t next_pts = 0;
    AVRational next_pts_tb{};
    std::thread decoder_tid;
};

struct SeekInfo {
    enum SeekType{
        SEEK_NONE, SEEK_PERCENT, SEEK_INCREMENT, SEEK_CHAPTER
    };

    SeekType type = SEEK_NONE;
    double percent = 0.0;
    double increment = 0.0;
};

struct VideoState {
    std::thread read_thr;
    std::thread audio_render_thr;
    std::atomic_bool pause_request = false;
    bool queue_attachments_req = 0;
    int seek_req = 0;
    int seek_flags = 0;
    int64_t seek_pos = 0;
    int64_t seek_rel = 0;
    AVFormatContext *ic = nullptr;

    Clock audclk;
    Clock vidclk;

    FrameQueue pictq;
    FrameQueue subpq;
    FrameQueue sampq;

    Decoder auddec;
    Decoder viddec;
    Decoder subdec;

    int audio_stream = -1;

    AVStream *audio_st = nullptr;
    PacketQueue audioq;

    float audio_volume = 1.0f;
    AudioParams audio_src;
    AudioParams audio_filter_src;
    AudioParams audio_tgt;
    SwrContext *swr_ctx = nullptr;
    SDL_AudioStream* sdl_astream = nullptr;

    SDL_Texture *sub_texture = nullptr;
    SDL_Texture *vid_texture = nullptr;

    int subtitle_stream = -1;
    AVStream *subtitle_st = nullptr;
    PacketQueue subtitleq;

    double frame_timer = 0.0;
    int video_stream = -1;
    AVStream *video_st = nullptr;
    PacketQueue videoq;
    double max_frame_duration = 0.0;      // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
    SwsContext *sub_convert_ctx = nullptr;
    bool eof = false;

    std::string url;
    int width = 0, height = 0;

    AVFilterContext *in_video_filter = nullptr;   // the first filter in the video chain
    AVFilterContext *out_video_filter = nullptr;  // the last filter in the video chain
    AVFilterContext *in_audio_filter = nullptr;   // the first filter in the audio chain
    AVFilterContext *out_audio_filter = nullptr;  // the last filter in the audio chain
    AVFilterGraph *agraph = nullptr;              // audio filter graph

    int last_video_stream = -1, last_audio_stream = -1, last_subtitle_stream = -1;

    std::condition_variable continue_read_thread;
};

/* options specified by the user */
static int screen_width  = 0;
static int screen_height = 0;
static int seek_by_bytes = -1;
static float seek_interval = 5;
static int autoexit = 0;
static int loop = 1;

static SDL_Renderer *renderer;
std::vector<SDL_PixelFormat> supported_pix_fmts;

std::atomic_bool quit_request = false;
bool quitRequested(){return quit_request.load();}

//This function is supposed to be called exclusively from the main thread
void setQuitRequest(bool quit){quit_request.store(quit);}

static constexpr struct TextureFormatEntry {
    AVPixelFormat format;
    SDL_PixelFormat texture_fmt;
} sdl_texture_format_map[] = {
    { AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332 },
    { AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_XRGB4444 },
    { AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_XRGB1555 },
    { AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_XBGR1555 },
    { AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565 },
    { AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565 },
    { AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24 },
    { AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24 },
    { AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_XRGB8888 },
    { AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_XBGR8888 },
    { AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
    { AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
    { AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888 },
    { AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888 },
    { AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888 },
    { AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888 },
    { AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV },
    { AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2 },
    { AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY },
    { AV_PIX_FMT_NONE,           SDL_PIXELFORMAT_UNKNOWN },
};

static inline
    int cmp_audio_fmts(AVSampleFormat fmt1, int64_t channel_count1,
                   AVSampleFormat fmt2, int64_t channel_count2)
{
    /* If channel count == 1, planar and non-planar formats are the same */
    if ((channel_count1 == 1) && (channel_count2 == 1))
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return (channel_count1 != channel_count2) || (fmt1 != fmt2);
}



static int decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, std::condition_variable *empty_queue_cond) {
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
        if (d->queue->serial == d->pkt_serial) {
            do {
                if (d->queue->abort_request)
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
                        AVRational tb = (AVRational){1, frame->sample_rate};
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
                default:
                    break;
                }
                if (ret == AVERROR_EOF) {
                    d->finished = d->pkt_serial;
                    avcodec_flush_buffers(d->avctx);
                    return 0;
                }
                if (ret >= 0)
                    return 1;
            } while (ret != AVERROR(EAGAIN));
        }

        do {
            if (d->queue->nb_packets == 0)
                d->empty_queue_cond->notify_one();
            if (d->packet_pending) {
                d->packet_pending = 0;
            } else {
                int old_serial = d->pkt_serial;
                if (d->queue->get(d->pkt, 1, &d->pkt_serial) < 0)
                    return -1;
                if (old_serial != d->pkt_serial) {
                    avcodec_flush_buffers(d->avctx);
                    d->finished = 0;
                    d->next_pts = d->start_pts;
                    d->next_pts_tb = d->start_pts_tb;
                }
            }
            if (d->queue->serial == d->pkt_serial)
                break;
            av_packet_unref(d->pkt);
        } while (1);

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
                //TODO: add a pkt_pos field to the future CAVFrame class and use it instead
                auto fd = av_buffer_allocz(sizeof(FrameData));
                if (!fd)
                    return AVERROR(ENOMEM);
                ((FrameData*)fd->data)->pkt_pos = d->pkt->pos;
                d->pkt->opaque_ref = fd;
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
    f->pktq = pktq;
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    for (int i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    return 0;
}

static void frame_queue_destroy(FrameQueue *f)
{
    for (int i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
    }
}

static void frame_queue_signal(FrameQueue *f)
{
    std::unique_lock lck(f->mutex);
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
           !f->pktq->abort_request) {
        f->cond.wait(lck);
    }
    lck.unlock();

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[f->windex];
}

static Frame *frame_queue_peek_readable(FrameQueue *f)
{
    /* wait until we have a readable a new frame */
   std::unique_lock lck(f->mutex);
    while (f->size - f->rindex_shown <= 0 &&
           !f->pktq->abort_request) {
        f->cond.wait(lck);
    }
    lck.unlock();

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static void frame_queue_push(FrameQueue *f)
{
    if (++f->windex == f->max_size)
        f->windex = 0;
    std::unique_lock lck(f->mutex);
    ++f->size;
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
    std::unique_lock lck(f->mutex);
    --f->size;
    f->cond.notify_one();
}

/* return the number of undisplayed frames in the queue */
static int frame_queue_nb_remaining(FrameQueue *f)
{
    return f->size - f->rindex_shown;
}

/* return last shown position */
static int64_t frame_queue_last_pos(FrameQueue *f)
{
    Frame *fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->pktq->serial)
        return fp->pos;
    else
        return -1;
}

static void decoder_abort(Decoder *d, FrameQueue *fq)
{
    d->queue->abort();
    frame_queue_signal(fq);
    if(d->decoder_tid.joinable())
        d->decoder_tid.join();
    d->queue->flush();
}

static int realloc_texture(SDL_Texture **texture, SDL_PixelFormat new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture)
{
    SDL_PixelFormat format;
    int access, w, h;

    auto must_realloc_texture = [&](SDL_Texture* tex){
        if(!tex) return true;
        auto props = SDL_GetTextureProperties(tex);
        w = SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_WIDTH_NUMBER, 0);
        h = SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_HEIGHT_NUMBER, 0);
        access = SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_ACCESS_NUMBER, -1);
        auto integer_format = SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_FORMAT_NUMBER, SDL_GPU_TEXTUREFORMAT_INVALID);
        format = (SDL_PixelFormat)integer_format;
        return new_width != w || new_height != h || new_format != format;
    };

    if (must_realloc_texture(*texture)) {
        void *pixels;
        int pitch;
        if (*texture)
            SDL_DestroyTexture(*texture);
        if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
            return -1;
        if (!SDL_SetTextureBlendMode(*texture, blendmode))
            return -1;
        if (init_texture) {
            if (!SDL_LockTexture(*texture, NULL, &pixels, &pitch))
                return -1;
            memset(pixels, 0, pitch * new_height);
            SDL_UnlockTexture(*texture);
        }
    }
    return 0;
}

static void calculate_display_rect(SDL_FRect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar)
{
    AVRational aspect_ratio = pic_sar;
    int64_t width, height, x, y;

    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
        aspect_ratio = av_make_q(1, 1);

    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    /* XXX: we suppose the screen has a 1.0 pixel ratio */
    height = scr_height;
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
    if (width > scr_width) {
        width = scr_width;
        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
    }
    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;
    rect->x = scr_xleft + x;
    rect->y = scr_ytop  + y;
    rect->w = FFMAX((int)width,  1);
    rect->h = FFMAX((int)height, 1);
}

static void get_sdl_pix_fmt_and_blendmode(int format, SDL_PixelFormat *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode)
{
    *sdl_blendmode = SDL_BLENDMODE_NONE;
    *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
    if (format == AV_PIX_FMT_RGB32   ||
        format == AV_PIX_FMT_RGB32_1 ||
        format == AV_PIX_FMT_BGR32   ||
        format == AV_PIX_FMT_BGR32_1)
        *sdl_blendmode = SDL_BLENDMODE_BLEND;
    for (int i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
        if (format == sdl_texture_format_map[i].format) {
            *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
            return;
        }
    }
}

static bool upload_texture(SDL_Texture **tex, AVFrame *frame)
{
    bool ret = 0;
    SDL_PixelFormat sdl_pix_fmt;
    SDL_BlendMode sdl_blendmode;
    get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
    if (realloc_texture(tex, sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt, frame->width, frame->height, sdl_blendmode, 0) < 0)
        return false;
    switch (sdl_pix_fmt) {
    case SDL_PIXELFORMAT_IYUV:
        if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
            ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0], frame->linesize[0],
                                       frame->data[1], frame->linesize[1],
                                       frame->data[2], frame->linesize[2]);
        } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
            ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height                    - 1), -frame->linesize[0],
                                       frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1],
                                       frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]);
        } else {
            av_log(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
            return false;
        }
        break;
    default:
        if (frame->linesize[0] < 0) {
            ret = SDL_UpdateTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);
        } else {
            ret = SDL_UpdateTexture(*tex, NULL, frame->data[0], frame->linesize[0]);
        }
        break;
    }
    return ret;
}

static enum AVColorSpace sdl_supported_color_spaces[] = {
    AVCOL_SPC_BT709,
    AVCOL_SPC_BT470BG,
    AVCOL_SPC_SMPTE170M,
    AVCOL_SPC_UNSPECIFIED,
};

/*static SDL_Colorspace set_sdl_yuv_conversion_mode(AVFrame *frame)
{
    const auto pix_fmt = static_cast<AVPixelFormat>(frame->format);
    const auto pixfmt_desc = av_pix_fmt_desc_get(pix_fmt);
    const bool is_rgb = (pixfmt_desc->flags & AV_PIX_FMT_FLAG_RGB);
    SDL_Colorspace mode = is_rgb ? SDL_COLORSPACE_RGB_DEFAULT : SDL_COLORSPACE_YUV_DEFAULT;
    if (frame && (frame->format == AV_PIX_FMT_YUV420P ||
                  frame->format == AV_PIX_FMT_YUYV422 ||
                  frame->format == AV_PIX_FMT_UYVY422)) {
        if (frame->color_range == AVCOL_RANGE_JPEG)
            mode = SDL_COLORSPACE_JPEG;
        else if (frame->colorspace == AVCOL_SPC_BT709)
            mode = SDL_COLORSPACE_BT709_FULL;
        else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M)
            mode = SDL_COLORSPACE_BT601_FULL;
    }
    SDL_SetYUVConversionMode(mode); // FIXME: no support for linear transfer
}*/

static void video_image_display(VideoState *is)
{
    Frame *vp;
    Frame *sp = NULL;
    SDL_FRect rect;

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
                    if (realloc_texture(&is->sub_texture, SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height, SDL_BLENDMODE_BLEND, 1) < 0)
                        return;

                    for (i = 0; i < sp->sub.num_rects; i++) {
                        AVSubtitleRect *sub_rect = sp->sub.rects[i];

                        sub_rect->x = av_clip(sub_rect->x, 0, sp->width );
                        sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
                        sub_rect->w = av_clip(sub_rect->w, 0, sp->width  - sub_rect->x);
                        sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

                        is->sub_convert_ctx = sws_getCachedContext(is->sub_convert_ctx,
                                                                   sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                                                                   sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
                                                                   0, NULL, NULL, NULL);
                        if (!is->sub_convert_ctx) {
                            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                            return;
                        }
                        if (SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)pixels, pitch)) {
                            sws_scale(is->sub_convert_ctx, (const uint8_t * const *)sub_rect->data, sub_rect->linesize,
                                      0, sub_rect->h, pixels, pitch);
                            SDL_UnlockTexture(is->sub_texture);
                        }
                    }
                    sp->uploaded = 1;
                }
            } else
                sp = NULL;
        }
    }

    calculate_display_rect(&rect, 0, 0, is->width, is->height, vp->width, vp->height, vp->sar);
    //set_sdl_yuv_conversion_mode(vp->frame);

    if (!vp->uploaded) {
        if (upload_texture(&is->vid_texture, vp->frame) < 0) {
            //set_sdl_yuv_conversion_mode(NULL);
            return;
        }
        vp->uploaded = 1;
        vp->flip_v = vp->frame->linesize[0] < 0;
    }

    SDL_RenderTextureRotated(renderer, is->vid_texture, NULL, &rect, 0, NULL, vp->flip_v ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);
    //set_sdl_yuv_conversion_mode(NULL);
    if (sp) {
#if USE_ONEPASS_SUBTITLE_RENDER
        SDL_RenderTexture(renderer, is->sub_texture, NULL, &rect);
#else
        int i;
        double xratio = (double)rect.w / (double)sp->width;
        double yratio = (double)rect.h / (double)sp->height;
        for (i = 0; i < sp->sub.num_rects; i++) {
            SDL_Rect *sub_rect = (SDL_Rect*)sp->sub.rects[i];
            SDL_Rect target = {.x = rect.x + sub_rect->x * xratio,
                               .y = rect.y + sub_rect->y * yratio,
                               .w = sub_rect->w * xratio,
                               .h = sub_rect->h * yratio};
            SDL_RenderCopy(renderer, is->sub_texture, sub_rect, &target);
        }
#endif
    }
}

static void stream_component_close(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        decoder_abort(&is->auddec, &is->sampq);
        if(is->audio_render_thr.joinable())
            is->audio_render_thr.join();
        if(is->sdl_astream)
            SDL_CloseAudioDevice(SDL_GetAudioStreamDevice(is->sdl_astream));
        decoder_destroy(&is->auddec);
        swr_free(&is->swr_ctx);
        is->sdl_astream = nullptr;
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
    assert(quitRequested());
    if(is->read_thr.joinable())
        is->read_thr.join();

    /* close each stream */
    if (is->audio_stream >= 0)
        stream_component_close(is, is->audio_stream);
    if (is->video_stream >= 0)
        stream_component_close(is, is->video_stream);
    if (is->subtitle_stream >= 0)
        stream_component_close(is, is->subtitle_stream);

    avformat_close_input(&is->ic);

    is->videoq.destroy();
    is->audioq.destroy();
    is->subtitleq.destroy();

    /* free all pictures */
    frame_queue_destroy(&is->pictq);
    frame_queue_destroy(&is->sampq);
    frame_queue_destroy(&is->subpq);
    sws_freeContext(is->sub_convert_ctx);

    if (is->vid_texture)
        SDL_DestroyTexture(is->vid_texture);
    if (is->sub_texture)
        SDL_DestroyTexture(is->sub_texture);
    delete is;
}

/* display the current picture, if any */
static void video_display(VideoState *is)
{
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    if (is->video_st)
        video_image_display(is);
    SDL_RenderPresent(renderer);
}

/* get the current master clock value */
static double get_master_clock(VideoState *is)
{
    return is->audio_st? is->audclk.get() : is->vidclk.get();;
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
    const auto new_paused = !is->pause_request.load();
    is->pause_request.store(new_paused);
}

static double compute_target_delay(double delay, VideoState *is)
{
    if(is->audio_st){
        double sync_threshold, diff = 0;
        /* update delay to follow master synchronisation source */
        /* if video is slave, we try to correct big delays by
           duplicating or deleting a frame */
        diff = is->vidclk.get() - is->audclk.get();

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
    is->vidclk.set(pts, serial);
}

/* called to display each frame */
static void video_refresh(VideoState *is, double *remaining_time, bool paused, bool& force_refresh)
{
    double time;

    Frame *sp, *sp2;

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

            if (vp->serial != is->videoq.serial) {
                frame_queue_next(&is->pictq);
                goto retry;
            }

            if (lastvp->serial != vp->serial)
                is->frame_timer = av_gettime_relative() / 1000000.0;

            if (paused)
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
                if(time > is->frame_timer + duration){
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

                    if (sp->serial != is->subtitleq.serial
                        || (is->vidclk.pts() > (sp->pts + ((float) sp->sub.end_display_time / 1000)))
                        || (sp2 && is->vidclk.pts() > (sp2->pts + ((float) sp2->sub.start_display_time / 1000))))
                    {
                        if (sp->uploaded) {
                            for (int i = 0; i < sp->sub.num_rects; i++) {
                                AVSubtitleRect *sub_rect = sp->sub.rects[i];
                                uint8_t *pixels;
                                int pitch, j;

                                if (SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)&pixels, &pitch)) {
                                    for (j = 0; j < sub_rect->h; j++, pixels += pitch)
                                        memset(pixels, 0, sub_rect->w << 2);
                                    SDL_UnlockTexture(is->sub_texture);
                                }
                            }
                        }
                        frame_queue_next(&is->subpq);
                    } else {
                        break;
                    }
                }
            }

            frame_queue_next(&is->pictq);
            force_refresh = true;
        }
    display:
        /* display picture */
        if (force_refresh && is->pictq.rindex_shown)
            video_display(is);
    }
    force_refresh = false;
}

static int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    Frame *vp;

    if (!(vp = frame_queue_peek_writable(&is->pictq)))
        return -1;

    vp->sar = src_frame->sample_aspect_ratio;

    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;
    vp->uploaded = false;
    vp->flip_v = src_frame->linesize[0] < 0;

    av_frame_move_ref(vp->frame, src_frame);
    frame_queue_push(&is->pictq);
    return 0;
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

static int configure_video_filters(AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame)
{
    enum AVPixelFormat pix_fmts[FF_ARRAY_ELEMS(sdl_texture_format_map)];
    char sws_flags_str[512] = "";
    char buffersrc_args[256];
    int ret;
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    AVCodecParameters *codecpar = is->video_st->codecpar;
    AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);
    const AVDictionaryEntry *e = NULL;
    int nb_pix_fmts = 0;
    int i, j;
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();

    if (!par)
        return AVERROR(ENOMEM);

    for (i = 0; i < supported_pix_fmts.size(); i++) {
        for (j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; j++) {
            if (supported_pix_fmts[i] == sdl_texture_format_map[j].texture_fmt) {
                pix_fmts[nb_pix_fmts++] = sdl_texture_format_map[j].format;
                break;
            }
        }
    }
    pix_fmts[nb_pix_fmts] = AV_PIX_FMT_NONE;

    AVDictionary* sws_dict = nullptr;
    while ((e = av_dict_iterate(sws_dict, e))) {
        if (!strcmp(e->key, "sws_flags")) {
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
        } else
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
    }
    if (strlen(sws_flags_str))
        sws_flags_str[strlen(sws_flags_str)-1] = '\0';

    graph->scale_sws_opts = av_strdup(sws_flags_str);

    snprintf(buffersrc_args, sizeof(buffersrc_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:"
             "colorspace=%d:range=%d",
             frame->width, frame->height, frame->format,
             is->video_st->time_base.num, is->video_st->time_base.den,
             codecpar->sample_aspect_ratio.num, FFMAX(codecpar->sample_aspect_ratio.den, 1),
             frame->colorspace, frame->color_range);
    if (fr.num && fr.den)
        av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);

    if ((ret = avfilter_graph_create_filter(&filt_src,
                                            avfilter_get_by_name("buffer"),
                                            "ffplay_buffer", buffersrc_args, NULL,
                                            graph)) < 0)
        goto fail;
    par->hw_frames_ctx = frame->hw_frames_ctx;
    ret = av_buffersrc_parameters_set(filt_src, par);
    if (ret < 0)
        goto fail;

    ret = avfilter_graph_create_filter(&filt_out,
                                       avfilter_get_by_name("buffersink"),
                                       "ffplay_buffersink", NULL, NULL, graph);
    if (ret < 0)
        goto fail;

    if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts,  AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto fail;
    if ((ret = av_opt_set_int_list(filt_out, "color_spaces", sdl_supported_color_spaces,  AVCOL_SPC_UNSPECIFIED, AV_OPT_SEARCH_CHILDREN)) < 0)
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

    if (1) {
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

        auto get_rotation = [](const int32_t *displaymatrix)->double
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
        };

        const auto theta = get_rotation(displaymatrix);

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

if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
    goto fail;

is->in_video_filter  = filt_src;
is->out_video_filter = filt_out;

fail:
       av_freep(&par);
return ret;
}

static int configure_audio_filters(VideoState *is, const char *afilters, int force_output_format)
{
    static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_NONE };
    int sample_rates[2] = { 0, -1 };
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
    char aresample_swr_opts[512] = "";
    const AVDictionaryEntry *e = NULL;
    AVBPrint bp{};
    char asrc_args[256]{};
    int ret;

    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);
    is->agraph->nb_threads = 0;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);

    AVDictionary* swr_opts = nullptr;
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


    ret = avfilter_graph_create_filter(&filt_asink,
                                       avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
                                       NULL, NULL, is->agraph);
    if (ret < 0)
        goto end;

    if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts,  AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;
    if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;

    if (force_output_format) {
        av_bprint_clear(&bp);
        av_channel_layout_describe_bprint(&is->audio_tgt.ch_layout, &bp);
        sample_rates   [0] = is->audio_tgt.freq;
        if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set(filt_asink, "ch_layouts", bp.str, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "sample_rates"   , sample_rates   ,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
    }


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

static int audio_thread(void *arg)
{
    VideoState *is = (VideoState*)arg;
    AVFrame *frame = av_frame_alloc();
    Frame *af;
    int last_serial = -1;
    int reconfigure;
    int got_frame = 0;
    AVRational tb;
    int ret = 0;

    if (!frame)
        return AVERROR(ENOMEM);

    do {
        if ((got_frame = decoder_decode_frame(&is->auddec, frame, NULL)) < 0)
            goto the_end;

        if (got_frame) {
            tb = (AVRational){1, frame->sample_rate};

            reconfigure =
                cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.ch_layout.nb_channels,
                               AVSampleFormat(frame->format), frame->ch_layout.nb_channels)    ||
                av_channel_layout_compare(&is->audio_filter_src.ch_layout, &frame->ch_layout) ||
                is->audio_filter_src.freq           != frame->sample_rate ||
                is->auddec.pkt_serial               != last_serial;

            if (reconfigure) {
                char buf1[1024], buf2[1024];
                av_channel_layout_describe(&is->audio_filter_src.ch_layout, buf1, sizeof(buf1));
                av_channel_layout_describe(&frame->ch_layout, buf2, sizeof(buf2));
                /*av_log(NULL, AV_LOG_DEBUG,
                       "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                       is->audio_filter_src.freq, is->audio_filter_src.ch_layout.nb_channels, av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
                       frame->sample_rate, frame->ch_layout.nb_channels, av_get_sample_fmt_name(frame->format), buf2, is->auddec.pkt_serial);*/

                is->audio_filter_src.fmt            = AVSampleFormat(frame->format);
                ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &frame->ch_layout);
                if (ret < 0)
                    goto the_end;
                is->audio_filter_src.freq           = frame->sample_rate;
                last_serial                         = is->auddec.pkt_serial;

                if ((ret = configure_audio_filters(is, nullptr, 1)) < 0)
                    goto the_end;
            }

            if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
                goto the_end;

            while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
                FrameData *fd = frame->opaque_ref ? (FrameData*)frame->opaque_ref->data : NULL;
                tb = av_buffersink_get_time_base(is->out_audio_filter);
                if (!(af = frame_queue_peek_writable(&is->sampq)))
                    goto the_end;

                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                af->pos = fd ? fd->pkt_pos : -1;
                af->serial = is->auddec.pkt_serial;
                af->duration = av_q2d((AVRational){frame->nb_samples, frame->sample_rate});

                av_frame_move_ref(af->frame, frame);
                frame_queue_push(&is->sampq);

                if (is->audioq.serial != is->auddec.pkt_serial)
                    break;
            }
            if (ret == AVERROR_EOF)
                is->auddec.finished = is->auddec.pkt_serial;
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
the_end:
    avfilter_graph_free(&is->agraph);
    av_frame_free(&frame);
    return ret;
}

static int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void* arg)
{
    d->queue->start();
    d->decoder_tid = std::thread(fn, arg);
    return 0;
}

static int video_thread(void *arg)
{
    VideoState *is = (VideoState*)arg;
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

    AVFilterGraph *graph = NULL;
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    auto last_format = AVPixelFormat(-2); //So that AV_PIX_FMT_UNKNOWN doesn't cause any issues
    int last_serial = -1;

    if (!frame)
        return AVERROR(ENOMEM);

    for (;;) {
        ret = decoder_decode_frame(&is->viddec, frame, NULL);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;

        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        if (   last_w != frame->width
            || last_h != frame->height
            || last_format != frame->format
            || last_serial != is->viddec.pkt_serial) {
           /* av_log(NULL, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
                   frame->width, frame->height,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(frame->format), "none"), is->viddec.pkt_serial);*/
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if (!graph) {
                ret = AVERROR(ENOMEM);
                goto the_end;
            }
            graph->nb_threads = 0;
            if ((ret = configure_video_filters(graph, is, NULL, frame)) < 0) {
                qDebug() << "Failed to configure video filters, aborting playback...";
                goto the_end;
            }
            filt_in  = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = AVPixelFormat(frame->format);
            last_serial = is->viddec.pkt_serial;
            frame_rate = av_buffersink_get_frame_rate(filt_out);
        }

        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;

        while (ret >= 0) {
            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF)
                    is->viddec.finished = is->viddec.pkt_serial;
                ret = 0;
                break;
            }

            const auto fd = frame->opaque_ref ? (FrameData*)frame->opaque_ref->data : NULL;
            tb = av_buffersink_get_time_base(filt_out);
            duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            ret = queue_picture(is, frame, pts, duration, fd ? fd->pkt_pos : -1, is->viddec.pkt_serial);
            av_frame_unref(frame);
            if (is->videoq.serial != is->viddec.pkt_serial)
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

static int subtitle_thread(void *arg)
{
    VideoState *is = (VideoState*)arg;
    Frame *sp;

    for (;;) {
        if (!(sp = frame_queue_peek_writable(&is->subpq)))
            return 0;

        int got_subtitle;
        if ((got_subtitle = decoder_decode_frame(&is->subdec, NULL, &sp->sub)) < 0)
            break;

        double pts = 0;

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

/**
 * Decode one audio frame and return its uncompressed size.
 *
 * The processed audio frame is decoded, converted if required, and
 * stored in is->audio_buf, with size in bytes given by the return
 * value.
 */
static std::vector<float> audio_decode_frame(VideoState *is, double& audio_clock, int& audio_clock_serial)
{
    Frame *af;
    std::vector<float> adata;

    do {
        if (frame_queue_nb_remaining(&is->sampq) == 0) {
            return adata;
        }
        if (!(af = frame_queue_peek_readable(&is->sampq)))
            return adata;
        frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial);

    if (af->frame->format        != is->audio_src.fmt            ||
        av_channel_layout_compare(&af->frame->ch_layout, &is->audio_src.ch_layout) ||
        af->frame->sample_rate   != is->audio_src.freq) {
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
            return adata;
        }
        if (av_channel_layout_copy(&is->audio_src.ch_layout, &af->frame->ch_layout) < 0)
            return adata;
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = AVSampleFormat(af->frame->format);
    }

    if (is->swr_ctx) {
        const uint8_t **in = (const uint8_t **)af->frame->extended_data;
        const int out_count = (int64_t)af->frame->nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
        if (out_count < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return adata;
        }

        adata.resize(out_count * is->audio_tgt.ch_layout.nb_channels);
        auto out = reinterpret_cast<uint8_t*>(adata.data());

        const int len2 = swr_convert(is->swr_ctx, &out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return std::vector<float>();
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(is->swr_ctx) < 0)
                swr_free(&is->swr_ctx);
        }
        adata.resize(len2 * is->audio_tgt.ch_layout.nb_channels);
    } else {
        const auto data_ptr = reinterpret_cast<float*>(af->frame->data[0]);
        adata = std::vector<float>(data_ptr, data_ptr + af->frame->nb_samples * af->frame->ch_layout.nb_channels);
    }

    if (!isnan(af->pts))
        audio_clock = af->pts + (double) af->frame->nb_samples / af->frame->sample_rate;
    else
        audio_clock = NAN;
    audio_clock_serial = af->serial;

    return adata;
}

static SDL_AudioStream* audio_open(AVChannelLayout& wanted_channel_layout, int wanted_sample_rate, AudioParams& audio_hw_params)
{
    SDL_AudioSpec wanted_spec{};
    int wanted_nb_channels = wanted_channel_layout.nb_channels;
    SDL_AudioStream* astream = nullptr;

    if (wanted_channel_layout.order != AV_CHANNEL_ORDER_NATIVE) { //Handle streams with non-standard channel layout
        av_channel_layout_uninit(&wanted_channel_layout);
        av_channel_layout_default(&wanted_channel_layout, wanted_nb_channels);
    }

    wanted_nb_channels = wanted_channel_layout.nb_channels;
    if (wanted_sample_rate <= 0 || wanted_nb_channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return nullptr;
    }

    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    wanted_spec.format = SDL_AUDIO_F32;

    if(!(astream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &wanted_spec, nullptr, nullptr))){
        av_log(NULL, AV_LOG_ERROR, "Audio open failed\n");
        return nullptr;
    }

    audio_hw_params.fmt = AV_SAMPLE_FMT_FLT;
    audio_hw_params.freq = wanted_sample_rate;
    if (av_channel_layout_copy(&audio_hw_params.ch_layout, &wanted_channel_layout) < 0)
        return nullptr;

    return astream;
}

void audio_render_thread(VideoState* ctx){
    static constexpr auto audiobuf_preferred_duration = 0.1;//in seconds
    static constexpr auto timeout = std::chrono::milliseconds(10);
    SDL_AudioStream* astream = ctx->sdl_astream;
    double audio_clock = 0.0;
    int audio_clock_serial = -1;
    bool local_paused = false, last_paused = false;

    auto wait_timeout = []{std::this_thread::sleep_for(timeout);};

    auto get_buffered_duration = [ctx](SDL_AudioStream* stream){
        const auto bytes_queued = SDL_GetAudioStreamQueued(stream);
        return ((double)bytes_queued/(ctx->audio_tgt.ch_layout.nb_channels * sizeof(float)))/(double)ctx->audio_tgt.freq;
    };

    const auto audio_dev = SDL_GetAudioStreamDevice(astream);
    SDL_ResumeAudioDevice(audio_dev);

    while(!quitRequested() && !ctx->audioq.abort_request){
        const bool global_paused = ctx->pause_request.load();
        if(global_paused != last_paused){
            last_paused = local_paused = global_paused;
            const bool success = local_paused ? SDL_PauseAudioDevice(audio_dev) : SDL_ResumeAudioDevice(audio_dev);
            ctx->audclk.set_paused(local_paused);
        }

        if(global_paused){
            wait_timeout();
            continue;
        }

        const auto buffered_duration = get_buffered_duration(astream);
        if(buffered_duration < audiobuf_preferred_duration){
            const auto adata = audio_decode_frame(ctx, audio_clock, audio_clock_serial);
            if(adata.size() > 0){
                SDL_PutAudioStreamData(astream, adata.data(), adata.size() * sizeof(float));
                if (!isnan(audio_clock)) {
                    ctx->audclk.set(audio_clock - get_buffered_duration(astream), audio_clock_serial);
                }
            } else {
                wait_timeout();
            }
        } else {
            wait_timeout();
        }
    }

    if(astream){
        SDL_FlushAudioStream(astream);//Also do this on EOF
    }
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    const AVCodec *codec;
    AVDictionary *opts = NULL;
    int sample_rate;
    int ret = 0;
    int stream_lowres = 0;
    AVChannelLayout ch_layout{};

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;

    avctx = avcodec_alloc_context3(NULL);
    if (!avctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0)
        goto fail;
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;

    codec = avcodec_find_decoder(avctx->codec_id);
    if (!codec) {
        av_log(NULL, AV_LOG_WARNING,
               "No decoder could be found for codec %s\n", avcodec_get_name(avctx->codec_id));
        ret = AVERROR(EINVAL);
        goto fail;
    }

    switch(avctx->codec_type){
    case AVMEDIA_TYPE_AUDIO   : is->last_audio_stream    = stream_index; break;
    case AVMEDIA_TYPE_SUBTITLE: is->last_subtitle_stream = stream_index; break;
    case AVMEDIA_TYPE_VIDEO   : is->last_video_stream    = stream_index; break;
    default: break;
    }

    avctx->codec_id = codec->id;
    if (stream_lowres > codec->max_lowres) {
        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
               codec->max_lowres);
        stream_lowres = codec->max_lowres;
    }
    avctx->lowres = stream_lowres;

    if (0)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;

    if (!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (stream_lowres)
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);

    av_dict_set(&opts, "flags", "+copy_opaque", AV_DICT_MULTIKEY);

    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }

    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
    {
        sample_rate = is->audio_filter_src.freq = avctx->sample_rate;
        ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &avctx->ch_layout);
        if (ret < 0)
            goto fail;
        ret = av_channel_layout_copy(&ch_layout, &avctx->ch_layout);
        if (ret < 0)
            goto fail;
        is->audio_filter_src.fmt = avctx->sample_fmt;
        auto astream = audio_open(ch_layout, sample_rate, is->audio_tgt);
        if(!astream) goto fail;
        is->audio_src = is->audio_tgt;
        is->sdl_astream = astream;
    }

        is->audio_stream = stream_index;
        is->audio_st = ic->streams[stream_index];

        if ((ret = decoder_init(&is->auddec, avctx, &is->audioq, &is->continue_read_thread)) < 0)
            goto fail;
        if (is->ic->iformat->flags & AVFMT_NOTIMESTAMPS) {
            is->auddec.start_pts = is->audio_st->start_time;
            is->auddec.start_pts_tb = is->audio_st->time_base;
        }
        if ((ret = decoder_start(&is->auddec, audio_thread, "audio_decoder", is)) < 0)
            goto out;
        is->audio_render_thr = std::thread(audio_render_thread, is);
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st = ic->streams[stream_index];

        if ((ret = decoder_init(&is->viddec, avctx, &is->videoq, &is->continue_read_thread)) < 0)
            goto fail;
        if ((ret = decoder_start(&is->viddec, video_thread, "video_decoder", is)) < 0)
            goto out;
        is->queue_attachments_req = 1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_stream = stream_index;
        is->subtitle_st = ic->streams[stream_index];

        if ((ret = decoder_init(&is->subdec, avctx, &is->subtitleq, &is->continue_read_thread)) < 0)
            goto fail;
        if ((ret = decoder_start(&is->subdec, subtitle_thread, "subtitle_decoder", is)) < 0)
            goto out;
        break;
    default:
        break;
    }
    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_channel_layout_uninit(&ch_layout);
    av_dict_free(&opts);

    return ret;
}

static int decode_interrupt_cb(void *ctx)
{
    return quitRequested();
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue) {
    return stream_id < 0 ||
           queue->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
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
static int read_thread(void *arg)
{
    VideoState *is = (VideoState*)arg;
    AVFormatContext *ic = NULL;
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket *pkt = NULL;
    const AVDictionaryEntry *t;
    std::mutex wait_mutex;
    int scan_all_pmts_set = 0;
    AVDictionary* format_opts = nullptr;
    bool realtime = false, last_paused = false, local_paused = false;

    memset(st_index, -1, sizeof(st_index));
    is->eof = 0;

    pkt = av_packet_alloc();
    if (!pkt) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate packet.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;
    if (!av_dict_get(format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }
    err = avformat_open_input(&ic, is->url.c_str(), nullptr, &format_opts);
    if (err < 0) {
        //print_error(is->filename, err);
        ret = -1;
        goto fail;
    }
    if (scan_all_pmts_set)
        av_dict_set(&format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);

    is->ic = ic;

    ic->flags |= AVFMT_FLAG_GENPTS;

    err = avformat_find_stream_info(ic, nullptr);

    if (err < 0) {
        av_log(NULL, AV_LOG_WARNING,
               "Could not find codec parameters\n");
        ret = -1;
        goto fail;
    }

    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

    if (seek_by_bytes < 0)
        seek_by_bytes = !(ic->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
                        !!(ic->iformat->flags & AVFMT_TS_DISCONT) &&
                        strcmp("ogg", ic->iformat->name);

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    /*if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
        window_title = av_asprintf("%s - %s", t->value, input_filename);*/

    realtime = is_realtime(ic);

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
        stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
    }

    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
    }

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
               is->url.c_str());
        ret = -1;
        goto fail;
    }

    while (!quitRequested()) {
        const bool pause_requested = is->pause_request.load();
        if (pause_requested != last_paused) {
            local_paused = last_paused = pause_requested;
            if (local_paused)
                av_read_pause(ic);
            else
                av_read_play(ic);
        }

        if (local_paused &&
            (!strcmp(ic->iformat->name, "rtsp") ||
             (ic->pb && !strncmp(is->url.c_str(), "mmsh:", 5)))) {
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

            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR,
                       "%s: error while seeking\n", is->ic->url);
            } else {
                if (is->audio_stream >= 0)
                    is->audioq.flush();
                if (is->subtitle_stream >= 0)
                    is->subtitleq.flush();
                if (is->video_stream >= 0)
                    is->videoq.flush();
            }
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;
        }
        if (is->queue_attachments_req) {
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                if ((ret = av_packet_ref(pkt, &is->video_st->attached_pic)) < 0)
                    goto fail;
                is->videoq.put(pkt);
                is->videoq.put_nullpacket(is->video_stream);
            }
            is->queue_attachments_req = 0;
        }

        /* if the queue are full, no need to read more */
        if (!realtime &&
            (is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE
             || (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
                 stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
                 stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq)))) {
            /* wait 10 ms */
            std::unique_lock lck(wait_mutex);
            is->continue_read_thread.wait_for(lck, std::chrono::milliseconds(10));
            continue;
        }
        if (!local_paused &&
            (!is->audio_st || (is->auddec.finished == is->audioq.serial && frame_queue_nb_remaining(&is->sampq) == 0)) &&
            (!is->video_st || (is->viddec.finished == is->videoq.serial && frame_queue_nb_remaining(&is->pictq) == 0))) {
            if (loop != 1 && (!loop || --loop)) {
                stream_seek(is, 0, 0, 0);
            } else if (autoexit) {
                ret = AVERROR_EOF;
                goto fail;
            }
        }
        ret = av_read_frame(ic, pkt);
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
                if (autoexit)
                    goto fail;
                else
                    break;
            }
            std::unique_lock lck(wait_mutex);
            is->continue_read_thread.wait_for(lck, std::chrono::milliseconds(10));
            continue;
        } else {
            is->eof = 0;
        }

        if (pkt->stream_index == is->audio_stream) {
            is->audioq.put(pkt);
        } else if (pkt->stream_index == is->video_stream
                   && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            is->videoq.put(pkt);
        } else if (pkt->stream_index == is->subtitle_stream) {
            is->subtitleq.put(pkt);
        } else {
            av_packet_unref(pkt);
        }
    }

    ret = 0;
fail:
    if (ic && !is->ic)
        avformat_close_input(&ic);

    av_packet_free(&pkt);

    return 0;
}

static VideoState *stream_open(std::string filename)
{
    auto is = new VideoState;
    is->url = filename;

    if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
        goto fail;
    if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
        goto fail;
    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;

    is->vidclk.init(&is->videoq.serial);
    is->audclk.init(&is->audioq.serial);

    is->read_thr = std::thread(read_thread, is);

    return is;

fail:
    stream_close(is);
    return NULL;
}

static void stream_cycle_channel(VideoState *is, AVMediaType codec_type)
{
    AVFormatContext *ic = is->ic;
    int start_index, stream_index;
    int old_index;
    AVStream *st;
    AVProgram *p = NULL;
    int nb_streams = is->ic->nb_streams;

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
        st = is->ic->streams[p ? p->stream_index[stream_index] : stream_index];
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
           av_get_media_type_string(AVMediaType(codec_type)),
           old_index,
           stream_index);

    stream_component_close(is, old_index);
    stream_component_open(is, stream_index);
}

static void seek_chapter(VideoState *is, int incr)
{
    int64_t pos = get_master_clock(is) * AV_TIME_BASE;
    int i;

    if (!is->ic->nb_chapters)
        return;

    /* find the current chapter */
    for (i = 0; i < is->ic->nb_chapters; i++) {
        AVChapter *ch = is->ic->chapters[i];
        if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
            i--;
            break;
        }
    }

    i += incr;
    i = FFMAX(i, 0);
    if (i >= is->ic->nb_chapters)
        return;

    av_log(NULL, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
    stream_seek(is, av_rescale_q(is->ic->chapters[i]->start, is->ic->chapters[i]->time_base,
                                 AV_TIME_BASE_Q), 0, 0);
}

static int64_t cursor_last_shown = 0;
static int cursor_hidden = 0;

/* handle an event sent by the GUI */
static void handle_gui_evt(VideoState *cur_stream, bool paused, const SDL_Event& event)
{
    double incr, pos, frac;

    double x;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        // If we don't yet have a window, skip all key events, because read_thread might still be initializing...
        if (!cur_stream->width)
            return;
        switch (event.key.key) {
        case SDLK_F:
            //cur_stream->force_refresh = 1;
            break;
        case SDLK_P:
        case SDLK_SPACE:
            stream_toggle_pause(cur_stream);
            break;
        case SDLK_M:
            //toggle_mute(cur_stream);
            break;
        case SDLK_KP_MULTIPLY:
        case SDLK_0:
            //update_volume(cur_stream, 1, SDL_VOLUME_STEP);
            break;
        case SDLK_KP_DIVIDE:
        case SDLK_9:
            //update_volume(cur_stream, -1, SDL_VOLUME_STEP);
            break;
        /*case SDLK_s: // S: Step to next frame
            step_to_next_frame(cur_stream);
            break;
        case SDLK_a:
            stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
            break;
        case SDLK_v:
            stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
            break;
        case SDLK_c:
            stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
            stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
            stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
            break;
        case SDLK_t:
            stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
            break;
        case SDLK_w:
            toggle_audio_display(cur_stream);
            break;*/
        case SDLK_PAGEUP:
            if (cur_stream->ic->nb_chapters <= 1) {
                incr = 600.0;
                goto do_seek;
            }
            seek_chapter(cur_stream, 1);
            break;
        case SDLK_PAGEDOWN:
            if (cur_stream->ic->nb_chapters <= 1) {
                incr = -600.0;
                goto do_seek;
            }
            seek_chapter(cur_stream, -1);
            break;
        case SDLK_LEFT:
            incr = seek_interval ? -seek_interval : -10.0;
            goto do_seek;
        case SDLK_RIGHT:
            incr = seek_interval ? seek_interval : 10.0;
            goto do_seek;
        case SDLK_UP:
            incr = 60.0;
            goto do_seek;
        case SDLK_DOWN:
            incr = -60.0;
        do_seek:
            if (seek_by_bytes) {
                pos = -1;
                if (pos < 0 && cur_stream->video_stream >= 0)
                    pos = frame_queue_last_pos(&cur_stream->pictq);
                if (pos < 0 && cur_stream->audio_stream >= 0)
                    pos = frame_queue_last_pos(&cur_stream->sampq);
                if (pos < 0)
                    pos = avio_tell(cur_stream->ic->pb);
                if (cur_stream->ic->bit_rate)
                    incr *= cur_stream->ic->bit_rate / 8.0;
                else
                    incr *= 180000.0;
                pos += incr;
                stream_seek(cur_stream, pos, incr, 1);
            } else {
                pos = get_master_clock(cur_stream);
                if (isnan(pos))
                    pos = (double)cur_stream->seek_pos / AV_TIME_BASE;
                pos += incr;
                if (cur_stream->ic->start_time != AV_NOPTS_VALUE && pos < cur_stream->ic->start_time / (double)AV_TIME_BASE)
                    pos = cur_stream->ic->start_time / (double)AV_TIME_BASE;
                stream_seek(cur_stream, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), 0);
            }
            break;
        default:
            break;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.button == SDL_BUTTON_LEFT) {
            static int64_t last_mouse_left_click = 0;
            if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                //cur_stream->force_refresh = 1;
                last_mouse_left_click = 0;
            } else {
                last_mouse_left_click = av_gettime_relative();
            }
        }
    case SDL_EVENT_MOUSE_MOTION:
        if (cursor_hidden) {
            SDL_ShowCursor();
            cursor_hidden = 0;
        }
        cursor_last_shown = av_gettime_relative();
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (event.button.button != SDL_BUTTON_RIGHT)
                break;
            x = event.button.x;
        } else {
            if (!(event.motion.state & SDL_BUTTON_RMASK))
                break;
            x = event.motion.x;
        }
        if (seek_by_bytes || cur_stream->ic->duration <= 0) {
            uint64_t size =  avio_size(cur_stream->ic->pb);
            stream_seek(cur_stream, size*x/cur_stream->width, 0, 1);
        } else {
            int64_t ts;
            int ns, hh, mm, ss;
            int tns, thh, tmm, tss;
            tns  = cur_stream->ic->duration / 1000000LL;
            thh  = tns / 3600;
            tmm  = (tns % 3600) / 60;
            tss  = (tns % 60);
            frac = x / cur_stream->width;
            ns   = frac * tns;
            hh   = ns / 3600;
            mm   = (ns % 3600) / 60;
            ss   = (ns % 60);
            av_log(NULL, AV_LOG_INFO,
                   "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n", frac*100,
                   hh, mm, ss, thh, tmm, tss);
            ts = frac * cur_stream->ic->duration;
            if (cur_stream->ic->start_time != AV_NOPTS_VALUE)
                ts += cur_stream->ic->start_time;
            stream_seek(cur_stream, ts, 0, 0);
        }
        break;
    case SDL_EVENT_WINDOW_RESIZED:
        screen_width  = cur_stream->width  = event.window.data1;
        screen_height = cur_stream->height = event.window.data2;
        qDebug() << "Window has been resized to " << screen_width <<"x" <<screen_height;
    case SDL_EVENT_WINDOW_EXPOSED:
        //cur_stream->force_refresh = 1;
        break;
    default:
        break;
    }
}

VideoState* player_inst = nullptr;
std::thread render_thr;

void playback_init_cleanup(VideoState *is, SDL_Window* window)
{
    if (is) {
        stream_close(is);
    }
    if (renderer)
        SDL_DestroyRenderer(renderer);

    if (window)
        SDL_DestroyWindow(window);

    SDL_Quit();
};

bool init_libraries(SDL_Window*& window)
{    
    av_log_set_flags(AV_LOG_SKIP_REPEATED);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        playback_init_cleanup(nullptr, window);
        return false;
    }

    if (!SDL_CreateWindowAndRenderer("ffplay", 0, 0, SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS, &window, &renderer)) {
        av_log(NULL, AV_LOG_FATAL, "Failed to create window and/or renderer: %s", SDL_GetError());
        playback_init_cleanup(nullptr, window);
        return false;
    }

    if (renderer) {
        const auto props = SDL_GetRendererProperties(renderer);
        auto tex_fmts = (const SDL_PixelFormat*)SDL_GetPointerProperty(props, SDL_PROP_RENDERER_TEXTURE_FORMATS_POINTER, nullptr);
        if(!tex_fmts || tex_fmts[0] == SDL_PIXELFORMAT_UNKNOWN){
            av_log(NULL, AV_LOG_FATAL, "Failed to create renderer: %s", SDL_GetError());
            playback_init_cleanup(nullptr, window);
            return false;
        } else{
            auto tex_fmt = tex_fmts[0];
            int i = 0;
            while(tex_fmt != SDL_PIXELFORMAT_UNKNOWN){
                supported_pix_fmts.push_back(tex_fmt);
                ++i;
                tex_fmt = tex_fmts[i];
            }
        }
    } else {
        av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
        playback_init_cleanup(nullptr, window);
        return false;
    }

    return true;
}

uintptr_t getSDLWindowHandle(SDL_Window* sdl_wnd){
    uintptr_t retrieved_handle = 0U;

#if defined(SDL_PLATFORM_WIN32)
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (hwnd) {
        retrieved_handle = hwnd;
    }
#elif defined(SDL_PLATFORM_MACOS)
    NSWindow *nswindow = (__bridge NSWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    if (nswindow) {
        retrieved_handle = nswindow;
    }
#elif defined(SDL_PLATFORM_LINUX)
    if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0) {
        auto xwindow = SDL_GetNumberProperty(SDL_GetWindowProperties(sdl_wnd), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        retrieved_handle = (uintptr_t)xwindow;
    } else if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
        struct wl_display *display = (struct wl_display *)SDL_GetPointerProperty(SDL_GetWindowProperties(sdl_wnd), SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
        struct wl_surface *surface = (struct wl_surface *)SDL_GetPointerProperty(SDL_GetWindowProperties(sdl_wnd), SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
        if (display && surface) {
            retrieved_handle = (uintptr_t)surface;
        }
    }
#elif defined(SDL_PLATFORM_IOS)
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    UIWindow *uiwindow = (__bridge UIWindow *)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
    if (uiwindow) {
        retrieved_handle = uiwindow;
    }
#endif

    return retrieved_handle;
}

void renderer_thread(std::string url){
    SDL_Window *window = nullptr;
    if(!init_libraries(window)) return;
    SDL_SyncWindow(window);

    const auto wnd_handle = getSDLWindowHandle(window);
    embedSDLWindow(wnd_handle);

    player_inst = stream_open(url);
    if (!player_inst) {
        av_log(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
        playback_init_cleanup(nullptr, window);
        return;
    }

    auto ctx = player_inst;
    bool local_paused = false;
    double remaining_time = 0.0;
    bool force_refresh = false;

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
    static constexpr auto REFRESH_RATE = 0.005;

    while (!quitRequested()) {
        const auto pause_requested = ctx->pause_request.load();
        if(local_paused != pause_requested){
            local_paused = pause_requested;
            if(!local_paused){
                ctx->frame_timer += gettime_s() - ctx->vidclk.last_upd();
                ctx->vidclk.set(ctx->vidclk.get(), ctx->vidclk.serial());
            }
            ctx->vidclk.set_paused(local_paused);
            force_refresh = true;
        }

        SDL_Event event{};
        SDL_PumpEvents();
        if (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 0) {            handle_gui_evt(ctx, local_paused, event);
            if (!cursor_hidden && av_gettime_relative() - cursor_last_shown >= CURSOR_HIDE_DELAY) {
                SDL_HideCursor();
                cursor_hidden = 1;
            }
            if (remaining_time > 0.0015)
                std::this_thread::sleep_for(std::chrono::milliseconds(int(remaining_time * 1000)));
            remaining_time = REFRESH_RATE;
            if (!local_paused || force_refresh)
                video_refresh(ctx, &remaining_time, local_paused, force_refresh);
        } else{
            handle_gui_evt(ctx, local_paused, event);
        }
    }

    SDL_HideWindow(window);
    SDL_SyncWindow(window);
    disembedSDLWindow();
    playback_init_cleanup(ctx, window);
    signalPostQuitCleanup();
}

bool isPlaybackActive(){
    return player_inst && !quitRequested();
}

bool open_url(std::string url){
    if(player_inst || quitRequested()) return false;
    render_thr = std::thread(renderer_thread, url);
    return true;
}

void stop_playback(){
    if(!player_inst || quitRequested()) return;
    setQuitRequest(true);
}

void pause(){
    if(quitRequested() || !player_inst || player_inst->pause_request.load()) return;
    stream_toggle_pause(player_inst);
}

void resume(){
    if(quitRequested() || !player_inst || !player_inst->pause_request.load()) return;
    stream_toggle_pause(player_inst);
}

//Fire off a cleanup event when quitting the render thread
void postquit_cleanup(){
    if(render_thr.joinable()){
        render_thr.join();
    }

    player_inst = nullptr;
    setQuitRequest(false);
}
