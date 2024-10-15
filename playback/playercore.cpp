#include "playercore.hpp"

#include <QApplication>

extern "C"{
#include "libavutil/dict.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
}

std::atomic_bool quit_request = false;
bool quitRequested(){return quit_request.load();}

//This function is supposed to be called exclusively from the main thread
void setQuitRequest(bool quit){quit_request.store(quit);}

// static int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub) {
//     int ret = AVERROR(EAGAIN);

//     for (;;) {
//         if (d->queue->serial == d->pkt_serial) {
//             do {
//                 if (d->queue->abort_request)
//                     return -1;

//                 switch (d->avctx->codec_type) {
//                 case AVMEDIA_TYPE_VIDEO:
//                     ret = avcodec_receive_frame(d->avctx, frame);
//                     if (ret >= 0) {
//                         frame->pts = frame->best_effort_timestamp;
//                     }
//                     break;
//                 case AVMEDIA_TYPE_AUDIO:
//                     ret = avcodec_receive_frame(d->avctx, frame);
//                     if (ret >= 0) {
//                         AVRational tb = (AVRational){1, frame->sample_rate};
//                         if (frame->pts != AV_NOPTS_VALUE)
//                             frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
//                         else if (d->next_pts != AV_NOPTS_VALUE)
//                             frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
//                         if (frame->pts != AV_NOPTS_VALUE) {
//                             d->next_pts = frame->pts + frame->nb_samples;
//                             d->next_pts_tb = tb;
//                         }
//                     }
//                     break;
//                 default:
//                     break;
//                 }
//                 if (ret == AVERROR_EOF) {
//                     d->finished = d->pkt_serial;
//                     avcodec_flush_buffers(d->avctx);
//                     return 0;
//                 }
//                 if (ret >= 0)
//                     return 1;
//             } while (ret != AVERROR(EAGAIN));
//         }

//         do {
//             if (d->queue->nb_packets == 0)
//                 d->empty_queue_cond->notify_one();
//             if (d->packet_pending) {
//                 d->packet_pending = 0;
//             } else {
//                 int old_serial = d->pkt_serial;
//                 if (d->queue->get(d->pkt, 1, &d->pkt_serial) < 0)
//                     return -1;
//                 if (old_serial != d->pkt_serial) {
//                     avcodec_flush_buffers(d->avctx);
//                     d->finished = 0;
//                     d->next_pts = d->start_pts;
//                     d->next_pts_tb = d->start_pts_tb;
//                 }
//             }
//             if (d->queue->serial == d->pkt_serial)
//                 break;
//             av_packet_unref(d->pkt);
//         } while (1);

//         if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
//             int got_frame = 0;
//             ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, d->pkt);
//             if (ret < 0) {
//                 ret = AVERROR(EAGAIN);
//             } else {
//                 if (got_frame && !d->pkt->data) {
//                     d->packet_pending = 1;
//                 }
//                 ret = got_frame ? 0 : (d->pkt->data ? AVERROR(EAGAIN) : AVERROR_EOF);
//             }
//             av_packet_unref(d->pkt);
//         } else {
//             if (d->pkt->buf && !d->pkt->opaque_ref) {
//                 //TODO: add a pkt_pos field to the future CAVFrame class and use it instead
//                 auto fd = av_buffer_allocz(sizeof(FrameData));
//                 if (!fd)
//                     return AVERROR(ENOMEM);
//                 ((FrameData*)fd->data)->pkt_pos = d->pkt->pos;
//                 d->pkt->opaque_ref = fd;
//             }

//             if (avcodec_send_packet(d->avctx, d->pkt) == AVERROR(EAGAIN)) {
//                 av_log(d->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
//                 d->packet_pending = 1;
//             } else {
//                 av_packet_unref(d->pkt);
//             }
//         }
//     }
// }

static void stream_component_close(VideoState *is, AVFormatContext *ic, int stream_index)
{
    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    auto st = ic->streams[stream_index];
    st->discard = AVDISCARD_ALL;

    switch (st->codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
    {
        auto lck = is->audioq.getLocker();
        is->athr_quit = true;
    }
        if(is->audio_render_thr.joinable())
            is->audio_render_thr.join();
        is->athr_quit = is->flush_athr = false;
        is->auddec = nullptr;
        is->audioq.flush();
        is->audio_st = nullptr;
        is->audio_stream = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
    {
        auto lck = is->videoq.getLocker();
        is->vthr_quit = true;
    }
        if(is->video_render_thr.joinable())
            is->video_render_thr.join();
        is->vthr_quit = is->flush_vthr = false;
        is->viddec = nullptr;
        is->videoq.flush();//No need to lock here since we're in the demuxer thread, and the decoder thread is not running
        is->video_st = nullptr;
        is->video_stream = -1;
        break;
    // case AVMEDIA_TYPE_SUBTITLE:
    //     decoder_abort(&is->subdec, is->subpq);
    //     decoder_destroy(&is->subdec);
    // is->subtitle_st = NULL;
    // is->subtitle_stream = -1;
    //     break;
    default:
        break;
    }
}

// static void video_image_display(VideoState *is)
// {
//     Subtitle *sp = NULL;
//     SDL_FRect rect{};

//     auto vp = is->pictq.peek_last();

//     if (is->subtitle_st) {
//         if (is->subpq.nb_remaining() > 0) {
//             sp = is->subpq.peek();

//             if (vp->frame.ts() >= sp->ts() + ((float) sp->constAv().start_display_time / 1000)) {
//                 if (!sp->isUploaded()) {
//                     uint8_t* pixels[4]{};
//                     int pitch[4]{};

//                     sp->ensureDimensions(vp->frame.width(), vp->frame.height());

//                     if (realloc_texture(&is->sub_texture, SDL_PIXELFORMAT_ARGB8888, sp->width(), sp->height(), SDL_BLENDMODE_BLEND, 1) < 0)
//                         return;

//                     for (int i = 0; i < sp->constAv().num_rects; i++) {
//                         AVSubtitleRect *sub_rect = sp->constAv().rects[i];

//                         sub_rect->x = av_clip(sub_rect->x, 0, sp->width() );
//                         sub_rect->y = av_clip(sub_rect->y, 0, sp->height());
//                         sub_rect->w = av_clip(sub_rect->w, 0, sp->width()  - sub_rect->x);
//                         sub_rect->h = av_clip(sub_rect->h, 0, sp->height() - sub_rect->y);

//                         is->sub_convert_ctx = sws_getCachedContext(is->sub_convert_ctx,
//                                                                    sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
//                                                                    sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
//                                                                    0, NULL, NULL, NULL);
//                         if (!is->sub_convert_ctx) {
//                             av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
//                             return;
//                         }
//                         if (SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)pixels, pitch)) {
//                             sws_scale(is->sub_convert_ctx, (const uint8_t * const *)sub_rect->data, sub_rect->linesize,
//                                       0, sub_rect->h, pixels, pitch);
//                             SDL_UnlockTexture(is->sub_texture);
//                         }
//                     }
//                     sp->setUploaded(true);
//                 }
//             } else
//                 sp = NULL;
//         }
//     }

//     calculate_display_rect(&rect, 0, 0, is->width, is->height, vp->frame.width(), vp->frame.height(), vp->frame.sampleAR());
//     //set_sdl_yuv_conversion_mode(vp->frame);

//     if (!vp->uploaded) {
//         if (!upload_texture(&is->vid_texture, vp->frame.constAv())) {
//             //set_sdl_yuv_conversion_mode(NULL);
//             return;
//         }
//         vp->uploaded = 1;
//     }

//     SDL_RenderTextureRotated(renderer, vid_texture, NULL, &rect, 0, NULL, vp->flip_v ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);
//     //set_sdl_yuv_conversion_mode(NULL);
//     if (sp) {
// #if 1
//         SDL_RenderTexture(renderer, sub_texture, NULL, &rect);
// #else
//         int i;
//         double xratio = (double)rect.w / (double)sp->width;
//         double yratio = (double)rect.h / (double)sp->height;
//         for (i = 0; i < sp->sub.num_rects; i++) {
//             SDL_Rect *sub_rect = (SDL_Rect*)sp->sub.rects[i];
//             SDL_Rect target = {.x = rect.x + sub_rect->x * xratio,
//                                .y = rect.y + sub_rect->y * yratio,
//                                .w = sub_rect->w * xratio,
//                                .h = sub_rect->h * yratio};
//             SDL_RenderCopy(renderer, is->sub_texture, sub_rect, &target);
//         }
// #endif
//     }
// }

static double get_lead_clock(VideoState *is)
{
    return is->audio_st? is->audclk.get() : is->vidclk.get();;
}

// static int subtitle_thread(void *arg)
// {
//     return 0;
//     VideoState *is = (VideoState*)arg;
//     //AVSubtitle sub{};

//     for (;;) {
//         /*Subtitle* sp;
//         if (!(sp = is->subpq.peek_writable()))
//             break;

//         int got_subtitle;
//         if ((got_subtitle = decoder_decode_frame(&is->subdec, NULL, &sp->av())) < 0)
//             break;

//         double pts = 0;

//         if (got_subtitle && sp->constAv().format == 0) {
//             sp->unref();
//             if (sp->constAv().pts != AV_NOPTS_VALUE)
//                 pts = sp->constAv().pts / (double)AV_TIME_BASE;
//             sp->sub_pts = pts;
//             sp->serial = is->subdec.pkt_serial;
//             sp->sub_width = is->subdec.avctx->width;
//             sp->sub_height = is->subdec.avctx->height;
//             sp->uploaded = 0;

//             /* now we can update the picture count */
//             /*is->subpq.push();
//         } else if (got_subtitle) {
//             avsubtitle_free(&sp->sub);
//         }*/
//     }
//    // avsubtitle_free(&sub);

//     return 0;
// }

static std::vector<float> convert_audio_frame(const CAVFrame& af,
                                              AudioParams& audio_src, const AudioParams& audio_tgt,  SwrContext*& swr_ctx, bool flush){
    std::vector<float> adata;

    if (af.sampleFmt() != audio_src.fmt || af.sampleRate() != audio_src.freq ||
        av_channel_layout_compare(&af.chLayout(), &audio_src.ch_layout.constAv())) {
        swr_free(&swr_ctx);
        const auto ret = swr_alloc_set_opts2(&swr_ctx,
                                  &audio_tgt.ch_layout.constAv(), audio_tgt.fmt, audio_tgt.freq,
                                  &af.chLayout(), af.sampleFmt(), af.sampleRate(),
                                  0, NULL);
        if (ret < 0 || swr_init(swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                   af.sampleRate(), av_get_sample_fmt_name(af.sampleFmt()), af.chCount(),
                   audio_tgt.freq, av_get_sample_fmt_name(audio_tgt.fmt), audio_tgt.ch_layout.nbChannels());
            swr_free(&swr_ctx);
            return adata;
        }
        av_log(NULL, AV_LOG_INFO,
               "Created a sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels\n",
               af.sampleRate(), av_get_sample_fmt_name(af.sampleFmt()), af.chCount(),
               audio_tgt.freq, av_get_sample_fmt_name(audio_tgt.fmt), audio_tgt.ch_layout.nbChannels());
        audio_src.ch_layout = CAVChannelLayout(af.chLayout());
        audio_src.freq = af.sampleRate();
        audio_src.fmt = af.sampleFmt();
    }

    if (swr_ctx) {
        const int out_count = swr_get_out_samples(swr_ctx, af.nbSamples());
        if (out_count < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_get_out_samples() failed\n");
            return adata;
        }

        adata.resize(out_count * audio_tgt.ch_layout.nbChannels(), 0.0f);
        auto out = reinterpret_cast<uint8_t*>(adata.data());
        auto in = af.extData();
        const int len2 = swr_convert(swr_ctx, &out, out_count, in, af.nbSamples());
        if (len2 <= 0) {
            if(len2 < 0)
                av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            adata.clear();
        } else {
            adata.resize(len2 * audio_tgt.ch_layout.nbChannels());
            if(flush){
                const auto nb_remaining_samples = swr_get_out_samples(swr_ctx, 0);
                if (nb_remaining_samples < 0) {
                    av_log(NULL, AV_LOG_ERROR, "swr_get_out_samples() failed\n");
                } else if(nb_remaining_samples > 0){
                    std::vector<float> remaining_samples(nb_remaining_samples, 0.0f);
                    out = reinterpret_cast<uint8_t*>(remaining_samples.data());
                    const int len2 = swr_convert(swr_ctx, &out, nb_remaining_samples, nullptr, 0);
                    if (len2 < 0) {
                        av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
                    } else if(len2 > 0){
                        remaining_samples.resize(len2 * audio_tgt.ch_layout.nbChannels());
                        adata.reserve(adata.size() + remaining_samples.size());
                        adata.insert(adata.end(), remaining_samples.begin(), remaining_samples.end());
                    }
                }
            }
        }
    } else {
        const auto data_ptr = reinterpret_cast<const float*>(af.constDataPlane(0));
        adata = std::vector<float>(data_ptr, data_ptr + af.nbSamples() * af.chCount());
    }

    return adata;
}

static SDL_AudioStream* audio_open(CAVChannelLayout wanted_channel_layout, int wanted_sample_rate)
{
    if (!wanted_channel_layout.isNative()) //Handle streams with non-standard channel layout
        wanted_channel_layout.make_default(wanted_channel_layout.nbChannels());

    const auto wanted_nb_channels = wanted_channel_layout.nbChannels();
    SDL_AudioStream* astream = nullptr;
    if (wanted_sample_rate > 0 && wanted_nb_channels > 0) {
        if(SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            const SDL_AudioSpec wanted_spec{.format = SDL_AUDIO_F32, .channels = wanted_nb_channels, .freq = wanted_sample_rate};
            if(!(astream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &wanted_spec, nullptr, nullptr))){
                av_log(NULL, AV_LOG_ERROR, "Audio open failed\n");
                SDL_QuitSubSystem(SDL_INIT_AUDIO);
            } else{
                SDL_ResumeAudioStreamDevice(astream);
            }
        }
    } else{
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
    }

    return astream;
}

void audio_render_thread(VideoState* ctx){
    static constexpr auto audiobuf_preferred_duration = 0.1, audiobuf_empty_threshold = 0.01;//in seconds
    static constexpr auto timeout = std::chrono::milliseconds(10);
    static constexpr auto framebuffer_preferred_size = 12; //Try to keep enough CAVFrames worth of data buffered in decoded_frames

    auto& dec = *ctx->auddec;
    const auto audio_tgt = ([&dec]{auto format = dec.outputAudioFormat(); format.fmt = AV_SAMPLE_FMT_FLT; return format;})();
    AudioParams audio_src;

    auto astream = audio_open(audio_tgt.ch_layout, audio_tgt.freq);
    if(!astream) return;

    bool local_paused = false;
    SwrContext *swr_ctx = nullptr;
    std::list<CAVFrame> decoded_frames;
    bool eos = false, eos_reported = false, flush = false, seek_ready = false, force_frame = true;
    float last_volume = 1.0f;
    CAVPacket pkt;
    double last_pts = 0.0, last_duration = 0.0;
    int64_t last_byte_pos = -1;
    std::vector<float> adata;

    auto wait_timeout = []{std::this_thread::sleep_for(timeout);};

    auto get_buffered_duration = [&audio_tgt](SDL_AudioStream* stream){
        const auto bytes_queued = SDL_GetAudioStreamQueued(stream);
        return ((double)bytes_queued/(audio_tgt.ch_layout.nbChannels() * sizeof(float)))/(double)audio_tgt.freq;
    };

    auto& queue = ctx->audioq;
    auto& clk = ctx->audclk;

    while(!quitRequested()){
        auto params_lck = queue.getLocker();
        const bool quit_req = ctx->athr_quit;
        if(quit_req) break;
        const bool flush_req = ctx->flush_athr;
        ctx->flush_athr = false;
        const auto pause_req = ctx->athr_pause_req;
        const bool queue_is_empty = queue.isEmpty();
        const bool fetch_pkt = decoded_frames.size() < framebuffer_preferred_size && !flush_req && !queue_is_empty;
        const float cur_volume = ctx->audio_volume;
        if(fetch_pkt){
            queue.get(pkt);
        }
        if(last_pts > 0 && !std::isnan(last_pts))
            ctx->last_audio_pts = last_pts + last_duration;
        if(last_byte_pos > 0)
            ctx->last_audio_pos = last_byte_pos;
        ctx->athr_seek_ready = seek_ready;
        params_lck.unlock();

        if(pause_req != local_paused){
            local_paused = pause_req;
            const bool success = local_paused ? SDL_PauseAudioStreamDevice(astream) : SDL_ResumeAudioStreamDevice(astream);
            ctx->audclk.set_paused(local_paused);
        }

        if(last_volume != cur_volume){
            last_volume = cur_volume;
            SDL_SetAudioStreamGain(astream, last_volume);
        }

        if(flush_req){
            decoded_frames.clear();
            if(astream)
                SDL_ClearAudioStream(astream);
            audio_src = AudioParams();
            dec.flush();
            eos = eos_reported = seek_ready = false;
            force_frame = true;
            last_pts = last_duration = 0.0;
            last_byte_pos = -1;
            pkt.unref();
            ctx->audclk.set_eos(false, NAN);
            continue;
        }

        if(!pkt.isEmpty() || pkt.isFlush()){
            dec.decodeAudioPacket(pkt, decoded_frames);
            pkt.unref();
        }

        const auto buffered_duration = get_buffered_duration(astream);
        eos = dec.eofReached() && decoded_frames.empty() && buffered_duration < audiobuf_empty_threshold;
        if(eos){
            seek_ready = true;
            if(!eos_reported){
                ctx->audclk.set_eos(true, last_pts + last_duration);
                std::scoped_lock dlck(ctx->demux_mutex);
                ctx->athr_eos = eos_reported = true;
            }
        }

        const auto abuffer_hungry = buffered_duration < audiobuf_preferred_duration;
        const bool wait = (decoded_frames.empty() && queue_is_empty) || !abuffer_hungry || (local_paused && !force_frame) || eos;
        if(wait){
            wait_timeout();
        } else {
            if(!decoded_frames.empty()){
                const auto& af = decoded_frames.front();
                last_pts = af.ts();
                last_duration = af.dur();
                last_byte_pos = af.pktPos();
                const bool eos_upcoming = dec.eofReached() && decoded_frames.size() == 1;
                adata = convert_audio_frame(af, audio_src, audio_tgt, swr_ctx, eos_upcoming);
                decoded_frames.pop_front();
                const auto stream_pos = last_pts - buffered_duration;
                if (!std::isnan(last_pts)) {
                    ctx->audclk.set(stream_pos);
                    emit playerCore.sigUpdateStreamPos(stream_pos);
                }
                if(!adata.empty()){
                    SDL_PutAudioStreamData(astream, adata.data(), adata.size() * sizeof(float));
                    adata.clear();
                }
                if(eos_upcoming){
                    SDL_FlushAudioStream(astream);
                }
                seek_ready = true;
                force_frame = false;
            }
        }
    }

    if(astream){
        SDL_DestroyAudioStream(astream);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }

    if(swr_ctx){
        swr_free(&swr_ctx);
    }
}

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

static double compute_target_delay(double delay, VideoState *ctx, double max_duration)
{
    if(ctx->audio_st){
        /* update delay to follow leading synchronisation source */
        /* if video is not lead, we try to correct big delays by
           duplicating or deleting a frame */
        const double diff = ctx->vidclk.get() - ctx->audclk.get();

        /* skip or repeat frame. We take into account the
           delay to compute the threshold. I still don't know
           if it is the best guess */
        const double sync_threshold = std::max(AV_SYNC_THRESHOLD_MIN, std::min(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < max_duration) {
            if (diff <= -sync_threshold)
                delay = std::max(0.0, delay + diff);
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay += diff;
            else if (diff >= sync_threshold)
                delay *= 2;
        }
    }

    return delay;
}

static double vp_duration(const CAVFrame& vp, double last_pts, double last_duration, double max_duration) {
    const double duration = vp.ts() - last_pts;
    if (isnan(duration) || duration <= 0 || duration > max_duration)
        return last_duration;
    else
        return duration;
}

void video_render_thread(VideoState* ctx){
    static constexpr auto timeout = std::chrono::milliseconds(10);
    static constexpr auto sleep_threshold = 0.0015; //Wait if the delay is larger than this value, if below - then show the frame
    static constexpr auto framebuffer_preferred_size = 2;//Try to keep 2 CAVFrames worth of data buffered in decoded_frames
    static constexpr auto REFRESH_RATE = 0.01;//Should be less than 1/fps
    const double max_frame_duration = ctx->max_frame_duration;
    bool local_paused = false;
    SwsContext *sub_convert_ctx = nullptr;
    std::list<CAVFrame> decoded_frames;
    bool eos = false, eos_reported = false, flush = false, seek_ready = false, update_frame_timer = false, force_frame = true;
    double frame_timer = 0.0, last_pts = 0.0, last_duration = 0.0;
    double remaining_time = 0.0, remaining_time_set_at = 0.0;

    CAVPacket pkt;
    int64_t last_byte_pos = -1;

    auto& queue = ctx->videoq;
    Decoder& dec = *ctx->viddec;
    playerCore.createSDLRenderer();
    auto renderer = playerCore.sdlRenderer();
    dec.setSupportedPixFmts(renderer->supportedFormats());

    auto wait_timeout = []{std::this_thread::sleep_for(timeout);};

    while(!quitRequested()){
        auto params_lck = queue.getLocker();
        const bool quit_req = ctx->vthr_quit;
        if(quit_req) break;
        const bool flush_req = ctx->flush_vthr;
        ctx->flush_vthr = false;
        const bool pause_req = ctx->vthr_pause_req;
        const bool queue_is_empty = queue.isEmpty();
        const bool should_fetch_pkt = decoded_frames.size() < framebuffer_preferred_size && !flush_req && !queue_is_empty;
        if(should_fetch_pkt){
            queue.get(pkt);
        }
        if(last_byte_pos > 0)
            ctx->last_video_pos = last_byte_pos;
        if(last_pts > 0 && !std::isnan(last_pts))
            ctx->last_video_pts = last_pts;
        ctx->vthr_seek_ready = seek_ready;
        params_lck.unlock();

        if(flush_req){
            decoded_frames.clear();
            dec.flush();
            update_frame_timer = force_frame = true;
            eos = eos_reported = seek_ready = false;
            remaining_time = remaining_time_set_at = 0.0;
            last_pts = last_duration = 0.0;
            last_byte_pos = -1;
            pkt.unref();
            ctx->vidclk.set_eos(false, NAN);
            continue;
        }

        if(!pkt.isEmpty() || pkt.isFlush()){
            dec.decodeVideoPacket(pkt, decoded_frames);
            pkt.unref();
        } else if(decoded_frames.empty()) {
            wait_timeout();
        }

        eos = decoded_frames.empty() && dec.eofReached();
        if(eos){
            seek_ready = true;
            if(!eos_reported){
                ctx->vidclk.set_eos(true, last_pts);
                std::scoped_lock dlck(ctx->demux_mutex);
                ctx->vthr_eos = eos_reported = true;
            }
        }

        if(pause_req != local_paused){
            local_paused = pause_req;
            ctx->vidclk.set_paused(local_paused);
        }

        if((local_paused && !force_frame) || eos){
            wait_timeout();
            continue;
        }

        if(!decoded_frames.empty()) {
            if (update_frame_timer){
                frame_timer = gettime_s();
                update_frame_timer = false;
            } else if(remaining_time_set_at > 0.0){
                remaining_time = std::min(remaining_time, REFRESH_RATE);
                const auto actual_remaining_time = remaining_time - (gettime_s() - remaining_time_set_at);
                //qDebug() << "True remaining time:  " << true_remaining_time;
                if(actual_remaining_time > sleep_threshold){
                    const int to_sleep = actual_remaining_time * 1000;
                    std::this_thread::sleep_for(std::chrono::milliseconds(to_sleep));
                }
                remaining_time = remaining_time_set_at = 0.0;
            }

            auto& vp = decoded_frames.front();

            const auto nom_last_duration = vp_duration(vp, last_pts, last_duration, max_frame_duration);
            const auto delay = compute_target_delay(nom_last_duration, ctx, max_frame_duration);
            const auto time = gettime_s();
            const auto next_frame_time = frame_timer + delay;
            if (!force_frame && time < next_frame_time) {
                remaining_time = next_frame_time - time;
                if(remaining_time > sleep_threshold){
                    remaining_time_set_at = time;
                    continue;
                }
            }

            last_pts = vp.ts();
            last_byte_pos = vp.pktPos();

            if (!isnan(last_pts))
                ctx->vidclk.set(last_pts, time);
            emit playerCore.sigUpdateStreamPos(last_pts);

            if(!force_frame){
                last_duration = nom_last_duration;
                frame_timer += delay;
                if (delay > 0 && time - frame_timer > AV_SYNC_THRESHOLD_MAX)
                    frame_timer = time;

                if (decoded_frames.size() > 1) { //Framedrop lookahead
                    auto& nextvp = *(++decoded_frames.begin());
                    const auto duration = vp_duration(nextvp, last_pts, last_duration, max_frame_duration);
                    if(time > frame_timer + duration){
                        decoded_frames.pop_front();
                        decoded_frames.pop_front();
                        qDebug() << "Dropping frames";
                        continue;
                    }
                }
            }
            force_frame = false;
            seek_ready = true;

            renderer->pushFrame(std::move(decoded_frames.front()));//Asynchronous double-buffering
            decoded_frames.pop_front();

            // if (is->subtitle_st) {
            //     while (is->subpq.nb_remaining() > 0) {
            //         sp = is->subpq.peek();

            //         if (is->subpq.nb_remaining() > 1)
            //             sp2 = is->subpq.peek_next();
            //         else
            //             sp2 = NULL;

            //         if (sp->serial() != is->subtitleq.serial
            //             || (is->vidclk.pts() > (sp->ts() + ((float) sp->constAv().end_display_time / 1000)))
            //             || (sp2 && is->vidclk.pts() > (sp2->ts() + ((float) sp2->constAv().start_display_time / 1000))))
            //         {
            //             if (sp->isUploaded()) {
            //                 for (int i = 0; i < sp->constAv().num_rects; i++) {
            //                     const AVSubtitleRect *sub_rect = sp->constAv().rects[i];
            //                     uint8_t *pixels;
            //                     int pitch;

            //                     if (SDL_LockTexture(is->sub_texture, (SDL_Rect*)sub_rect, (void **)&pixels, &pitch)) {
            //                         for (int j = 0; j < sub_rect->h; j++, pixels += pitch)
            //                             memset(pixels, 0, sub_rect->w << 2);
            //                         SDL_UnlockTexture(is->sub_texture);
            //                     }
            //                 }
            //             }
            //             is->subpq.next();
            //         } else {
            //             break;
            //         }
            //     }
            // }
        }
    }

    renderer->clearDisplay();//Asynchronous, thread-safe
    playerCore.destroySDLRenderer();
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(VideoState *is, AVFormatContext* ic, int stream_index)
{
    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;

    auto st = ic->streams[stream_index];

    switch (st->codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->last_audio_stream = stream_index;

        try{
            is->auddec = std::make_unique<Decoder>(CAVStream(ic, st));
        } catch(...){
            is->auddec = nullptr;
            return -1;
        }

        is->audio_stream = stream_index;
        is->audio_st = st;

        is->audio_render_thr = std::thread(audio_render_thread, is);

        break;
    case AVMEDIA_TYPE_VIDEO:
        is->last_video_stream = stream_index;
        try{
            is->viddec = std::make_unique<Decoder>(CAVStream(ic, st));
        } catch(...){
            is->viddec = nullptr;
            return -1;
        }

        is->video_stream = stream_index;
        is->video_st = st;

        is->video_render_thr = std::thread(video_render_thread, is);
        break;
        // case AVMEDIA_TYPE_SUBTITLE:
        //     is->subtitle_stream = stream_index;
        //     is->subtitle_st = ic->streams[stream_index];

        //     if ((ret = decoder_init(&is->subdec, avctx, &is->subtitleq, &is->continue_read_thread)) < 0)
        //         goto fail;
        //     if ((ret = decoder_start(&is->subdec, subtitle_thread, "subtitle_decoder", is)) < 0)
        //         goto out;
        //     break;
    default:
        break;
    }
    st->discard = AVDISCARD_DEFAULT;

    return 1;
}

static int decode_interrupt_cb(void *ctx)
{
    return quitRequested();
}

static bool check_buffer_fullness(const AVStream *astream, PacketQueue& aq,
                                  const AVStream* vstream, PacketQueue& vq) {
    auto stream_is_valid = [](const AVStream* st){
        return st && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC);
    };

    constexpr auto MAX_QUEUE_SIZE = (20 * 1024 * 1024);
    constexpr auto MIN_PACKETS = 30;
    constexpr auto MIN_DURATION = 1.5;

    int byte_size = 0;

    auto queue_is_full = [&byte_size](PacketQueue& q, const AVStream* st)->bool {
        const auto params = q.getParams();
        byte_size += params.size;
        return params.nb_packets > MIN_PACKETS && (!params.duration || av_q2d(st->time_base) * params.duration > MIN_DURATION);
    };

    bool aq_full = true, vq_full = true;
    if(stream_is_valid(astream)){
        aq_full = queue_is_full(aq, astream);
    }
    if(stream_is_valid(vstream)){
        vq_full = queue_is_full(vq, vstream);
    }

    return  byte_size > MAX_QUEUE_SIZE || (aq_full && vq_full);
}

static bool is_realtime(AVFormatContext *s)
{
    if(!strcmp(s->iformat->name, "rtp")
        || !strcmp(s->iformat->name, "rtsp")
        || !strcmp(s->iformat->name, "sdp"))
        return true;

    if(s->pb && (!strncmp(s->url, "rtp:", 4)
                  || !strncmp(s->url, "udp:", 4)))
        return true;
    return false;
}

static int demux_thread(VideoState* is)
{
    int st_index[AVMEDIA_TYPE_NB];
    AVDictionary* format_opts = nullptr;
    bool realtime = false, local_paused = false, queue_attachments_req = false, seek_by_bytes = false;
    int64_t last_seek_pos = 0, last_seek_rel = 0;
    int seek_flags = 0, err = 0;
    bool demux_eof = false, video_eos = false, audio_eos = false;

    auto set_seek = [&](int64_t pos, int64_t rel, bool by_bytes)
    {
        last_seek_pos = pos;
        last_seek_rel = rel;
        if (by_bytes)
            seek_flags |= AVSEEK_FLAG_BYTE;
        else
            seek_flags &= ~AVSEEK_FLAG_BYTE;
    };

    auto wait_timeout = [is]{
        std::unique_lock lck(is->demux_mutex);
        is->continue_read_thread.wait_for(lck, std::chrono::milliseconds(10));
    };

    memset(st_index, -1, sizeof(st_index));

    auto ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        goto fail;
    }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;
    av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
    err = avformat_open_input(&ic, is->url.c_str(), nullptr, &format_opts);
    av_dict_free(&format_opts);
    if (err < 0) {
        goto fail;
    }

    ic->flags |= AVFMT_FLAG_GENPTS;
    if (avformat_find_stream_info(ic, nullptr) < 0) {
        av_log(NULL, AV_LOG_WARNING, "Could not find codec parameters\n");
        goto fail;
    }

    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

    seek_by_bytes = !(ic->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
                        !!(ic->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", ic->iformat->name);
    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    emit playerCore.sigReportStreamDuration(ic->duration/(double)AV_TIME_BASE);

    /*if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
        window_title = av_asprintf("%s - %s", t->value, input_filename);*/

    realtime = is_realtime(ic);

    for (int i = 0; i < ic->nb_streams; i++) {
        ic->streams[i]->discard = AVDISCARD_ALL;
    }

    st_index[AVMEDIA_TYPE_VIDEO] =
        av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                            st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);

    st_index[AVMEDIA_TYPE_AUDIO] =
        av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                            st_index[AVMEDIA_TYPE_AUDIO],
                            st_index[AVMEDIA_TYPE_VIDEO],
                            NULL, 0);

    // st_index[AVMEDIA_TYPE_SUBTITLE] =
    //     av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
    //                         st_index[AVMEDIA_TYPE_SUBTITLE],
    //                         (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
    //                              st_index[AVMEDIA_TYPE_AUDIO] :
    //                              st_index[AVMEDIA_TYPE_VIDEO]),
    //                         NULL, 0);

    if(stream_component_open(is, ic, st_index[AVMEDIA_TYPE_VIDEO]) >= 0){
        queue_attachments_req = true;
        demux_eof = false;
    }

    if(stream_component_open(is, ic, st_index[AVMEDIA_TYPE_AUDIO]) >= 0){
        demux_eof = false;
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s'\n",
               is->url.c_str());
        goto fail;
    }

    stream_component_open(is, ic, st_index[AVMEDIA_TYPE_SUBTITLE]);
    emit playerCore.setControlsActive(true);

    while (!quitRequested()) {
        {
            std::scoped_lock lck(is->demux_mutex);
            const bool pause_changed = (is->pause_req != local_paused);
            const bool seek_requested = is->seek_req;
            is->seek_req = false;
            if (pause_changed || seek_requested) {
                if(pause_changed){
                    local_paused = !local_paused;
                    const auto ret = local_paused ? av_read_pause(ic) : av_read_play(ic);
                }
                auto lck1 = is->videoq.getLocker();
                auto lck2 = is->audioq.getLocker();
                if(pause_changed)
                    is->athr_pause_req = is->vthr_pause_req = local_paused;//Forward pause/unpause to the worker threads
                const bool can_seek = (!is->audio_st || is->athr_seek_ready) && (!is->video_st || is->vthr_seek_ready);
                if(seek_requested && can_seek){
                    auto lck3 = is->subtitleq.getLocker();
                    const auto info = is->seek_info;
                    switch(info.type){
                    case SeekInfo::SEEK_PERCENT:
                        if (seek_by_bytes || ic->duration <= 0) {
                            const uint64_t size =  avio_size(ic->pb);
                            set_seek(size*info.percent, 0, true);
                        } else {
                            const auto tns  = ic->duration / AV_TIME_BASE;
                            const int thh  = tns / 3600;
                            const int tmm  = (tns % 3600) / 60;
                            const int tss  = (tns % 60);
                            const double frac = info.percent;
                            const int ns   = frac * tns;
                            const int hh   = ns / 3600;
                            const int mm   = (ns % 3600) / 60;
                            const int ss   = (ns % 60);
                            const int64_t ts = frac * ic->duration + (ic->start_time == AV_NOPTS_VALUE ? 0LL : ic->start_time);
                            set_seek(ts, 0, false);
                            av_log(NULL, AV_LOG_INFO, "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)\n", frac*100,
                                   hh, mm, ss, thh, tmm, tss);
                        }
                        break;
                    case SeekInfo::SEEK_INCREMENT:
                    {
                        double incr = info.increment;
                        if(incr < 0.0)
                            seek_flags |= AVSEEK_FLAG_BACKWARD;
                        if (seek_by_bytes) {
                            int64_t pos = -1;
                            if (is->audio_stream >= 0)
                                pos = is->last_audio_pos;
                            if (pos < 0 && is->video_stream >= 0)
                                pos = is->last_video_pos;
                            if (pos < 0)
                                pos = avio_tell(ic->pb);
                            if (ic->bit_rate)
                                incr *= ic->bit_rate / 8.0;//get the byte size
                            else
                                incr *= 180000.0;
                            pos += incr;
                            set_seek(pos, incr, true);
                        } else {
                            double pos = is->audio_st? is->last_audio_pts : is->last_video_pts;
                            if (isnan(pos))
                                pos = (double)last_seek_pos / AV_TIME_BASE;
                            pos += incr;
                            if (ic->start_time != AV_NOPTS_VALUE && pos < ic->start_time / (double)AV_TIME_BASE)
                                pos = ic->start_time / (double)AV_TIME_BASE;
                            set_seek((int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), false);
                        }
                    }
                    break;
                    default:
                        break;
                    }
                    /*Execute the seek*/
                    const int64_t seek_target = last_seek_pos;
                    const int64_t seek_min    = last_seek_rel > 0 ? seek_target - last_seek_rel + 2: INT64_MIN;
                    const int64_t seek_max    = last_seek_rel < 0 ? seek_target - last_seek_rel - 2: INT64_MAX;
                    // FIXME the +-2 is due to rounding being not done in the correct direction in generation
                    //      of the seek_pos/seek_rel variables

                    const auto seekRes = avformat_seek_file(ic, -1, seek_min, seek_target, seek_max, seek_flags);
                    if (seekRes < 0) {
                        av_log(NULL, AV_LOG_ERROR, "%s: error while seeking\n", ic->url);
                    } else {
                        if (is->audio_stream >= 0){
                            is->audioq.flush();
                            is->athr_eos = false;
                        }
                        if (is->video_stream >= 0){
                            is->videoq.flush();
                            is->vthr_eos = false;
                        }
                        is->subtitleq.flush();
                    }
                    seek_flags = 0;
                    is->flush_athr = is->flush_vthr = queue_attachments_req = true;
                    is->vthr_seek_ready = is->athr_seek_ready = false;
                    demux_eof = false;
                }
                is->seek_info = SeekInfo();
            }

            video_eos = is->video_st && is->vthr_eos;
            audio_eos = is->audio_st && is->athr_eos;
        }

        if (!realtime && check_buffer_fullness(is->audio_st, is->audioq, is->video_st, is->videoq)) {
            wait_timeout();
            continue;
        }

        if (!local_paused && demux_eof && video_eos && audio_eos) {
            /*if (loop != 1 && (!loop || --loop)) {
                stream_seek(is, 0, 0, 0);
            } else if (autoexit) {
                ret = AVERROR_EOF;
                goto fail;
            }*/
        }

        if (queue_attachments_req) {
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                CAVPacket pkt;
                if (av_packet_ref(pkt.av(), &is->video_st->attached_pic) < 0)
                    goto fail;
                is->videoq.put(std::move(pkt));
                is->videoq.put_nullpacket();
            }
            queue_attachments_req = false;
        }

        if (local_paused && (!strcmp(ic->iformat->name, "rtsp") ||
                             (ic->pb && !strncmp(is->url.c_str(), "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            wait_timeout();
            continue;
        }

        CAVPacket cpkt;
        const auto readRes = av_read_frame(ic, cpkt.av());
        if (readRes < 0) {
            if ((readRes == AVERROR_EOF || avio_feof(ic->pb)) && !demux_eof) {
                if (is->video_stream >= 0)
                    is->videoq.put_nullpacket();
                if (is->audio_stream >= 0)
                    is->audioq.put_nullpacket();
                if (is->subtitle_stream >= 0)
                    is->subtitleq.put_nullpacket();
                demux_eof = true;
            }
            if (ic->pb && ic->pb->error) {
                break;
            }
            wait_timeout();
        } else {
            demux_eof = false;
            auto pkt = cpkt.av();
            if (pkt->stream_index == is->audio_stream) {
                is->audioq.put(std::move(cpkt));
            } else if (pkt->stream_index == is->video_stream
                       && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                is->videoq.put(std::move(cpkt));
            } else if (pkt->stream_index == is->subtitle_stream) {
                is->subtitleq.put(std::move(cpkt));
            }
        }
    }

fail:
    stream_component_close(is, ic, is->audio_stream);
    stream_component_close(is, ic, is->video_stream);
    stream_component_close(is, ic, is->subtitle_stream);

    emit playerCore.setControlsActive(false);

    avformat_close_input(&ic);

    return 0;
}

static void stream_cycle_channel(VideoState *is, AVFormatContext* ic, AVMediaType codec_type)
{
    int start_index = -1, stream_index = -1, old_index = -1;

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

    AVProgram *p = nullptr;
    int nb_streams = ic->nb_streams;
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
        const auto st = ic->streams[p ? p->stream_index[stream_index] : stream_index];
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
           av_get_media_type_string(codec_type), old_index, stream_index);

    stream_component_close(is, ic, old_index);
    stream_component_open(is, ic, stream_index);
}

// static void seek_chapter(VideoState *is, AVFormatContext* ic, int incr)
// {
//     if (!ic->nb_chapters)
//         return;//Request a seek by incr * 600.0 here

//     const int64_t pos = get_main_clock(is) * AV_TIME_BASE;
//     int i = 0;
//     /* find the current chapter */
//     for (i = 0; i < ic->nb_chapters; i++) {
//         const auto ch = ic->chapters[i];
//         if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
//             i--;
//             break;
//         }
//     }

//     i += incr;
//     i = std::max(i, 0);
//     if (i >= ic->nb_chapters)
//         return;

//     av_log(NULL, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
//     stream_seek(is, av_rescale_q(ic->chapters[i]->start, ic->chapters[i]->time_base,
//                                  AV_TIME_BASE_Q), 0, 0);
// }

// static void handle_gui_evt(VideoState *cur_stream, bool paused, const SDL_Event& event)
// {
//     double incr, pos, frac;

//     double x;
//     switch (event.type) {
//     case SDL_EVENT_KEY_DOWN:
//         // If we don't yet have a window, skip all key events, because read_thread might still be initializing...
//         if (!cur_stream->width)
//             return;
//         switch (event.key.key) {
//         case SDLK_PAGEUP:
//             if (cur_stream->ic->nb_chapters <= 1) {
//                 incr = 600.0;
//                 goto do_seek;
//             }
//             seek_chapter(cur_stream, 1);
//             break;
//         case SDLK_PAGEDOWN:
//             if (cur_stream->ic->nb_chapters <= 1) {
//                 incr = -600.0;
//                 goto do_seek;
//             }
//             seek_chapter(cur_stream, -1);
//             break;
//         case SDLK_LEFT:
//             incr = seek_interval ? -seek_interval : -10.0;
//             goto do_seek;
//         case SDLK_RIGHT:
//             incr = seek_interval ? seek_interval : 10.0;
//             goto do_seek;
//         case SDLK_UP:
//             incr = 60.0;
//             goto do_seek;
//         case SDLK_DOWN:
//             incr = -60.0;
//         do_seek:

//             break;
//         default:
//             break;
//         }
//         break;
//     case SDL_EVENT_MOUSE_BUTTON_DOWN:
//         if (event.button.button == SDL_BUTTON_LEFT) {
//             static int64_t last_mouse_left_click = 0;
//             if (av_gettime_relative() - last_mouse_left_click <= 500000) {
//                 //cur_stream->force_refresh = 1;
//                 last_mouse_left_click = 0;
//             } else {
//                 last_mouse_left_click = av_gettime_relative();
//             }
//         }
//     case SDL_EVENT_MOUSE_MOTION:
//         if (cursor_hidden) {
//             SDL_ShowCursor();
//             cursor_hidden = 0;
//         }
//         cursor_last_shown = av_gettime_relative();
//         if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
//             if (event.button.button != SDL_BUTTON_RIGHT)
//                 break;
//             x = event.button.x;
//         } else {
//             if (!(event.motion.state & SDL_BUTTON_RMASK))
//                 break;
//             x = event.motion.x;
//         }
//         break;
//     default:
//         break;
//     }
// }

static void set_playback_paused(VideoState* ctx, bool paused){
    if(quitRequested() || !ctx) return;
    std::scoped_lock lck(ctx->demux_mutex);
    ctx->pause_req = paused;
}

void PlayerCore::openURL(QString url){
    if(is_active()){
        stopPlayback();
    }
    player_ctx = std::make_unique<VideoState>();
    player_ctx->url = url.toStdString();
    player_ctx->demux_thr = std::thread(demux_thread, player_ctx.get());
}

void PlayerCore::stopPlayback(){
    if(!is_active()) return;
    setQuitRequest(true);
    if(player_ctx->demux_thr.joinable())
        player_ctx->demux_thr.join();
    player_ctx = nullptr;
    setQuitRequest(false);
    QCoreApplication::processEvents();//To properly delete the renderer
}

void PlayerCore::pausePlayback(){
    set_playback_paused(player_ctx.get(), true);
}

void PlayerCore::resumePlayback(){
    set_playback_paused(player_ctx.get(), false);
}
void PlayerCore::togglePause(){
    if(!is_active()) return;
    std::scoped_lock lck(player_ctx->demux_mutex);
    player_ctx->pause_req = !player_ctx->pause_req;
}

bool PlayerCore::is_active() {
    return player_ctx && !quitRequested();
}

void PlayerCore::requestSeekPercent(double percent){
    if(!is_active()) return;
    const SeekInfo info{.type = SeekInfo::SEEK_PERCENT, .percent = percent};
    std::scoped_lock lck(player_ctx->demux_mutex);
    player_ctx->seek_info = info;
    player_ctx->seek_req = true;
}

void PlayerCore::requestSeekIncr(double incr){
    if(!is_active()) return;
    const SeekInfo info{.type = SeekInfo::SEEK_INCREMENT, .increment = incr};
    std::scoped_lock lck(player_ctx->demux_mutex);
    player_ctx->seek_info = info;
    player_ctx->seek_req = true;
}

void PlayerCore::createSDLRenderer(){
    QMetaObject::invokeMethod(video_dw, &VideoDisplayWidget::createSDLRenderer, Qt::BlockingQueuedConnection, qReturnArg(video_renderer));
}

void PlayerCore::destroySDLRenderer(){
    if(video_renderer){
        QMetaObject::invokeMethod(video_dw, &VideoDisplayWidget::destroySDLRenderer, Qt::QueuedConnection);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));//Wait for the renderer to be destroyed(ugly)
        video_renderer = nullptr;
    }
}

void PlayerCore::reportStreamDuration(double dur){
    stream_duration = dur;
}

void PlayerCore::updateStreamPos(double pos){
    cur_pos = pos;
    emit updatePlaybackPos(pos, stream_duration);
}



static PlayerCore* plcore_inst = nullptr;

PlayerCore& PlayerCore::instance() {
    return *plcore_inst;
}

PlayerCore::PlayerCore(QObject* parent, VideoDisplayWidget* dw): QObject(parent), video_dw(dw){
    plcore_inst = this;

    connect(this, &PlayerCore::sigReportStreamDuration, this, &PlayerCore::reportStreamDuration);
    connect(this, &PlayerCore::sigUpdateStreamPos, this, &PlayerCore::updateStreamPos);
}

PlayerCore::~PlayerCore(){
    stopPlayback();
}


