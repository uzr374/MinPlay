#include "cavframe.h"

extern "C"{
#include <libavutil/pixdesc.h>
}

CAVFrame::CAVFrame() : frame(av_frame_alloc()) {}
CAVFrame::~CAVFrame(){av_frame_free(&frame);}
CAVFrame::CAVFrame(const CAVFrame& rhs) : CAVFrame(){
    copyParams(rhs);
    ref(rhs);
}
CAVFrame::CAVFrame(CAVFrame&& rhs) : CAVFrame(){
    copyParams(rhs);
    move_ref(rhs);
}
CAVFrame& CAVFrame::operator=(const CAVFrame& rhs){
    clear();
    copyParams(rhs);
    ref(rhs);
    return *this;
}
CAVFrame& CAVFrame::operator=(CAVFrame&& rhs){
    clear();
    copyParams(rhs);
    move_ref(rhs);
    return *this;
}

void CAVFrame::copyParams(const CAVFrame& src){
    pkt_pos = src.pkt_pos;
    pts = src.pts;
    duration = src.duration;
    uploaded = false;
    ser = src.serial();
}

bool CAVFrame::create(int w, int h, AVPixelFormat fmt){
    clear();
    frame->format = fmt;
    frame->width = w;
    frame->height = h;

    if(av_frame_get_buffer(frame, 0) == 0){
        return av_frame_make_writable(frame) == 0;
    }

    return false;
}

bool CAVFrame::ensureParams(int w, int h, AVPixelFormat fmt){
    if(w != width() || h != height() || pixFmt() != fmt){
        return create(w, h, fmt);
    }
    return true;
}

int CAVFrame::serial() const{return ser;}
void CAVFrame::setSerial(int s){ser = s;}

void CAVFrame::setPktPos(int64_t pos){pkt_pos = pos;}
void CAVFrame::setTimingInfo(double new_pts, double new_duration){pts = new_pts; duration = new_duration;}
double CAVFrame::ts() const{return pts;}
double CAVFrame::dur() const{return duration;}
int64_t CAVFrame::pktPos() const{return pkt_pos;}
AVRational CAVFrame::sampleAR() const{return frame->sample_aspect_ratio;}

const AVFrame* CAVFrame::constAv() const{return frame;}
AVFrame* CAVFrame::av() {return frame;}
void CAVFrame::clear(){av_frame_unref(frame); pkt_pos = -1LL; pts = duration = 0.0; uploaded = false; ser = -1;}
bool CAVFrame::ref(const CAVFrame& src){
    const auto res = av_frame_ref(frame, src.constAv());
    return res == 0;
}
void CAVFrame::move_ref(CAVFrame& src){
    av_frame_move_ref(frame, src.av());
}
const uint8_t* const CAVFrame::constDataPlane(int idx) const{
    return frame->extended_data[idx];
}
const uint8_t* const* CAVFrame::extData() const{
    return frame->extended_data;
}
AVRational CAVFrame::timeBase() const{return frame->time_base;}
int CAVFrame::linesize(int idx) const{return frame->linesize[idx];}
int CAVFrame::width() const{return frame->width;};
int CAVFrame::height() const{return frame->height;};
int CAVFrame::chromaShiftW() const{
    auto pix_desc = getPixFmtDesc();
    return pix_desc ? pix_desc->log2_chroma_w : 0;
}
int CAVFrame::chromaShiftH() const{
    auto pix_desc = getPixFmtDesc();
    return pix_desc ? pix_desc->log2_chroma_h : 0;
}
int CAVFrame::width(int plane) const{
    return plane == 0 ? width() : AV_CEIL_RSHIFT(width(), chromaShiftW());
}
int CAVFrame::height(int plane) const{
    return plane == 0 ? height() : AV_CEIL_RSHIFT(height(), chromaShiftH());
}
AVPixelFormat CAVFrame::pixFmt() const{return static_cast<AVPixelFormat>(frame->format);};
bool CAVFrame::isInterlaced() const{return frame->flags & AV_FRAME_FLAG_INTERLACED;};
bool CAVFrame::isTopFieldFirst() const{return frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST;}
bool CAVFrame::isRGB() const{
    auto pix_desc = getPixFmtDesc();
    return pix_desc && pix_desc->flags & AV_PIX_FMT_FLAG_RGB;
}
bool CAVFrame::isGray() const{
    auto pix_desc = getPixFmtDesc();
    return pix_desc && pix_desc->nb_components == 1;
}
bool CAVFrame::isLimited() const{return colorRange() == AVCOL_RANGE_MPEG;}
bool CAVFrame::isHW() const{
    auto pix_desc = getPixFmtDesc();
    return pix_desc && pix_desc->flags & AV_PIX_FMT_FLAG_HWACCEL;
}
bool CAVFrame::flipV() const{return linesize(0) < 0;}
AVColorSpace CAVFrame::colorSpace() const{return frame->colorspace;}
AVColorRange CAVFrame::colorRange() const{return frame->color_range;}
AVColorPrimaries CAVFrame::colorPrim() const{return frame->color_primaries;};
AVColorTransferCharacteristic CAVFrame::avcolTRC() const{return frame->color_trc;};
AVChromaLocation CAVFrame::chromaLoc() const{return frame->chroma_location;};
const AVPixFmtDescriptor* CAVFrame::getPixFmtDesc() const{return av_pix_fmt_desc_get(pixFmt());}

AVSampleFormat CAVFrame::sampleFmt() const{return static_cast<AVSampleFormat>(frame->format);};
int CAVFrame::sampleRate() const{return frame->sample_rate;};
const AVChannelLayout& CAVFrame::chLayout() const{return frame->ch_layout;};
int CAVFrame::chCount() const{return frame->ch_layout.nb_channels;};
int CAVFrame::nbSamples() const{return frame->nb_samples;};
bool CAVFrame::videoFrameValid() const{return width()  > 0 && height() > 0 && pixFmt() != AV_PIX_FMT_NONE;}
bool CAVFrame::audioFrameValid() const{return nbSamples() > 0 && sampleFmt() != AV_SAMPLE_FMT_NONE;}
bool CAVFrame::isUploaded() const{return uploaded;}
void CAVFrame::setUploaded(bool upl){uploaded = upl;}
