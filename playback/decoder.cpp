#include "decoder.hpp"

#include <stdexcept>
#include <QDebug>

extern "C"{
#include <libavutil/cpu.h>
#include <libavutil/avstring.h>
#include <libavutil/opt.h>
#include <libavutil/display.h>
#include <libavutil/bprint.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

Decoder::Decoder(const CAVStream& src_stream) {
    stream = src_stream;
    auto codec = stream.getCodec();
    if(!codec) throw std::runtime_error("[Decoder]: failed to find codec by ID!");
    avctx = avcodec_alloc_context3(codec);
    if(!avctx) throw std::runtime_error("[Decoder]: failed to allocate AVCodecContext");
    if(avcodec_parameters_to_context(avctx, &stream.codecPar()) < 0)
    {
        avcodec_free_context(&avctx);
        throw std::runtime_error("Failed to initialize AVCodecContext");
    }
    avctx->pkt_timebase = stream.tb();
    avctx->lowres = 0;
    avctx->thread_count = 1;
    avctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
    if(false){
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;
    }
    if(stream.isVideo()){
        avctx->thread_count = 0;
        avctx->thread_type = FF_THREAD_SLICE | FF_THREAD_FRAME;
        avctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    } else if(stream.isAudio()){
        start_pts = stream.startTime();
        start_pts_tb = stream.tb();
        audio_tgt = AudioParams(stream.codecPar().ch_layout, AV_SAMPLE_FMT_FLTP, stream.codecPar().sample_rate);
    }

    if(avcodec_open2(avctx, codec, nullptr) < 0){
        avcodec_free_context(&avctx);
        throw std::runtime_error("Failed to open AVCodecContext");
    }
}

void Decoder::setSupportedPixFmts(const std::vector<AVPixelFormat>& supported_fmts){
    supported_pix_fmts = supported_fmts;
}

Decoder::~Decoder(){
    if(avctx){
        avcodec_free_context(&avctx);
    }
    if(graph){
        avfilter_graph_free(&graph);
    }
}

AudioParams Decoder::outputAudioFormat() const{
    return audio_tgt;
}

bool Decoder::eofReached() const{
    return eof_reached;
}

static constexpr std::array<AVColorSpace, 6> supported_color_spaces = {
    AVCOL_SPC_BT709,
    AVCOL_SPC_BT470BG,
    AVCOL_SPC_SMPTE170M,
    AVCOL_SPC_RGB,
    AVCOL_SPC_BT2020_CL,
    AVCOL_SPC_BT2020_NCL,
};

static inline
    bool cmp_audio_fmts(AVSampleFormat fmt1, int channel_count1,
                   AVSampleFormat fmt2, int channel_count2)
{
    /* If channel count == 1, planar and non-planar formats are the same */
    if ((channel_count1 == 1) && (channel_count2 == 1))
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return (channel_count1 != channel_count2) || (fmt1 != fmt2);
}

static int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
    int ret;
    const int nb_filters = graph->nb_filters;
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
    for (int i = 0; i < graph->nb_filters - nb_filters; i++)
        std::swap(graph->filters[i], graph->filters[i + nb_filters]);

    ret = avfilter_graph_config(graph, NULL);
fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

static int configure_video_filters(AVFilterGraph *graph, const char *vfilters,
                                   const CAVFrame& src, AVFilterContext*& in_video_filter, AVFilterContext*& out_video_filter,
                                   const std::vector<AVPixelFormat> supported_pix_fmts, const CAVStream& stream)
{
    char sws_flags_str[512] = "";
    int ret;
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    auto codecpar = stream.codecPar();
    const auto fr = stream.frameRate();
    const AVDictionaryEntry *e = NULL;
    auto par = av_buffersrc_parameters_alloc();
    if (!par)
        return AVERROR(ENOMEM);

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

    filt_src = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffer"),
                                           "ffplay_buffer");
    if (!filt_src) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    par->format              = src.pixFmt();
    par->time_base           = stream.tb();
    par->width               = src.width();
    par->height              = src.height();
    par->sample_aspect_ratio = codecpar.sample_aspect_ratio;
    par->color_space         = src.colorSpace();
    par->color_range         = src.colorRange();
    par->frame_rate          = fr;
    par->hw_frames_ctx =        src.constAv()->hw_frames_ctx;

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
                                0, supported_pix_fmts.size(), AV_OPT_TYPE_PIXEL_FMT, supported_pix_fmts.data())) < 0)
        goto fail;
    /*Do not set the following option for vk renderer*/
        if((ret = av_opt_set_array(filt_out, "colorspaces", AV_OPT_SEARCH_CHILDREN,
                                0, supported_color_spaces.size(),
                                AV_OPT_TYPE_INT, supported_color_spaces.data())) < 0)
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

    if (1) {
        int32_t *displaymatrix = NULL;
        auto sd = av_frame_get_side_data(src.constAv(), AV_FRAME_DATA_DISPLAYMATRIX);
        if (sd)
            displaymatrix = (int32_t *)sd->data;
        if (!displaymatrix) {
            const auto psd = av_packet_side_data_get(codecpar.coded_side_data,
                                                                  codecpar.nb_coded_side_data,
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

        if (src.isInterlaced())
            INSERT_FILT("yadif", nullptr);
    }

if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
    goto fail;

in_video_filter  = filt_src;
out_video_filter = filt_out;

fail:
       av_freep(&par);
return ret;
}

static int configure_audio_filters(const char *afilters, const AudioParams& audio_filter_src,
                                   AVFilterGraph*& agraph, AVFilterContext*& in_audio_filter,
                                   AVFilterContext*& out_audio_filter, const AudioParams& audio_tgt)
{
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
    char aresample_swr_opts[512]{}, asrc_args[256]{};
    const AVDictionaryEntry *e = NULL;
    AVBPrint bp{};
    int ret;

    avfilter_graph_free(&agraph);
    if (!(agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);
    agraph->nb_threads = 1;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);

    AVDictionary* swr_opts = nullptr;
    while ((e = av_dict_iterate(swr_opts, e)))
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
    if (strlen(aresample_swr_opts))
        aresample_swr_opts[strlen(aresample_swr_opts)-1] = '\0';
    av_opt_set(agraph, "aresample_swr_opts", aresample_swr_opts, 0);

    av_channel_layout_describe_bprint(&audio_filter_src.ch_layout.constAv(), &bp);

    snprintf(asrc_args, sizeof(asrc_args),
             "sample_rate=%d:sample_fmt=%s:time_base=%d/%d:channel_layout=%s",
             audio_filter_src.freq, av_get_sample_fmt_name(audio_filter_src.fmt),
             1, audio_filter_src.freq, bp.str);

    if ((ret = avfilter_graph_create_filter(&filt_asrc,
                                            avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                            asrc_args, NULL, agraph)) < 0)
        goto end;

    if (!(filt_asink = avfilter_graph_alloc_filter(agraph, avfilter_get_by_name("abuffersink"),
                                                   "ffplay_abuffersink"))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if ((ret = av_opt_set(filt_asink, "sample_formats", "fltp", AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;

    if ((ret = av_opt_set_array(filt_asink, "channel_layouts", AV_OPT_SEARCH_CHILDREN,
                                0, 1, AV_OPT_TYPE_CHLAYOUT, &audio_tgt.ch_layout.constAv())) < 0)
        goto end;
    if ((ret = av_opt_set_array(filt_asink, "samplerates", AV_OPT_SEARCH_CHILDREN,
                                0, 1, AV_OPT_TYPE_INT, &audio_tgt.freq)) < 0)
        goto end;

    if ((ret = avfilter_init_dict(filt_asink, NULL)) < 0)
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

bool Decoder::filterAudioFrame(CAVFrame& src, std::list<CAVFrame>& filtered){
    if (cmp_audio_fmts(audio_filter_src.fmt, audio_filter_src.ch_layout.nbChannels(),
                       src.sampleFmt(), src.chCount())    ||
            av_channel_layout_compare(&audio_filter_src.ch_layout.constAv(), &src.chLayout()) ||
            audio_filter_src.freq != src.sampleRate() ||
            flush_filters) {
        char buf1[1024]{}, buf2[1024]{};
        av_channel_layout_describe(&audio_filter_src.ch_layout.constAv(), buf1, sizeof(buf1));
        av_channel_layout_describe(&src.chLayout(), buf2, sizeof(buf2));
        av_log(NULL, AV_LOG_INFO,
               "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s to rate:%d ch:%d fmt:%s layout:%s\n",
               audio_filter_src.freq, audio_filter_src.ch_layout.nbChannels(), av_get_sample_fmt_name(audio_filter_src.fmt), buf1,
               src.sampleRate(), src.chCount(), av_get_sample_fmt_name(src.sampleFmt()), buf2);

        audio_filter_src.fmt            = src.sampleFmt();
        audio_filter_src.ch_layout = CAVChannelLayout(src.chLayout());
        audio_filter_src.freq           = src.sampleRate();

        if (configure_audio_filters(nullptr, audio_filter_src, graph, in_filter, out_filter, audio_tgt) < 0)
            return false;

        flush_filters = false;
    }

    if (av_buffersrc_add_frame(in_filter, src.av()) < 0)
        return false;

    while (true) {
        CAVFrame buf;
        const int ret = av_buffersink_get_frame_flags(out_filter, buf.av(), 0);
        if(ret < 0){
            if(ret == AVERROR_EOF) filters_eof = true;
            break;
        } else{
            AVFrame* frame = buf.av();
            //qDebug() << "Received an audio frame, nb_samples=" << frame->nb_samples;
            FrameData *fd = frame->opaque_ref ? (FrameData*)frame->opaque_ref->data : NULL;
            const auto tb = av_buffersink_get_time_base(out_filter);
            const double pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            const auto pos = fd ? fd->pkt_pos : -1LL;
            const double duration = av_q2d({frame->nb_samples, frame->sample_rate});
            buf.setPktPos(pos);
            buf.setTimingInfo(pts, duration);
            filtered.push_back(std::move(buf));
        }
    }

    return true;
}

bool Decoder::filterVideoFrame(CAVFrame& src, std::list<CAVFrame>& filtered){
    if (   last_w != src.width()
        || last_h != src.height()
        || last_pix_fmt != src.pixFmt()
        || flush_filters) {
        av_log(NULL, AV_LOG_INFO,
                   "Video frame changed from size:%dx%d format:%s to size:%dx%d format:%s\n",
                   last_w, last_h,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(last_pix_fmt), "none"),
                   src.width(), src.height(),
                   (const char *)av_x_if_null(av_get_pix_fmt_name(src.pixFmt()), "none"));
        avfilter_graph_free(&graph);
        if (!(graph = avfilter_graph_alloc())) {
            return false;
        }
        graph->nb_threads = 0;
        AVFilterContext* in_video_filter = nullptr, *out_video_filter = nullptr;
        if (configure_video_filters(graph, nullptr, src, in_video_filter, out_video_filter,
                                    supported_pix_fmts, stream) < 0) {
            qDebug() << "Failed to configure video filters, aborting playback...";
            avfilter_graph_free(&graph);
            return false;
        }
        in_filter  = in_video_filter;
        out_filter = out_video_filter;
        last_w = src.width();
        last_h = src.height();
        last_pix_fmt = src.pixFmt();
        flush_filters = false;
    }

    int ret = av_buffersrc_add_frame(in_filter, src.av());//Pass a nullptr frame here on EOF
    if (ret < 0)
        return false;

    while (true) {
        CAVFrame buf;
        ret = av_buffersink_get_frame_flags(out_filter, buf.av(), 0);
        if (ret < 0) {
            if (ret == AVERROR_EOF)
                filters_eof = true;;
            break;
        }
        auto frame = buf.av();
        const auto fd = frame->opaque_ref ? (FrameData*)frame->opaque_ref->data : NULL;
        const auto tb = av_buffersink_get_time_base(out_filter);
        const auto frame_rate = av_buffersink_get_frame_rate(out_filter);
        const double duration = (frame_rate.num && frame_rate.den ? av_q2d({frame_rate.den, frame_rate.num}) : 0);
        const double pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
        buf.setPktPos(fd ? fd->pkt_pos : -1);
        buf.setTimingInfo(pts, duration);
        filtered.push_back(std::move(buf));
    }

    return true;
}

bool Decoder::decodeVideoPacket(CAVPacket& src, std::list<CAVFrame>& decoded){
    preparePacket(src);
    const auto sendRes = avcodec_send_packet(avctx, src.isFlush() ? nullptr : src.constAv());
    if(sendRes < 0) return false;
    int recRes = 0;
    while(recRes >= 0){
        CAVFrame tmp_frame;
        recRes = avcodec_receive_frame(avctx, tmp_frame.av());
        if(recRes == 0){
            auto avframe = tmp_frame.av();
            avframe->pts = avframe->best_effort_timestamp;
            filterVideoFrame(tmp_frame, decoded);
        } else if(recRes == AVERROR_EOF){
            eof_reached = true;
        }
    }

    return true;
}

bool Decoder::decodeAudioPacket(CAVPacket& src, std::list<CAVFrame>& decoded){
    preparePacket(src);
    const auto sendRes = avcodec_send_packet(avctx, src.isFlush() ? nullptr : src.constAv());
    if(sendRes < 0) return false;
    int recRes = 0;
    while(recRes >= 0){
        CAVFrame tmp_frame;
        recRes = avcodec_receive_frame(avctx, tmp_frame.av());
        if(recRes == 0){
            auto frame = tmp_frame.av();
            const AVRational tb{1, frame->sample_rate};
            if (frame->pts != AV_NOPTS_VALUE)
                frame->pts = av_rescale_q(frame->pts, avctx->pkt_timebase, tb);
            else if (next_pts != AV_NOPTS_VALUE)
                frame->pts = av_rescale_q(next_pts, next_pts_tb, tb);
            if (frame->pts != AV_NOPTS_VALUE) {
                next_pts = frame->pts + frame->nb_samples;
                next_pts_tb = tb;
            }
            filterAudioFrame(tmp_frame, decoded);
        } else if(recRes == AVERROR_EOF){
            eof_reached = true;
        }
    }

    return true;
}

void Decoder::preparePacket(CAVPacket& cpkt){
    auto pkt = cpkt.av();
    if (pkt->buf && !pkt->opaque_ref) {
        auto fd = av_buffer_allocz(sizeof(FrameData));
        if (!fd)
            return;
        ((FrameData*)fd->data)->pkt_pos = pkt->pos;
        pkt->opaque_ref = fd;
    }
}

/*!Must be flushed only from the decoder thread*/
void Decoder::flush(){
    if(!avctx) return;
    avcodec_flush_buffers(avctx);
    flush_filters = true;
    next_pts = start_pts;
    next_pts_tb = start_pts_tb;
    eof_reached = false;
}
