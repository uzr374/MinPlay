#include "playbackengine.hpp"
#include "audioresampler.hpp"
#include "formatcontext.hpp"
#include "audiooutput.hpp"
#include "avframeview.hpp"
#include "audiotrack.hpp"
#include "videotrack.hpp"
#include "subtrack.hpp"

#include <QApplication>
#include <cstdarg>

extern "C"{
#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/dict.h"
#include "libavutil/time.h"
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#include "libavutil/display.h"
}

#include <SDL3/SDL.h>

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01
#define SDL_AUDIO_BUFLEN 0.2 /*in seconds*/

struct VideoState {
    SDLRenderer* sdl_renderer = nullptr;
    AudioOutput aout;
    AudioResampler acvt;

    std::mutex render_mutex; /*guards each iteration of the refresh loop*/
    double stream_duration = 0.0;

    std::mutex seek_mutex;
    SeekInfo seek_info;
    bool seek_req = false;

    std::mutex streams_mutex;
    bool streams_updated = false;
    std::vector<CAVStream> streams;

    std::thread read_thr;
    const AVInputFormat *iformat = nullptr;
    bool abort_request = false;
    bool force_refresh = false;
    bool paused = false;
    bool last_paused = false;
    bool queue_attachments_req = false;
    bool realtime = false;

    std::unique_ptr<AudioTrack> atrack;
    std::unique_ptr<VideoTrack> vtrack;
    std::unique_ptr<SubTrack> strack;

    int audio_stream = -1;

    std::vector<uint8_t> audio_buf;
    bool muted = false;

    int frame_drops_late = 0;

    int subtitle_stream = -1;

    double frame_timer = 0.0;

    int video_stream = -1;

    double max_frame_duration = 0.0;      // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
    SwsContext *sub_convert_ctx = nullptr;
    bool eof = false;

    char *filename = nullptr;
    bool step = false;

    int last_video_stream = -1, last_audio_stream = -1, last_subtitle_stream = -1;

    std::condition_variable continue_read_thread;
};

static void video_image_display(VideoState *is)
{
    CSubtitle *sp = NULL;

    auto& vp = is->vtrack->getLastPicture();

    if (is->strack) {
        /*if (frame_queue_nb_remaining(&is->subpq) > 0) {
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
        }*/
    }

    if (!vp.isUploaded()) {
        is->sdl_renderer->updateVideoTexture(AVFrameView(*vp.constAv()));
        is->sdl_renderer->refreshDisplay();
        vp.setUploaded(true);
    }
}

static void request_ao_change(VideoState* ctx, int new_freq, int new_chn){
    ctx->aout.requestChange(new_freq, new_chn);
}

static void ao_close(VideoState* ctx){
    request_ao_change(ctx, 0, 0);
}

static void stream_component_close(VideoState *is, int stream_index, AVFormatContext* ic)
{
    if (!ic || stream_index < 0 || stream_index >= ic->nb_streams)
        return;

    switch (ic->streams[stream_index]->codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->atrack = nullptr;
        is->audio_stream = -1;
        ao_close(is);
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->vtrack = nullptr;
        is->video_stream = -1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->strack = nullptr;
        is->subtitle_stream = -1;
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
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

    sws_freeContext(is->sub_convert_ctx);
    av_free(is->filename);

    delete is;
}

/* get the current master clock value */
static double get_master_clock(VideoState *is)
{
    double clock = NAN;
    if(is->atrack){
        clock = is->atrack->getClockVal();
    } else if(is->vtrack){
        clock = is->vtrack->getClockVal();
    }
    return clock;
}

/* pause or resume the video */
static void stream_toggle_pause(VideoState *is)
{
    std::scoped_lock lck(is->render_mutex);
    if (is->paused) {
        if(is->vtrack){
            is->frame_timer += av_gettime_relative() / 1000000.0 - is->vtrack->clockUpdateTime();
        }
    }

    is->paused = !is->paused;
    if(is->vtrack){
        is->vtrack->setPauseStatus(is->paused);
    }
    if(is->atrack){
        is->atrack->setPauseStatus(is->paused);
    }
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
    if (is->atrack) {
        /* if video is slave, we try to correct big delays by
           duplicating or deleting a frame */
        diff = is->vtrack->getClockVal() - is->atrack->getClockVal();

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

static double vp_duration(VideoState *is, const CAVFrame& vp, const CAVFrame& nextvp) {
    if (vp.serial() == nextvp.serial()) {
        const double duration = nextvp.ts() - vp.ts();
        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
            return vp.dur();
        else
            return duration;
    } else {
        return 0.0;
    }
}

/* called to display each frame */
static void video_refresh(VideoState *is, double *remaining_time)
{
    double time;

    CSubtitle *sp = nullptr, *sp2 = nullptr;

    if (is->vtrack) {
    retry:
        if (is->vtrack->framesAvailable() == 0) {
            // nothing to do, no picture to display in the queue
        } else {
            double last_duration, duration, delay;
            /* dequeue the picture */
            const auto& lastvp = is->vtrack->getLastPicture();
            const auto& vp = is->vtrack->peekCurrentPicture();

            if (vp.serial() != is->vtrack->serial()) {
                is->vtrack->nextFrame();
                goto retry;
            }

            if (lastvp.serial() != vp.serial())
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

            if (!isnan(vp.ts()))
                is->vtrack->updateClock(vp.ts());

            if (is->vtrack->framesAvailable() > 1) {
                const auto& nextvp = is->vtrack->peekNextPicture();
                duration = vp_duration(is, vp, nextvp);
                if(!is->step && (is->atrack) && time > is->frame_timer + duration){
                    is->frame_drops_late++;
                    is->vtrack->nextFrame();
                    goto retry;
                }
            }

            if (is->strack) {
                /*while (frame_queue_nb_remaining(&is->subpq) > 0) {
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
                }*/
            }

            is->vtrack->nextFrame();
            is->force_refresh = 1;

            if (is->step && !is->paused)
                stream_toggle_pause(is);
        }
    display:
        /* display picture */
        if (is->force_refresh && is->vtrack->canDisplay())
            video_image_display(is);
    }
    is->force_refresh = 0;
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(VideoState *is, int stream_index, AVFormatContext* ic)
{
    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;

    const CAVStream st(ic, stream_index);
    const auto& codecpar = st.codecPar();

    is->eof = false;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (codecpar.codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->last_audio_stream    = stream_index;
        is->atrack = std::make_unique<AudioTrack>(st, is->continue_read_thread);
        is->audio_stream = stream_index;
        request_ao_change(is, codecpar.sample_rate, codecpar.ch_layout.nb_channels);
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->last_video_stream    = stream_index;
        is->vtrack = std::make_unique<VideoTrack>(st, is->continue_read_thread, is->sdl_renderer->supportedFormats());
        is->video_stream = stream_index;
        is->queue_attachments_req = true;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->last_subtitle_stream = stream_index;
        is->strack = std::make_unique<SubTrack>(st, is->continue_read_thread);
        is->subtitle_stream = stream_index;
        break;
    default:
        break;
    }

    return 0;
}

static int decode_interrupt_cb(void *ctx)
{
    VideoState *is = (VideoState*)ctx;
    return is->abort_request;
}

static bool demux_buffer_is_full(VideoState& ctx){
    bool aq_full = false, vq_full = false;
    int byte_size = 0;
    if(ctx.atrack){
        const auto [size, nb_pkts, dur] = ctx.atrack->getQueueParams();
        byte_size += size;
        aq_full = nb_pkts > MIN_FRAMES && (dur == 0.0 || dur > 1.0);
    } else{aq_full = true;}

    if(ctx.vtrack && !ctx.vtrack->isAttachedPic()){
        const auto [size, nb_pkts, dur] = ctx.vtrack->getQueueParams();
        byte_size += size;
        vq_full = nb_pkts > MIN_FRAMES && (dur == 0.0 || dur > 1.0);
    } else{vq_full = true;}

    return byte_size > MAX_QUEUE_SIZE || (aq_full && vq_full);
}

static bool is_realtime(AVFormatContext *s)
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
    int seek_flags = 0;
    int64_t seek_pos = 0;
    int64_t seek_rel = 0;

    auto set_seek = [&seek_flags, &seek_pos, &seek_rel](int64_t pos, int64_t rel, bool by_bytes){
        if(by_bytes)
            seek_flags = AVSEEK_FLAG_BYTE;
        seek_rel = rel;
        seek_pos = pos;
    };

    auto seek_incr = [&](double incr){
        if (seek_by_bytes) {
            int64_t pos = -1;
            if (pos < 0 && is->vtrack)
                pos = is->vtrack->lastPos();
            if (pos < 0 && is->audio_stream >= 0)
                pos = is->atrack->lastPos();
            if (pos < 0)
                pos = avio_tell(ic->pb);
            if (ic->bit_rate)
                incr *= ic->bit_rate / 8.0;
            else
                incr *= 180000.0;
            pos += incr;
            set_seek(pos, incr, true);
        } else {
            auto pos = get_master_clock(is);
            if (isnan(pos))
                pos = (double)seek_pos / AV_TIME_BASE;
            pos += incr;
            if (ic->start_time != AV_NOPTS_VALUE && pos < ic->start_time / (double)AV_TIME_BASE)
                pos = ic->start_time / (double)AV_TIME_BASE;
            set_seek((int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), false);
        }
    };

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

    err = avformat_find_stream_info(ic, nullptr);
    if (err < 0) {
        av_log(NULL, AV_LOG_WARNING,
               "%s: could not find codec parameters\n", is->filename);
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

    is->streams_mutex.lock();
    is->streams.reserve(ic->nb_streams);
    for (i = 0; i < ic->nb_streams; i++) {
        ic->streams[i]->discard = AVDISCARD_ALL;
        is->streams.push_back(CAVStream(ic, i));
    }
    is->streams_updated = true;
    is->streams_mutex.unlock();

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

    {
        std::scoped_lock lck(is->render_mutex);
        is->stream_duration = ic->duration == AV_NOPTS_VALUE ? 0.0 : ic->duration / (double)AV_TIME_BASE;
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
    }

    if (!is->atrack && !is->vtrack) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s'\n",
               is->filename);
        ret = -1;
        goto fail;
    }

    for (;;) {
        if (is->abort_request)
            break;
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                av_read_pause(ic);
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

        {
            std::scoped_lock seek_lock(is->seek_mutex);
        if (is->seek_req) {
            is->seek_req = false;
                std::scoped_lock rlck(is->render_mutex);
                bool seek_in_stream = false;
                const bool unseekable = (ic->flags & AVFMTCTX_UNSEEKABLE);
                const auto info = is->seek_info;
                switch(info.type){
                case SeekInfo::SEEK_PERCENT:
                {
                    const auto pcent = info.percent;
                    if (seek_by_bytes || ic->duration <= 0) {
                        const uint64_t size =  avio_size(ic->pb);
                        set_seek(size*pcent, 0, true);
                    } else {
                        int64_t ts;
                        int ns, hh, mm, ss;
                        int tns, thh, tmm, tss;
                        tns  = ic->duration / 1000000LL;
                        thh  = tns / 3600;
                        tmm  = (tns % 3600) / 60;
                        tss  = (tns % 60);
                        ns   = pcent * tns;
                        hh   = ns / 3600;
                        mm   = (ns % 3600) / 60;
                        ss   = (ns % 60);
                        av_log(NULL, AV_LOG_INFO,
                               "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)", pcent*100,
                               hh, mm, ss, thh, tmm, tss);
                        ts = pcent * ic->duration;
                        if (ic->start_time != AV_NOPTS_VALUE)
                            ts += ic->start_time;
                        set_seek(ts, 0, false);
                    }
                    seek_in_stream = !unseekable;
                }
                    break;
                case SeekInfo::SEEK_INCREMENT:
                {
                    seek_incr(info.increment);
                    seek_in_stream = !unseekable;
                }
                    break;
                case SeekInfo::SEEK_CHAPTER:
                {
                    const auto ch_incr = info.chapter_incr;
                    if(ic->nb_chapters > 1){
                        const int64_t pos = get_master_clock(is) * AV_TIME_BASE;

                        int i = 0;
                        /* find the current chapter */
                        for (i = 0; i < ic->nb_chapters; i++) {
                            const AVChapter *ch = ic->chapters[i];
                            if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
                                i--;
                                break;
                            }
                        }

                        i = std::max(i + ch_incr, 0);
                        if (i < ic->nb_chapters){
                            av_log(NULL, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
                            set_seek(av_rescale_q(ic->chapters[i]->start, ic->chapters[i]->time_base,
                                                         AV_TIME_BASE_Q), 0, false);
                        }
                    } else{
                        seek_incr(ch_incr * 600.0);
                    }
                    seek_in_stream = !unseekable;
                }
                    break;
                case SeekInfo::SEEK_STREAM_SWITCH:
                    break;
                default:
                    break;
                }

                if(seek_in_stream){
                    const int64_t seek_target = seek_pos;
                    const int64_t seek_min    = seek_rel > 0 ? seek_target - seek_rel + 2: INT64_MIN;
                    const int64_t seek_max    = seek_rel < 0 ? seek_target - seek_rel - 2: INT64_MAX;
                    // FIXME the +-2 is due to rounding being not done in the correct direction in generation
                    //      of the seek_pos/seek_rel variables

                    ret = avformat_seek_file(ic, -1, seek_min, seek_target, seek_max, seek_flags);
                    if (ret < 0) {
                        av_log(NULL, AV_LOG_ERROR,
                               "%s: error while seeking\n", ic->url);
                    } else {
                        if (is->atrack)
                            is->atrack->flush();
                        if (is->strack)
                            is->strack->flush();
                        if (is->vtrack)
                            is->vtrack->flush();
                    }

                    is->queue_attachments_req = true;
                    is->eof = 0;
                }
        }
        }

        if (is->queue_attachments_req) {
            if (is->vtrack && is->vtrack->isAttachedPic()) {
                CAVPacket pkt;
                if ((ret = av_packet_ref(pkt.av(), &ic->streams[is->video_stream]->attached_pic)) < 0)
                    goto fail;
                pkt.setTb(ic->streams[is->video_stream]->time_base);
                is->vtrack->putPacket(std::move(pkt));
                is->vtrack->putFinalPacket(is->video_stream);
            }
            is->queue_attachments_req = false;
        }

        /* if the queue are full, no need to read more */
        if (!is->realtime && demux_buffer_is_full(*is)) {
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
                if (is->vtrack)
                    is->vtrack->putFinalPacket(is->video_stream);
                if (is->atrack)
                    is->atrack->putFinalPacket(is->audio_stream);
                if (is->strack)
                    is->strack->putFinalPacket(is->subtitle_stream);
                is->eof = true;
            }
            if (ic->pb && ic->pb->error) {
                break;
            }
            std::unique_lock lck(wait_mutex);
            is->continue_read_thread.wait_for(lck, std::chrono::milliseconds(10));
            continue;
        } else {
            is->eof = false;
        }

        const auto pkt_st_index = pkt.streamIndex();
        pkt.setTb(ic->streams[pkt_st_index]->time_base);
        if (is->atrack && pkt_st_index == is->audio_stream) {
            is->atrack->putPacket(std::move(pkt));
        } else if (is->vtrack && pkt_st_index == is->video_stream
                   && !is->vtrack->isAttachedPic()) {
            is->vtrack->putPacket(std::move(pkt));
        } else if (is->strack && pkt_st_index == is->subtitle_stream) {
            is->strack->putPacket(std::move(pkt));
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

static double refresh_audio(VideoState* ctx){
    auto& aout = ctx->aout;
    if(aout.maybeHandleChange()){
        ctx->acvt.setOutputFmt(aout.rate(), [&aout]{CAVChannelLayout lout; lout.make_default(aout.channels()); return lout;}(),
                               AV_SAMPLE_FMT_FLT);
    }

    double remaining_time = REFRESH_RATE;

    if(ctx->atrack && !ctx->paused && aout.isOpen()){
        double buffered_time = aout.getLatency();
        if(buffered_time > SDL_AUDIO_BUFLEN){
            remaining_time = buffered_time / 4;
        } else{
            double decoded_dur = 0.0;
            do{
                const CAVFrame *af = ctx->atrack->getFrame();
                if(!af){
                    break;
                }

                auto& dst = ctx->audio_buf;
                AVFrameView aframe(*af->constAv());
                const auto wanted_nb_samples = aframe.nbSamples();

                if(ctx->acvt.convert(aframe, ctx->audio_buf, wanted_nb_samples, false)){}

                decoded_dur += (double)ctx->audio_buf.size() / aout.bitrate();
                aout.sendData(ctx->audio_buf.data(), ctx->audio_buf.size(), false);
                ctx->audio_buf.clear();
                if(!std::isnan(af->ts())){
                    buffered_time = aout.getLatency();
                    ctx->atrack->updateClock(af->ts() + af->dur() - buffered_time);
                }
            }while(decoded_dur < SDL_AUDIO_BUFLEN);
        }
    }

    return remaining_time;
}

static double refresh_video(VideoState* ctx){
    double remaining_time = REFRESH_RATE;
    if (ctx->vtrack)
        video_refresh(ctx, &remaining_time);
    return remaining_time;
}

static void check_playback_errors(VideoState* ctx){
    if(ctx->vtrack){

    }

    if(ctx->atrack){

    }

    if(ctx->strack){

    }
}

static double playback_loop(VideoState* ctx){
    const auto audio_remaining_time = refresh_audio(ctx);
    const auto video_remaining_time = refresh_video(ctx);
    check_playback_errors(ctx);

    return std::min(audio_remaining_time, video_remaining_time);
}

void PlayerCore::openURL(QUrl url){
    if(player_ctx){
        stopPlayback();
    }

    if((player_ctx = stream_open(url.toString().toStdString().c_str(), video_renderer))){
        refreshPlayback(); //To start the refresh timer
        setControlsActive(true);
    }
}

void PlayerCore::stopPlayback(){
    if(player_ctx){
        stream_close(player_ctx);
        player_ctx = nullptr;
        setControlsActive(false);
    }
}

void PlayerCore::pausePlayback(){

}

void PlayerCore::resumePlayback(){

}
void PlayerCore::togglePause(){
    if(player_ctx){
        std::scoped_lock lck(player_ctx->render_mutex);
        stream_toggle_pause(player_ctx);
    }
}

void PlayerCore::requestSeekPercent(double percent){
    if(player_ctx){
        std::scoped_lock slck(player_ctx->seek_mutex);
        player_ctx->seek_info = {.type = SeekInfo::SEEK_PERCENT, .percent = percent};
        player_ctx->seek_req = true;
    }
}

void PlayerCore::requestSeekIncr(double incr){
    if(player_ctx){
        std::scoped_lock slck(player_ctx->seek_mutex);
        player_ctx->seek_info = {.type = SeekInfo::SEEK_INCREMENT, .increment = incr};
        player_ctx->seek_req = true;
    }
}

void PlayerCore::log(const char* fmt, ...){
    std::va_list args;
    va_start(args, fmt);
    const auto msg = QString::vasprintf(fmt, args);
    va_end(args);
    QMetaObject::invokeMethod(loggerW, &LoggerWidget::logMessage, msg);
}

PlayerCore::PlayerCore(QObject* parent, VideoDisplayWidget* dw, LoggerWidget* lw): QObject(parent), video_dw(dw), loggerW(lw), refresh_timer(this){
    video_renderer = dw->getSDLRenderer();

    connect(&refresh_timer, &QTimer::timeout, this, &PlayerCore::refreshPlayback);
}

PlayerCore::~PlayerCore(){
    stopPlayback();
}

void PlayerCore::refreshPlayback(){
    if(player_ctx){
        std::scoped_lock lck(player_ctx->render_mutex);
        const auto remaining_time = playback_loop(player_ctx) * 1000;
        refresh_timer.setInterval(static_cast<int>(remaining_time));
        if(!refresh_timer.isActive()){
            refresh_timer.setTimerType(Qt::PreciseTimer);
            refresh_timer.start();
        }
        updateGUI();
    } else{
        refresh_timer.stop();
    }
}

void PlayerCore::handleStreamsUpdate(){
    std::scoped_lock slck(player_ctx->streams_mutex);
    if(player_ctx->streams_updated){
        player_ctx->streams_updated = false;
        emit sigUpdateStreams(player_ctx->streams);
    }
}

void PlayerCore::updateGUI(){
    handleStreamsUpdate();
    const auto pos = get_master_clock(player_ctx);
    if(!std::isnan(pos)){
        emit updatePlaybackPos(pos, player_ctx->stream_duration);
    }
}
