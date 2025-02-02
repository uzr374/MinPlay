#include "videotrack.hpp"

extern "C"{
#include <libavutil/display.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
#include <libavutil/avstring.h>
}

VideoTrack::VideoTrack(const CAVStream& st, std::condition_variable& empty_q_cond, const std::vector<AVPixelFormat>& fmts) :
    AVTrack(st, empty_q_cond), frame_pool(pkts, VIDEO_PICTURE_QUEUE_SIZE, 1), supported_pix_fmts(fmts) {
    dec.decoder_thr = std::thread(&VideoTrack::run, this);
}

VideoTrack::~VideoTrack(){
    flush();
    pkts.abort();
    frame_pool.notify();
    if(dec.decoder_thr.joinable())
        dec.decoder_thr.join();
}

void VideoTrack::flush(){
    clk.resetTime();
    AVTrack::flush();
}

int VideoTrack::framesAvailable(){return frame_pool.nb_remaining();}
CAVFrame& VideoTrack::getLastPicture(){return *frame_pool.peek_last();}
CAVFrame& VideoTrack::peekCurrentPicture(){return *frame_pool.peek();}
CAVFrame& VideoTrack::peekNextPicture(){return *frame_pool.peek_next();}
bool VideoTrack::canDisplay(){return frame_pool.rindexShown();}
void VideoTrack::nextFrame(){frame_pool.next();}
bool VideoTrack::isAttachedPic(){return rel_st.isAttachedPic();}
int64_t VideoTrack::lastPos(){return frame_pool.last_pos();}

double VideoTrack::getClockVal() const{return clk.get();}
double VideoTrack::clockUpdateTime(){return clk.updatedAt();}
void VideoTrack::updateClock(double pts){clk.set(pts);}
void VideoTrack::setPauseStatus(bool p){clk.setPaused(p);}


int VideoTrack::queue_picture(AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    CAVFrame *vp;

    if (!(vp = frame_pool.peek_writable()))
        return -1;

    vp->setUploaded(false);
    vp->setTimingInfo(pts, duration);
    vp->setPktPos(pos);
    vp->setSerial(serial);

    av_frame_move_ref(vp->av(), src_frame);
    frame_pool.push();
    return 0;
}

int VideoTrack::get_video_frame(AVFrame *frame)
{
    const auto got_picture = dec.decode_frame(frame, nullptr);
    if (got_picture > 0) {
        const auto stream_sar = rel_st.sampleAR();
        if(stream_sar.num){
            frame->sample_aspect_ratio = stream_sar;
        }
    }

    return got_picture;
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

int VideoTrack::configure_video_filters(AVFilterGraph *graph, const char *vfilters, const AVFrame *frame)
{
    const auto& pix_fmts = supported_pix_fmts;
    char sws_flags_str[512]{};
    int ret;
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    const auto& codecpar = rel_st.codecPar();
    const AVRational fr = rel_st.frameRate();
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
    par->time_base           = rel_st.tb();
    par->width               = frame->width;
    par->height              = frame->height;
    par->sample_aspect_ratio = codecpar.sample_aspect_ratio;
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
                                0, Decoder::sdl_supported_color_spaces.size(),
                                AV_OPT_TYPE_INT, Decoder::sdl_supported_color_spaces.data())) < 0)
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
            const AVPacketSideData *psd = av_packet_side_data_get(codecpar.coded_side_data,
                                                                  codecpar.nb_coded_side_data,
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

in_video_filter  = filt_src;
out_video_filter = filt_out;

fail:
       av_freep(&par);
return ret;
}

void VideoTrack::run()
{
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;

    AVFilterGraph *graph = NULL;
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    AVPixelFormat last_format = AV_PIX_FMT_NONE;
    int last_serial = -1;

    for (;;) {
        ret = get_video_frame(frame);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;

        if (   last_w != frame->width
            || last_h != frame->height
            || last_format != frame->format
            || last_serial != dec.pkt_serial) {
            av_log(NULL, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
                   frame->width, frame->height,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(AVPixelFormat(frame->format)), "none"), dec.pkt_serial);
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if (!graph) {
                goto the_end;
            }
            graph->nb_threads = 0;
            if (configure_video_filters(graph, nullptr, frame) < 0) {
                goto the_end;
            }
            filt_in  = in_video_filter;
            filt_out = out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = AVPixelFormat(frame->format);
            last_serial = dec.pkt_serial;
        }

        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;

        while (ret >= 0) {
            FrameData *fd;

            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF)
                    dec.finished_serial = dec.pkt_serial;
                ret = 0;
                break;
            }

            fd = frame->opaque_ref ? (FrameData*)frame->opaque_ref->data : NULL;

            const auto tb = av_buffersink_get_time_base(filt_out);
            const auto frame_rate = av_buffersink_get_frame_rate(filt_out);
            duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            ret = queue_picture(frame, pts, duration, fd ? fd->pkt_pos : -1, dec.pkt_serial);
            av_frame_unref(frame);
            if (pkts.serial() != dec.pkt_serial)
                break;
        }

        if (ret < 0)
            goto the_end;
    }
the_end:
    avfilter_graph_free(&graph);
    av_frame_free(&frame);
}
