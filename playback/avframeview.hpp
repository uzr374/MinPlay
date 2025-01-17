#ifndef AVFRAMEVIEW_HPP
#define AVFRAMEVIEW_HPP

extern "C"{
#include "libavutil/frame.h"
#include "libavutil/pixdesc.h"
}

class AVFrameView
{
private:
    const AVFrame& frame;

public:
    AVFrameView() = delete;
    AVFrameView(const AVFrame&);

    //Common fields
    const uint8_t* const constDataPlane(int idx) const;
    const uint8_t* const* extData() const;
    int linesize(int idx) const;
    double ts() const;
    double dur() const;
    int64_t pktPos() const;
    AVRational sampleAR() const;

    //Video-specific
    int width() const;
    int height() const;
    AVPixelFormat pixFmt() const;
    bool isInterlaced() const;
    bool isTopFieldFirst() const;
    bool isRGB() const;
    bool isGray() const;
    bool isLimited() const;
    bool isHW() const;
    bool flipV() const;
    AVColorSpace colorSpace() const;
    AVColorRange colorRange() const;
    AVColorPrimaries colorPrim() const;
    AVColorTransferCharacteristic avcolTRC() const;
    AVChromaLocation chromaLoc() const;
    AVRational timeBase() const;
    int chromaShiftW() const;
    int chromaShiftH() const;
    int width(int plane) const;
    int height(int plane) const;
    bool videoFrameValid() const;
    const AVPixFmtDescriptor* getPixFmtDesc() const;

    //Audio-related
    AVSampleFormat sampleFmt() const;
    int sampleRate() const;
    const AVChannelLayout& chLayout() const;
    int chCount() const;
    int nbSamples() const;
    bool audioFrameValid() const;
};

#endif // AVFRAMEVIEW_HPP
