#include "formatcontext.hpp"

#include <stdexcept>
#include <QUrl>

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

FormatContext::FormatContext(std::string url, decltype(AVFormatContext::interrupt_callback.callback) int_cb, void* cb_opaque) : ic(avformat_alloc_context()) {
    ic->interrupt_callback.callback = int_cb;
    ic->interrupt_callback.opaque = cb_opaque;
    ic->flags |= AVFMT_FLAG_GENPTS | AVFMT_FLAG_DISCARD_CORRUPT;
    ic->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    AVDictionary* format_opts = nullptr;
    av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
    auto err = avformat_open_input(&ic, url.c_str(), nullptr, &format_opts);
    av_dict_free(&format_opts);
    if (err < 0) {
        throw std::runtime_error(std::string("Failed to open url: ") + url);
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
    if ((t = av_dict_get(ic->metadata, "title", nullptr, 0))){
        stream_title = QString::asprintf("%s - %s", t->value, ic->url).toStdString();
    }
    else{
        QUrl url = isRealtime() ? QString(ic->url) : QUrl::fromLocalFile(ic->url);
        stream_title = url.isLocalFile() ? url.fileName().toStdString() : ic->url;
    }
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

    auto seek_incr = [&](double incr){
        if (seek_by_bytes) {
            int64_t pos = last_pos;
            if (pos < 0)
                pos = bytePos();
            if (bitrate())
                incr *= bitrate() / 8.0;
            else
                incr *= 180000.0;
            pos += incr;
            set_seek(pos, incr);
        } else {
            auto pos = last_pts;
            if (isnan(pos))
                pos = (double)last_seek_pos / AV_TIME_BASE;
            pos += incr;
            if (startTime() != AV_NOPTS_VALUE && pos < startTime() / (double)AV_TIME_BASE)
                pos = startTime() / (double)AV_TIME_BASE;
            set_seek((int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE));
        }
    };

    bool seek_in_stream = false;
    const bool unseekable = (ic->flags & AVFMTCTX_UNSEEKABLE);
    if(unseekable && info.type != SeekInfo::SEEK_STREAM_SWITCH)
        return false;
    switch(info.type){
    case SeekInfo::SEEK_PERCENT:
    {
        const auto pcent = info.percent;
        if (seek_by_bytes || ic->duration <= 0) {
            const uint64_t size =  avio_size(ic->pb);
            set_seek(size*pcent, 0);
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
            set_seek(ts, 0);
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
            const int64_t pos = last_pts * AV_TIME_BASE;

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
                                      AV_TIME_BASE_Q), 0);
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

    bool seek_succeeded = !seek_in_stream;
    if(seek_in_stream){
        /*Execute the seek*/
        // FIXME the +-2 is due to rounding being not done in the correct direction in generation
        //      of the seek_pos/seek_rel variables
        const int64_t seek_target = last_seek_pos;
        const int64_t seek_min    = last_seek_rel > 0 ? seek_target - last_seek_rel + 2: INT64_MIN;
        const int64_t seek_max    = last_seek_rel < 0 ? seek_target - last_seek_rel - 2: INT64_MAX;

        const auto seek_flags = seek_by_bytes ? AVSEEK_FLAG_BYTE : 0;
        const auto seekRes = avformat_seek_file(ic, -1, seek_min, seek_target, seek_max, seek_flags);
        seek_succeeded = seekRes >= 0;
        if(seek_succeeded)
            eof = false;
    }

    return seek_succeeded;
}

int FormatContext::read(CAVPacket& into){
    const auto readRes = av_read_frame(ic, into.av());
    if (readRes < 0) {
        if ((readRes == AVERROR_EOF) || ic->pb && avio_feof(ic->pb)) {
            if(!eof){
                eof = true;
                return AVERROR_EOF;
            } else{
                return AVERROR_BUG;
            }
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

CAVStream FormatContext::streamAt(int idx) const{return cstreams.at(idx);}
std::vector<CAVStream> FormatContext::streams() const{return cstreams;}
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
int64_t FormatContext::bytePos() const{
    int64_t pos = -1LL;
    if(ic->pb)
        pos = avio_tell(ic->pb);
    return pos;
}
int64_t FormatContext::bitrate() const{return ic->bit_rate;}
int64_t FormatContext::startTime() const{return ic->start_time;}
CAVPacket FormatContext::attachedPic() const{
    CAVPacket pkt;
    if(video_idx >= 0 && streamAt(video_idx).isAttachedPic()){
        auto st = ic->streams[video_idx];
        av_packet_ref(pkt.av(), &st->attached_pic);
        pkt.setTb(st->time_base);
    }
    return pkt;
}
std::string FormatContext::title() const{return stream_title;}
