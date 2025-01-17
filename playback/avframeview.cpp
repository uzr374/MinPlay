#include "avframeview.hpp"

AVFrameView::AVFrameView(const AVFrame& fr) : frame(fr) {}

const uint8_t* const AVFrameView::constDataPlane(int idx) const{
    return frame.extended_data[idx];
}
const uint8_t* const* AVFrameView::extData() const{
    return frame.extended_data;
}
AVRational AVFrameView::timeBase() const{return frame.time_base;}
int AVFrameView::linesize(int idx) const{return frame.linesize[idx];}
int AVFrameView::width() const{return frame.width;};
int AVFrameView::height() const{return frame.height;};
int AVFrameView::chromaShiftW() const{
    auto pix_desc = getPixFmtDesc();
    return pix_desc ? pix_desc->log2_chroma_w : 0;
}
int AVFrameView::chromaShiftH() const{
    auto pix_desc = getPixFmtDesc();
    return pix_desc ? pix_desc->log2_chroma_h : 0;
}
int AVFrameView::width(int plane) const{
    return plane == 0 ? width() : AV_CEIL_RSHIFT(width(), chromaShiftW());
}
int AVFrameView::height(int plane) const{
    return plane == 0 ? height() : AV_CEIL_RSHIFT(height(), chromaShiftH());
}
AVPixelFormat AVFrameView::pixFmt() const{return static_cast<AVPixelFormat>(frame.format);};
bool AVFrameView::isInterlaced() const{return frame.flags & AV_FRAME_FLAG_INTERLACED;};
bool AVFrameView::isTopFieldFirst() const{return frame.flags & AV_FRAME_FLAG_TOP_FIELD_FIRST;}
bool AVFrameView::isRGB() const{
    auto pix_desc = getPixFmtDesc();
    return pix_desc && pix_desc->flags & AV_PIX_FMT_FLAG_RGB;
}
bool AVFrameView::isGray() const{
    auto pix_desc = getPixFmtDesc();
    return pix_desc && pix_desc->nb_components == 1;
}
bool AVFrameView::isLimited() const{return colorRange() == AVCOL_RANGE_MPEG;}
bool AVFrameView::isHW() const{
    auto pix_desc = getPixFmtDesc();
    return pix_desc && pix_desc->flags & AV_PIX_FMT_FLAG_HWACCEL;
}
bool AVFrameView::flipV() const{return linesize(0) < 0;}
AVColorSpace AVFrameView::colorSpace() const{return frame.colorspace;}
AVColorRange AVFrameView::colorRange() const{return frame.color_range;}
AVColorPrimaries AVFrameView::colorPrim() const{return frame.color_primaries;};
AVColorTransferCharacteristic AVFrameView::avcolTRC() const{return frame.color_trc;};
AVChromaLocation AVFrameView::chromaLoc() const{return frame.chroma_location;};
const AVPixFmtDescriptor* AVFrameView::getPixFmtDesc() const{return av_pix_fmt_desc_get(pixFmt());}
AVRational AVFrameView::sampleAR() const{return frame.sample_aspect_ratio;}

AVSampleFormat AVFrameView::sampleFmt() const{return static_cast<AVSampleFormat>(frame.format);};
int AVFrameView::sampleRate() const{return frame.sample_rate;};
const AVChannelLayout& AVFrameView::chLayout() const{return frame.ch_layout;};
int AVFrameView::chCount() const{return frame.ch_layout.nb_channels;};
int AVFrameView::nbSamples() const{return frame.nb_samples;};
bool AVFrameView::videoFrameValid() const{return width()  > 0 && height() > 0 && pixFmt() != AV_PIX_FMT_NONE;}
bool AVFrameView::audioFrameValid() const{return nbSamples() > 0 && sampleFmt() != AV_SAMPLE_FMT_NONE;}


