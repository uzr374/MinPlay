#include "formatcontext.hpp"
#include "playbackengine.hpp"

#include <stdexcept>

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

FormatContext::FormatContext(QString url, decltype(AVFormatContext::interrupt_callback.callback) int_cb, void* cb_opaque) : ic(avformat_alloc_context()) {
    ic->interrupt_callback.callback = int_cb;
    ic->interrupt_callback.opaque = cb_opaque;
    ic->flags |= AVFMT_FLAG_GENPTS | AVFMT_FLAG_FAST_SEEK | AVFMT_FLAG_DISCARD_CORRUPT;
    ic->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    AVDictionary* format_opts = nullptr;
    av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
    auto err = avformat_open_input(&ic, url.toStdString().c_str(), nullptr, &format_opts);
    av_dict_free(&format_opts);
    if (err < 0) {
        throw std::runtime_error(std::string("Failed to open url: ") + url.toStdString());
    }
    dynamic_streams = (ic->ctx_flags & AVFMTCTX_NOHEADER);
    seekable = (ic->ctx_flags & AVFMTCTX_UNSEEKABLE);
    if (avformat_find_stream_info(ic, nullptr) < 0) {
        const auto errmsg = "Could not find codec parameters";
        if (!dynamic_streams)
            throw std::runtime_error(errmsg);
    }
    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

    seek_by_bytes = !(ic->iformat->flags & AVFMT_NO_BYTE_SEEK) && !!(ic->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", ic->iformat->name);
    max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;
    duration_s = (ic->duration == AV_NOPTS_VALUE || ic->duration <= 0) ? NAN : double(ic->duration) / AV_TIME_BASE;
    realtime = is_realtime(ic);
    rtsp_or_mmsh = !strcmp(ic->iformat->name, "rtsp") || (ic->pb && !strncmp(ic->url, "mmsh:", 5));

    cstreams.reserve(ic->nb_streams);
    for (int i = 0; i < ic->nb_streams; i++) {
        cstreams.push_back(CAVStream(ic, i));
        ic->streams[i]->discard = AVDISCARD_ALL;
    }

    video_idx = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    audio_idx = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, video_idx, NULL, 0);
    sub_idx = av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE, -1, (audio_idx >= 0 ? audio_idx : video_idx), NULL, 0);

    AVDictionaryEntry* t = nullptr;
    if ((t = av_dict_get(ic->metadata, "title", NULL, 0)))
        title = QString::asprintf("%s - %s", t->value, ic->url);
    else
        title = ic->url;
}

FormatContext::~FormatContext(){
    if(ic){
        avformat_close_input(&ic);
    }
}

int64_t FormatContext::size() const {
    int64_t res = -1LL;
    if(!isRealtime() && ic->pb)
        res = avio_size(ic->pb);
    return res;
}

bool FormatContext::seek(const SeekInfo& info, double last_pts, int64_t last_pos){
    auto set_seek = [&](int64_t pos, int64_t rel)
    {
        last_seek_pos = pos;
        last_seek_rel = rel;
    };

    if(ic->ctx_flags & AVFMTCTX_UNSEEKABLE){
        return true;//Only return false if avformat_seek_file() failed
    }

    switch(info.type){
    case SeekInfo::SEEK_PERCENT:
        if (seek_by_bytes || ic->duration <= 0) {
            const int64_t byte_size = size();
            if (byte_size > 0)
                set_seek(byte_size * info.percent, 0);
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
            set_seek(ts, 0);
        }
        break;
    case SeekInfo::SEEK_INCREMENT:
    {
        double incr = info.increment;
        if (seek_by_bytes) {
            int64_t pos = last_pos;
            if (pos < 0 && ic->pb)
                pos = avio_tell(ic->pb);
            if (ic->bit_rate)
                incr *= ic->bit_rate / 8.0;//get the byte size
            else
                incr *= 180000.0;//not sure why
            pos += incr;
            set_seek(pos, incr);
        } else {
            double pos = last_pts;
            if (isnan(pos))
                pos = (double)last_seek_pos / AV_TIME_BASE;
            pos += incr;
            if (ic->start_time != AV_NOPTS_VALUE && pos < ic->start_time / (double)AV_TIME_BASE)
                pos = ic->start_time / (double)AV_TIME_BASE;
            set_seek((int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE));
        }
    }
    break;
    default:
        return false;
    }

    /*Execute the seek*/
    // FIXME the +-2 is due to rounding being not done in the correct direction in generation
    //      of the seek_pos/seek_rel variables
    const int64_t seek_target = last_seek_pos;
    const int64_t seek_min    = last_seek_rel > 0 ? seek_target - last_seek_rel + 2: INT64_MIN;
    const int64_t seek_max    = last_seek_rel < 0 ? seek_target - last_seek_rel - 2: INT64_MAX;

    const auto seek_flags = seek_by_bytes ? AVSEEK_FLAG_BYTE : 0;
    const auto seekRes = avformat_seek_file(ic, -1, seek_min, seek_target, seek_max, seek_flags);
    const auto seek_succeeded = seekRes >= 0;
    if(seek_succeeded)
        eof = false;

    return seek_succeeded;
}

int FormatContext::read(CAVPacket& into){
    const auto readRes = av_read_frame(ic, into.av());
    if (readRes < 0) {
        if ((readRes == AVERROR_EOF) || ic->pb && avio_feof(ic->pb)) {
            eof = true;
            return AVERROR_EOF;
        }
        if (ic->pb && ic->pb->error) {
            return AVERROR_EXIT;
        }
    } else {
        eof = false;
        into.setTb(ic->streams[into.av()->stream_index]->time_base);
    }
    return readRes;
}

bool FormatContext::setStreamEnabled(int s_idx, bool enable){
    if(s_idx < 0 || s_idx >= streamCount())
        return false;
    auto st = ic->streams[s_idx];
    st->discard = enable ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    eof = false;
    switch(st->codecpar->codec_type){
    case AVMEDIA_TYPE_VIDEO:
        if(enable){
            video_idx = s_idx;
        } else{
            video_last_idx = video_idx;
            video_idx = -1;
        }
        break;
    case AVMEDIA_TYPE_AUDIO:
        if(enable){
            audio_idx = s_idx;
        } else{
            audio_last_idx = audio_idx;
            audio_idx = -1;
        }
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        if(enable){
            sub_idx = s_idx;
        } else{
            sub_last_idx = sub_idx;
            sub_idx = -1;
        }
        break;
    default:
        break;
    }

    return true;
}

int FormatContext::setPaused(bool paused){
    if(paused != local_paused){
        local_paused = paused;
        return paused ? av_read_pause(ic) : av_read_play(ic);
    }
    return 0;
}

const std::vector<CAVStream>& FormatContext::streams() const{return cstreams;}
int FormatContext::streamCount() const{return ic->nb_streams;}
bool FormatContext::isRealtime() const{return realtime;}
bool FormatContext::byteSeek() const{return seek_by_bytes;}
double FormatContext::maxFrameDuration() const{return max_frame_duration;}
double FormatContext::duration() const{return duration_s;}
int FormatContext::videoStIdx() const{return video_idx;}
int FormatContext::audioStIdx() const{return audio_idx;}
int FormatContext::subStIdx() const{return sub_idx;}
bool FormatContext::eofReached() const{return eof;}
bool FormatContext::isRTSPorMMSH() const{return rtsp_or_mmsh;}
