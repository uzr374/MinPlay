#include "decoder.hpp"

const std::array<AVColorSpace, 3> Decoder::sdl_supported_color_spaces = {
    AVCOL_SPC_BT709,
    AVCOL_SPC_BT470BG,
    AVCOL_SPC_SMPTE170M,
};

Decoder::Decoder(const CAVStream& st, PacketQueue &q, std::condition_variable &empty_q_cond) :
    queue(q), empty_queue_cond(empty_q_cond) {
    packet_pending = false;
    finished_serial = 0;
    next_pts = 0LL;
    next_pts_tb = start_pts_tb = {};
    start_pts = AV_NOPTS_VALUE;
    pkt_serial = -1;

    const auto codecpar = st.codecPar();
    const auto codec = avcodec_find_decoder(codecpar.codec_id);
    if (!codec) {
        av_log(NULL, AV_LOG_WARNING,
               "No decoder could be found for codec %s\n", avcodec_get_name(codecpar.codec_id));
        throw std::runtime_error("Failed to find decoder!");
    }

    avctx = avcodec_alloc_context3(codec);
    if (!avctx)
        throw std::runtime_error("OOM!");

    int ret = avcodec_parameters_to_context(avctx, &codecpar);
    if (ret < 0)
        throw std::runtime_error("Failed to copy codec parameters!");

    avctx->pkt_timebase = st.tb();
    avctx->codec_id = codec->id;
    avctx->lowres = 0;
    avctx->thread_count = (codecpar.codec_type == AVMEDIA_TYPE_VIDEO) ? 0 : 1;
    if(!st.isSub())
        avctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;

    if (false)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;

    if ((ret = avcodec_open2(avctx, codec, nullptr)) < 0) {
        throw std::runtime_error("Failed to open decoder!");
    }

    if(st.noTimestamps()){
        start_pts = st.startTime();
        start_pts_tb = st.tb();
    }
}

Decoder::~Decoder(){
    destroy();
}

int Decoder::decode_frame(AVFrame *frame, AVSubtitle *sub) {
    int ret = AVERROR(EAGAIN);

    for (;;) {
        if (queue.serial() == pkt_serial) {
            do {
                if (queue.isAborted())
                    return -1;

                switch (avctx->codec_type) {
                case AVMEDIA_TYPE_VIDEO:
                    ret = avcodec_receive_frame(avctx, frame);
                    if (ret >= 0) {
                        frame->pts = frame->best_effort_timestamp;
                    }
                    break;
                case AVMEDIA_TYPE_AUDIO:
                    ret = avcodec_receive_frame(avctx, frame);
                    if (ret >= 0) {
                        /*Basic PTS generation logic for audio frames in case timestamps are missing*/
                        const AVRational tb {1, frame->sample_rate};
                        if (frame->pts != AV_NOPTS_VALUE)
                            frame->pts = av_rescale_q(frame->pts, avctx->pkt_timebase, tb);
                        else if (next_pts != AV_NOPTS_VALUE)
                            frame->pts = av_rescale_q(next_pts, next_pts_tb, tb);
                        if (frame->pts != AV_NOPTS_VALUE) {
                            next_pts = frame->pts + frame->nb_samples;
                            next_pts_tb = tb;
                        }
                    }
                    break;
                }
                if (ret == AVERROR_EOF) {
                    finished_serial = pkt_serial;
                    avcodec_flush_buffers(avctx);
                    return 0;
                }
                if (ret >= 0)
                    return 1;
            } while (ret != AVERROR(EAGAIN));
        }

        do {
            const auto [size, nb_packets, dur] = queue.getParams();
            if (nb_packets == 0)
                empty_queue_cond.notify_one();
            if (packet_pending) {
                packet_pending = 0;
            } else {
                const auto old_serial = pkt_serial;
                if (queue.get(pkt, true) < 0)
                    return -1;
                pkt_serial = pkt.serial();
                if (old_serial != pkt_serial) {
                    avcodec_flush_buffers(avctx);
                    finished_serial = 0;
                    next_pts = start_pts;
                    next_pts_tb = start_pts_tb;
                }
            }

            if (queue.serial() == pkt_serial){
                break;
            }
            pkt.unref();
        } while (true);

        if (avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            int got_frame = 0;
            ret = avcodec_decode_subtitle2(avctx, sub, &got_frame, pkt.constAv());
            if (ret < 0) {
                ret = AVERROR(EAGAIN);
            } else {
                if (got_frame && !pkt.constAv()->data) {
                    packet_pending = 1;
                }
                ret = got_frame ? 0 : (pkt.constAv()->data ? AVERROR(EAGAIN) : AVERROR_EOF);
            }
            pkt.unref();
        } else {
            if (pkt.constAv()->buf && !pkt.constAv()->opaque_ref) {
                FrameData *fd;

                pkt.av()->opaque_ref = av_buffer_allocz(sizeof(*fd));
                if (!pkt.constAv()->opaque_ref)
                    return AVERROR(ENOMEM);
                fd = (FrameData*)pkt.av()->opaque_ref->data;
                fd->pkt_pos = pkt.constAv()->pos;
            }

            if (avcodec_send_packet(avctx, pkt.constAv()) == AVERROR(EAGAIN)) {
                av_log(avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                packet_pending = 1;
            } else {
                pkt.unref();
            }
        }
    }
}

void Decoder::destroy() {
    avcodec_free_context(&avctx);
}

int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
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
        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

    ret = avfilter_graph_config(graph, NULL);
fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}
