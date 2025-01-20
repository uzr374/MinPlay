#include "cavstream.hpp"

#include <stdexcept>
#include <sstream>

CAVStream::CAVStream(): codecpar(avcodec_parameters_alloc()) {

}

CAVStream::~CAVStream(){
    avcodec_parameters_free(&codecpar);
}

CAVStream::CAVStream(AVFormatContext* ctx, int stream_index): CAVStream(){
    if (stream_index < 0 || stream_index >= ctx->nb_streams) {
        avcodec_parameters_free(&codecpar);
        throw std::runtime_error("CAVStream:: index out of range");
    }
    const auto st = ctx->streams[stream_index];
    index = stream_index;
    avcodec_parameters_copy(codecpar, st->codecpar);
    if(isVideo()){
        is_attached_pic = (st->disposition & AV_DISPOSITION_ATTACHED_PIC);
        if(codecpar->framerate.num > 0){
            frame_rate = codecpar->framerate;
        } else{
            frame_rate = av_guess_frame_rate(ctx, st, nullptr);
        }
        sample_ar = codecpar->sample_aspect_ratio;
        if(sample_ar.num == 0) sample_ar = {1, 1};//Just assume that the SAR is 1:1 if undefined
    }
    time_base = st->time_base;
    start_time = st->start_time;
    stream_duration = st->duration;

    const auto lang_tag = av_dict_get(st->metadata, "language", nullptr, 0);
    if(lang_tag && lang_tag->value){
        lang_str = lang_tag->value;
    }

    std::ostringstream ss;
    if(!lang_str.empty())
        ss << "[" << lang_str << "] ";

    if(isVideo()){
        ss << width() << "x" << height();
    } else if(isAudio()){
        ss << chLayoutStr() << ", " << sampleRate() << " Hz";
    }

    title_str = ss.str();
}

CAVStream::CAVStream(const CAVStream& rhs): CAVStream(){
    copyFrom(rhs);
}

CAVStream::CAVStream(CAVStream&& rhs): CAVStream(){
    copyFrom(rhs);
}

CAVStream& CAVStream::operator=(const CAVStream& rhs){
    clear();
    copyFrom(rhs);
    return *this;
}

CAVStream& CAVStream::operator=(CAVStream&& rhs){
    clear();
    copyFrom(rhs);
    return *this;
}

void CAVStream::clear(){
    avcodec_parameters_free(&codecpar);
    codecpar = avcodec_parameters_alloc();
    frame_rate = sample_ar = time_base ={};
    is_attached_pic = false;
    start_time = AV_NOPTS_VALUE;
    stream_duration = AV_NOPTS_VALUE;
    index = -1;
}

void CAVStream::copyFrom(const CAVStream& rhs){
    avcodec_parameters_copy(codecpar, &rhs.codecPar());
    sample_ar = rhs.sampleAR();
    frame_rate = rhs.frameRate();
    time_base = rhs.tb();
    is_attached_pic = rhs.isAttachedPic();
    start_time = rhs.startTime();
    stream_duration = rhs.duration();
    index = rhs.idx();
    title_str = rhs.titleStr();
    lang_str = rhs.langStr();
}

const AVCodec* CAVStream::getCodec() const{
    return avcodec_find_decoder(codecpar->codec_id);
}

/*The reference is invalidated if the stream is reset or otherwise rewritten*/
const AVCodecParameters& CAVStream::codecPar() const{
    return *codecpar;
}
/*A bunch of handy getters*/
AVRational CAVStream::frameRate() const{return frame_rate;}
AVRational CAVStream::sampleAR() const{return sample_ar;}
AVRational CAVStream::tb() const {return time_base;}
bool CAVStream::isAttachedPic() const{return is_attached_pic;}
bool CAVStream::isVideo() const{return type() == AVMEDIA_TYPE_VIDEO;}
bool CAVStream::isAudio() const{return type() == AVMEDIA_TYPE_AUDIO;}
bool CAVStream::isSub() const{return type() == AVMEDIA_TYPE_SUBTITLE;}
AVMediaType CAVStream::type() const{return codecPar().codec_type;}
int64_t CAVStream::startTime() const {return start_time;}
int64_t CAVStream::duration() const{return stream_duration;}
int CAVStream::idx() const{return index;}
std::string CAVStream::titleStr() const{
    return title_str;
}
std::string CAVStream::langStr() const{return lang_str;}
int CAVStream::width() const{return codecpar->width;}
int CAVStream::height() const{return codecpar->height;}
int CAVStream::sampleRate() const{return codecpar->sample_rate;}
int CAVStream::channelCount() const{return codecpar->ch_layout.nb_channels;}
std::string CAVStream::chLayoutStr() const{
    char buf[64]{};
    av_channel_layout_describe(&codecpar->ch_layout, buf, sizeof(buf));
    return std::string(buf);
}

