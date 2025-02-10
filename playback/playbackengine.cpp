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

void read_thread(PlayerContext&);

struct PlayerContext {
    Q_DISABLE_COPY_MOVE(PlayerContext);

    SDLRenderer& sdl_renderer;
    AudioOutput aout;
    AudioResampler acvt;
    PlayerCore& core;

    std::mutex render_mutex; /*guards each iteration of the refresh loop*/
    double stream_duration = 0.0;
    bool streams_updated = false;
    std::vector<CAVStream> streams;
    SeekInfo seek_info;
    bool seek_req = false;

    std::thread read_thr;
    std::atomic_bool abort_request = false;
    bool queue_attachments_req = false;
    bool flush_playback = false;
    std::string url;
    std::mutex demux_mutex;
    std::condition_variable continue_read_thread;

    std::unique_ptr<AudioTrack> atrack;
    std::unique_ptr<VideoTrack> vtrack;
    std::unique_ptr<SubTrack> strack;

    std::vector<uint8_t> audio_buf;
    bool muted = false;

    bool paused = false;
    int frame_drops_late = 0;
    double frame_timer = 0.0;
    double max_frame_duration = 0.0;      // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
    bool step = false;

    PlayerContext() = delete;
    PlayerContext(std::string _url, SDLRenderer& renderer, PlayerCore& c) :
        url(_url), sdl_renderer(renderer), core(c){
        read_thr = std::thread(read_thread, std::ref(*this));
    }

    ~PlayerContext(){
        sdl_renderer.clearDisplay();
        abort_request = true;
        if(read_thr.joinable())
            read_thr.join();
    }
};

static void video_image_display(PlayerContext& ctx, const CAVFrame& vp)
{
    if (ctx.strack) {
        if (ctx.strack->subsAvailable() > 0) {
            auto sp = ctx.strack->peekCurrent();

            if (vp.ts() >= sp->startTime()) {
                if (!sp->isUploaded()) {
                    if (!sp->width() || !sp->height()) {
                        sp->ensureDimensions(vp.width(), vp.height());
                    }
                    //Upload the sp here
                    sp->setUploaded(true);
                }
            }
        }
    }

    ctx.sdl_renderer.updateVideoTexture(AVFrameView(*vp.constAv()));
    ctx.sdl_renderer.refreshDisplay();
}

static void request_ao_change(PlayerContext& ctx, int new_freq, int new_chn){
    ctx.aout.requestChange(new_freq, new_chn);
}

static void ao_close(PlayerContext& ctx){
    request_ao_change(ctx, 0, 0);
}

static void stream_component_close(PlayerContext& ctx, int stream_index, FormatContext& fmt_ctx)
{
    if (stream_index < 0 || stream_index >= fmt_ctx.streamCount())
        return;

    auto st = fmt_ctx.streamAt(stream_index);
    switch (st.codecPar().codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        ctx.atrack = nullptr;
        ao_close(ctx);
        break;
    case AVMEDIA_TYPE_VIDEO:
        ctx.vtrack = nullptr;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        ctx.strack = nullptr;
        break;
    default:
        break;
    }

    fmt_ctx.setStreamEnabled(stream_index, false);
}

/* get the current master clock value */
static double get_master_clock(PlayerContext& ctx)
{
    double clock = NAN;
    if(ctx.atrack){
        clock = ctx.atrack->getClockVal();
    } else if(ctx.vtrack){
        clock = ctx.vtrack->getClockVal();
    }
    return clock;
}

/* pause or resume the video */
static void stream_toggle_pause(PlayerContext& ctx)
{
    if (ctx.paused) {
        if(ctx.vtrack){
            ctx.frame_timer += gettime() - ctx.vtrack->clockUpdateTime();
        }
    }

    ctx.paused = !ctx.paused;
    if(ctx.vtrack){
        ctx.vtrack->setPauseStatus(ctx.paused);
    }
    if(ctx.atrack){
        ctx.atrack->setPauseStatus(ctx.paused);
    }
}

static void toggle_mute(PlayerContext& ctx)
{
    ctx.muted = !ctx.muted;
}

static void step_to_next_frame(PlayerContext& ctx)
{
    if (!ctx.paused)
        stream_toggle_pause(ctx);
    ctx.step = true;
}

static double compute_target_delay(double delay, PlayerContext& ctx)
{
    /* update delay to follow master synchronisation source */
    if (ctx.atrack) {
        /* if video is slave, we try to correct big delays by
           duplicating or deleting a frame */
        const auto diff = ctx.vtrack->getClockVal() - ctx.atrack->getClockVal();

        /* skip or repeat frame. We take into account the
           delay to compute the threshold. I still don't know
           if it is the best guess */
        const auto sync_threshold = std::max(AV_SYNC_THRESHOLD_MIN, std::min(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < ctx.max_frame_duration) {
            if (diff <= -sync_threshold)
                delay = std::max(0.0, delay + diff);
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay = delay + diff;
            else if (diff >= sync_threshold)
                delay *= 2;
        }
    }

    return delay;
}

static double vp_duration(PlayerContext& ctx, const CAVFrame& vp, const CAVFrame& nextvp) {
    if (vp.serial() == nextvp.serial()) {
        const double duration = nextvp.ts() - vp.ts();
        if (isnan(duration) || duration <= 0 || duration > ctx.max_frame_duration)
            return vp.dur();
        else
            return duration;
    } else {
        return 0.0;
    }
}

static double video_refresh(PlayerContext& ctx)
{
    double remaining_time = REFRESH_RATE;
    while(ctx.vtrack->framesAvailable() > 0){
        const auto& lastvp = ctx.vtrack->getLastPicture();
        const auto& vp = ctx.vtrack->peekCurrentPicture();

        if (vp.serial() != ctx.vtrack->serial()) {
            ctx.vtrack->nextFrame();
            continue;
        }

        bool flush = lastvp.serial() != vp.serial();
        const double time = gettime();
        if (flush)
            ctx.frame_timer = time;

        if (ctx.paused && !flush) break;

        const auto last_duration = vp_duration(ctx, lastvp, vp);
        const auto delay = compute_target_delay(last_duration, ctx);

        if (time < ctx.frame_timer + delay) {
            remaining_time = std::min(ctx.frame_timer + delay - time, REFRESH_RATE);
            break;
        }

        ctx.frame_timer += delay;
        if (delay > 0 && time - ctx.frame_timer > AV_SYNC_THRESHOLD_MAX)
            ctx.frame_timer = time;

        if (!isnan(vp.ts()))
            ctx.vtrack->updateClock(vp.ts());

        if (ctx.vtrack->framesAvailable() > 1 && ctx.atrack && !ctx.step) {
            const auto& nextvp = ctx.vtrack->peekNextPicture();
            const auto duration = vp_duration(ctx, vp, nextvp);
            if(time > ctx.frame_timer + duration){
                ctx.frame_drops_late++;
                ctx.vtrack->nextFrame();
                continue;
            }
        }

        if (ctx.strack) {
            while (ctx.strack->subsAvailable() > 0) {
                auto sp = ctx.strack->peekCurrent();
                auto sp2 = ctx.strack->peekNext();
                const auto ref_ts = ctx.vtrack->curPts();
                if (sp->ser() != ctx.strack->serial() || (ref_ts > sp->endTime()) || (sp2 && ref_ts > sp2->startTime())) {
                    if (sp->isUploaded()) {
                            //Clear the sub texture here
                    }
                    ctx.strack->nextSub();
                } else {break;}
            }
        }

        ctx.vtrack->nextFrame();
        if (ctx.vtrack->canDisplay())
            video_image_display(ctx, ctx.vtrack->getLastPicture());
    } while(false);

    return remaining_time;
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(PlayerContext& ctx, int stream_index, FormatContext& ic)
{
    if (stream_index < 0 || stream_index >= ic.streamCount())
        return AVERROR(EINVAL);

    const CAVStream st = ic.streamAt(stream_index);
    const auto& codecpar = st.codecPar();

    switch (codecpar.codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        ctx.atrack = std::make_unique<AudioTrack>(st, ctx.continue_read_thread);
        request_ao_change(ctx, codecpar.sample_rate, codecpar.ch_layout.nb_channels);
        break;
    case AVMEDIA_TYPE_VIDEO:
        ctx.vtrack = std::make_unique<VideoTrack>(st, ctx.continue_read_thread, ctx.sdl_renderer.supportedFormats());
        ctx.queue_attachments_req = true;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        ctx.strack = std::make_unique<SubTrack>(st, ctx.continue_read_thread);
        break;
    default:
        break;
    }

    ic.setStreamEnabled(stream_index, true);

    return 0;
}

static int decode_interrupt_cb(void *opaque)
{
    const auto ctx = (PlayerContext*)opaque;
    return ctx->abort_request.load();
}

static bool demux_buffer_is_full(PlayerContext& ctx){
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

/* this thread gets the stream from the disk or the network */
void read_thread(PlayerContext& ctx)
{
    bool last_paused = false, wait_timeout = false;
    std::optional<FormatContext> ic;
    int subsequent_err_count = 0;

    try{
    ic.emplace(ctx.url.c_str(), decode_interrupt_cb, &ctx);
    } catch(std::exception& ex){
        qDebug() << "FormatContext: " << ex.what();
    } catch(...){
        qDebug() << "FormatContext: failed to initialize";
    }
    auto& fmt_ctx = *ic;

    ctx.max_frame_duration = fmt_ctx.maxFrameDuration();
    const bool realtime = fmt_ctx.isRealtime();

    {
        std::scoped_lock lck(ctx.render_mutex);
        ctx.streams = fmt_ctx.streams();
        ctx.stream_duration = fmt_ctx.duration();
        stream_component_open(ctx, fmt_ctx.audioStIdx(), fmt_ctx);
        stream_component_open(ctx, fmt_ctx.videoStIdx(), fmt_ctx);
        stream_component_open(ctx, fmt_ctx.subStIdx(), fmt_ctx);
        ctx.streams_updated = true;
    }

    if (!ctx.atrack && !ctx.vtrack) {
        return;
    }
    ctx.core.updateTitle(fmt_ctx.title());

    while (!ctx.abort_request.load()) {
        {
            std::unique_lock lck(ctx.demux_mutex);
            if (ctx.seek_req) {
                ctx.seek_req = false;
                const auto info = ctx.seek_info;
                ctx.seek_info = {};
                std::scoped_lock rlck(ctx.render_mutex);
                int64_t pos = -1;
                if (ctx.atrack)
                    pos = ctx.atrack->lastPos();
                if (pos < 0 && ctx.vtrack)
                    pos = ctx.vtrack->lastPos();
                const auto last_pts = get_master_clock(ctx);

                if(info.type == SeekInfo::SEEK_STREAM_SWITCH){
                    const auto idx = info.stream_idx;
                    if(idx >= 0 && idx < fmt_ctx.streamCount()) {
                        const auto type = fmt_ctx.streamAt(idx).type();
                        switch(type){
                        case AVMEDIA_TYPE_VIDEO:
                            if(fmt_ctx.videoStIdx() != idx){
                                stream_component_close(ctx, fmt_ctx.videoStIdx(), fmt_ctx);
                                stream_component_open(ctx, idx, fmt_ctx);
                            }
                            break;
                        case AVMEDIA_TYPE_AUDIO:
                            if(fmt_ctx.videoStIdx() != idx){
                                stream_component_close(ctx, fmt_ctx.audioStIdx(), fmt_ctx);
                                stream_component_open(ctx, idx, fmt_ctx);
                            }
                            break;
                        case AVMEDIA_TYPE_SUBTITLE:
                            if(fmt_ctx.subStIdx() != idx){
                                stream_component_close(ctx, fmt_ctx.subStIdx(), fmt_ctx);
                                stream_component_open(ctx, idx, fmt_ctx);
                            }
                            break;
                        default:
                            //This shouldn't be happening
                            break;
                        }
                    }
                } else{
                    if (fmt_ctx.seek(info, last_pts, pos)){
                        if(ctx.vtrack)
                            ctx.vtrack->flush();
                        if(ctx.atrack)
                            ctx.atrack->flush();
                        if(ctx.strack)
                            ctx.strack->flush();
                        ctx.step = true;
                    }
                }
            }

            if (wait_timeout){
                ctx.continue_read_thread.wait_for(lck, std::chrono::milliseconds(10));
                wait_timeout = false;
            }
        }

        if (ctx.paused != last_paused) {
            last_paused = ctx.paused;
            fmt_ctx.setPaused(last_paused);
        }

        if (last_paused && fmt_ctx.isRTSPorMMSH()) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            wait_timeout = true;
            continue;
        }

        if (ctx.queue_attachments_req) {
            if (ctx.vtrack && ctx.vtrack->isAttachedPic()) {
                CAVPacket pkt = fmt_ctx.attachedPic();
                ctx.vtrack->putPacket(std::move(pkt));
                ctx.vtrack->putFinalPacket(fmt_ctx.videoStIdx());
            }
            ctx.queue_attachments_req = false;
        }

        /* if the queue are full or eof was reached, no need to read more */
        if ((!realtime && demux_buffer_is_full(ctx)) || fmt_ctx.eofReached()) {
            wait_timeout = true;
        } else {
            CAVPacket pkt;
            const int ret = fmt_ctx.read(pkt);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    if (ctx.vtrack)
                        ctx.vtrack->putFinalPacket(fmt_ctx.videoStIdx());
                    if (ctx.atrack)
                        ctx.atrack->putFinalPacket(fmt_ctx.audioStIdx());
                    if (ctx.strack)
                        ctx.strack->putFinalPacket(fmt_ctx.subStIdx());
                } else if (ret == AVERROR_EXIT) {
                    break;
                }
                ++subsequent_err_count;
                if(subsequent_err_count > 1000){
                    break;
                }
            } else{
                const auto pkt_st_index = pkt.streamIndex();
                if (ctx.atrack && pkt_st_index == fmt_ctx.audioStIdx()) {
                    ctx.atrack->putPacket(std::move(pkt));
                } else if (ctx.vtrack && pkt_st_index == fmt_ctx.videoStIdx()
                           && !ctx.vtrack->isAttachedPic()) {
                    ctx.vtrack->putPacket(std::move(pkt));
                } else if (ctx.strack && pkt_st_index == fmt_ctx.subStIdx()) {
                    ctx.strack->putPacket(std::move(pkt));
                }
                subsequent_err_count = 0;
            }
        }
    }

    std::scoped_lock lck(ctx.render_mutex);
    stream_component_close(ctx, fmt_ctx.audioStIdx(), fmt_ctx);
    stream_component_close(ctx, fmt_ctx.videoStIdx(), fmt_ctx);
    stream_component_close(ctx, fmt_ctx.subStIdx(), fmt_ctx);
}

static double refresh_audio(PlayerContext& ctx){
    auto& aout = ctx.aout;
    if(aout.maybeHandleChange()){
        ctx.acvt.setOutputFmt(aout.rate(), [&aout]{CAVChannelLayout lout; lout.make_default(aout.channels()); return lout;}(),
                               AV_SAMPLE_FMT_FLT);
    }

    double remaining_time = REFRESH_RATE;

    if(ctx.atrack && !ctx.paused && aout.isOpen()){
        double buffered_time = aout.getLatency();
        if(buffered_time > SDL_AUDIO_BUFLEN){
            remaining_time = buffered_time / 4;
        } else{
            double decoded_dur = 0.0;
            do{
                const CAVFrame *af = ctx.atrack->getFrame();
                if(!af){
                    break;
                }

                auto& dst = ctx.audio_buf;
                AVFrameView aframe(*af->constAv());
                const auto wanted_nb_samples = aframe.nbSamples();

                if(ctx.acvt.convert(aframe, ctx.audio_buf, wanted_nb_samples, false)){}

                decoded_dur += (double)ctx.audio_buf.size() / aout.bitrate();
                aout.sendData(ctx.audio_buf.data(), ctx.audio_buf.size(), false);
                ctx.audio_buf.clear();
                if(!std::isnan(af->ts())){
                    buffered_time = aout.getLatency();
                    ctx.atrack->updateClock(af->ts() + af->dur() - buffered_time);
                }
            }while(decoded_dur < SDL_AUDIO_BUFLEN);
        }
    }

    return remaining_time;
}

static double refresh_video(PlayerContext& ctx){
    double remaining_time = REFRESH_RATE;
    if (ctx.vtrack)
        remaining_time = video_refresh(ctx);
    return remaining_time;
}

static void check_playback_errors(PlayerContext& ctx){
    if(ctx.vtrack){

    }

    if(ctx.atrack){

    }

    if(ctx.strack){

    }
}

static double playback_loop(PlayerContext& ctx){
    bool do_step = ctx.step && ctx.paused;
    if(do_step)
        stream_toggle_pause(ctx);
    const auto audio_remaining_time = refresh_audio(ctx);
    const auto video_remaining_time = refresh_video(ctx);
    check_playback_errors(ctx);
    if (do_step)
        stream_toggle_pause(ctx);
    ctx.step = false;

    return std::min(audio_remaining_time, video_remaining_time);
}

void PlayerCore::openURL(QUrl url){
    if(player_ctx){
        stopPlayback();
    }

    if((player_ctx = std::make_unique<PlayerContext>(url.toString().toStdString(), std::ref(*video_renderer), std::ref(*this)))){
        refreshPlayback(); //To start the refresh timer
        emit setControlsActive(true);
    }
}

void PlayerCore::stopPlayback(){
    if(player_ctx){
        player_ctx = nullptr;
        emit setControlsActive(false);
        emit resetGUI();
    }
}

void PlayerCore::togglePause(){
    if(player_ctx){
        std::scoped_lock lck(player_ctx->render_mutex);
        stream_toggle_pause(*player_ctx);
    }
}

void PlayerCore::requestSeekPercent(double percent){
    if(player_ctx){
        std::scoped_lock slck(player_ctx->demux_mutex);
        player_ctx->seek_info = {.type = SeekInfo::SEEK_PERCENT, .percent = percent};
        player_ctx->seek_req = true;
    }
}

void PlayerCore::requestSeekIncr(double incr){
    if(player_ctx){
        std::scoped_lock slck(player_ctx->demux_mutex);
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
        const auto remaining_time = playback_loop(*player_ctx) * 1000;
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
    if(player_ctx->streams_updated){
        player_ctx->streams_updated = false;
        emit sigUpdateStreams(player_ctx->streams);
    }
}

void PlayerCore::updateGUI(){
    handleStreamsUpdate();
    const auto pos = get_master_clock(*player_ctx);
    if(!std::isnan(pos)){
        emit updatePlaybackPos(pos, player_ctx->stream_duration);
    }
}

void PlayerCore::streamSwitch(int idx){
    if(player_ctx){
        std::scoped_lock lck(player_ctx->demux_mutex);
        player_ctx->seek_info = SeekInfo{.type = SeekInfo::SEEK_STREAM_SWITCH, .stream_idx = idx};
        player_ctx->seek_req = true;
    }
}

void PlayerCore::updateTitle(std::string title){
    emit setPlayerTitle(QString::fromStdString(title));
}
