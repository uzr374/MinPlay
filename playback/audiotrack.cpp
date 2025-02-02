#include "audiotrack.hpp"

extern "C"{
#include <libavutil/bprint.h>
#include <libavutil/opt.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

AudioTrack::AudioTrack(const CAVStream& st, std::condition_variable& cond) : AVTrack(st, cond), frame_pool(pkts, SAMPLE_QUEUE_SIZE, 1) {
    dec.decoder_thr = std::thread(&AudioTrack::run, this);
}

AudioTrack::~AudioTrack(){
    flush();
    pkts.abort();
    frame_pool.notify();
    if(dec.decoder_thr.joinable())
        dec.decoder_thr.join();
}

CAVFrame* AudioTrack::getFrame(){
    CAVFrame* af = nullptr;
    do {
        if(frame_pool.nb_remaining() == 0){
            return nullptr;
        }
        if (!(af = frame_pool.peek_readable())){
            return nullptr;
        }
        frame_pool.next();
    } while (af->serial() != serial());

    return af;
}

void AudioTrack::nextFrame(){
    frame_pool.next();
}

int64_t AudioTrack::lastPos(){
    return frame_pool.last_pos();
}

double AudioTrack::getClockVal() const{
    return clk.get();
}

void AudioTrack::updateClock(double pts){
    clk.set(pts);
}

void AudioTrack::setPauseStatus(bool p){
    clk.setPaused(p);
}

void AudioTrack::flush(){
    clk.resetTime();
    AVTrack::flush();
}

static inline
    bool cmp_audio_fmts(AVSampleFormat fmt1, int channel_count1,
                   AVSampleFormat fmt2, int channel_count2)
{
    /* If channel count == 1, planar and non-planar formats are the same */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

int AudioTrack::configure_audio_filters(const char *afilters)
{
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
    char aresample_swr_opts[512]{};
    const AVDictionaryEntry *e = NULL;
    AVDictionary* swr_opts = nullptr;
    AVBPrint bp{};
    char asrc_args[256]{};
    int ret;

    avfilter_graph_free(&agraph);
    if (!(agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);
    agraph->nb_threads = 1;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);

    while ((e = av_dict_iterate(swr_opts, e)))
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
    if (strlen(aresample_swr_opts))
        aresample_swr_opts[strlen(aresample_swr_opts)-1] = '\0';
    av_opt_set(agraph, "aresample_swr_opts", aresample_swr_opts, 0);

    av_channel_layout_describe_bprint(&audio_filter_src.ch_layout, &bp);

    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:time_base=%d/%d:channel_layout=%s",
                   audio_filter_src.freq, av_get_sample_fmt_name(audio_filter_src.fmt),
                   1, audio_filter_src.freq, bp.str);

    ret = avfilter_graph_create_filter(&filt_asrc,
                                       avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                       asrc_args, NULL, agraph);
    if (ret < 0)
        goto end;

    filt_asink = avfilter_graph_alloc_filter(agraph, avfilter_get_by_name("abuffersink"),
                                             "ffplay_abuffersink");
    if (!filt_asink) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if ((ret = av_opt_set(filt_asink, "sample_formats", "fltp", AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;

    if (1) {
        if ((ret = av_opt_set_array(filt_asink, "channel_layouts", AV_OPT_SEARCH_CHILDREN,
                                    0, 1, AV_OPT_TYPE_CHLAYOUT, &dec.avctx->ch_layout)) < 0)
            goto end;
        if ((ret = av_opt_set_array(filt_asink, "samplerates", AV_OPT_SEARCH_CHILDREN,
                                    0, 1, AV_OPT_TYPE_INT, &dec.avctx->sample_rate)) < 0)
            goto end;
    }

    ret = avfilter_init_dict(filt_asink, NULL);
    if (ret < 0)
        goto end;

    if ((ret = configure_filtergraph(agraph, afilters, filt_asrc, filt_asink)) < 0)
        goto end;

    in_audio_filter  = filt_asrc;
    out_audio_filter = filt_asink;

end:
    if (ret < 0)
        avfilter_graph_free(&agraph);
    av_bprint_finalize(&bp, NULL);

    return ret;
}

void AudioTrack::run()
{
    AVFrame *frame = av_frame_alloc();
    CAVFrame *af;
    int last_serial = -1;
    int got_frame = 0;
    int ret = 0;

    if (!frame)
        return;

    do {
        if ((got_frame = dec.decode_frame(frame, NULL)) < 0)
            goto the_end;
        else if (got_frame) {
            const bool reconfigure =
                cmp_audio_fmts(audio_filter_src.fmt, audio_filter_src.ch_layout.nb_channels,
                               AVSampleFormat(frame->format), frame->ch_layout.nb_channels)    ||
                av_channel_layout_compare(&audio_filter_src.ch_layout, &frame->ch_layout) ||
                audio_filter_src.freq           != frame->sample_rate ||
                dec.pkt_serial               != last_serial;

            if (reconfigure) {
                char buf1[1024]{}, buf2[1024]{};
                av_channel_layout_describe(&audio_filter_src.ch_layout, buf1, sizeof(buf1));
                av_channel_layout_describe(&frame->ch_layout, buf2, sizeof(buf2));
                av_log(NULL, AV_LOG_INFO,
                       "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                       audio_filter_src.freq, audio_filter_src.ch_layout.nb_channels, av_get_sample_fmt_name(audio_filter_src.fmt), buf1, last_serial,
                       frame->sample_rate, frame->ch_layout.nb_channels, av_get_sample_fmt_name(AVSampleFormat(frame->format)), buf2, dec.pkt_serial);

                audio_filter_src.fmt            = AVSampleFormat(frame->format);
                ret = av_channel_layout_copy(&audio_filter_src.ch_layout, &frame->ch_layout);
                if (ret < 0)
                    goto the_end;
                audio_filter_src.freq           = frame->sample_rate;
                last_serial                         = dec.pkt_serial;

                if ((ret = configure_audio_filters(nullptr)) < 0)
                    goto the_end;
            }

            if ((ret = av_buffersrc_add_frame(in_audio_filter, frame)) < 0)
                goto the_end;

            while ((ret = av_buffersink_get_frame_flags(out_audio_filter, frame, 0)) >= 0) {
                FrameData *fd = frame->opaque_ref ? (FrameData*)frame->opaque_ref->data : NULL;
                const auto tb = av_buffersink_get_time_base(out_audio_filter);
                if (!(af = frame_pool.peek_writable()))
                    goto the_end;

                af->setTimingInfo((frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb), av_q2d({frame->nb_samples, frame->sample_rate}));
                af->setPktPos(fd ? fd->pkt_pos : -1LL);
                af->setSerial(last_serial);

                av_frame_move_ref(af->av(), frame);
                frame_pool.push();

                if (pkts.serial() != dec.pkt_serial)
                    break;
            }
            if (ret == AVERROR_EOF)
                dec.finished_serial = dec.pkt_serial;
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);

the_end:
    avfilter_graph_free(&agraph);
    av_frame_free(&frame);
}
