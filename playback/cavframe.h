#ifndef CAVFRAME_H
#define CAVFRAME_H

extern "C"{
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

class CAVFrame
{
private:
    AVFrame* frame = nullptr;
    int64_t pkt_pos = -1LL;
    double pts = 0.0, duration = 0.0;
    bool uploaded = false;
    int ser = -1;

    void copyParams(const CAVFrame& src);
    const AVPixFmtDescriptor* getPixFmtDesc() const;
public:
    CAVFrame();
    ~CAVFrame();
    CAVFrame(const CAVFrame&);
    CAVFrame(CAVFrame&&);

    const AVFrame* constAv() const;
    AVFrame* av();
    void clear();
    bool ref(const CAVFrame& src);
    void move_ref(CAVFrame& src);
    CAVFrame& operator=(const CAVFrame&);
    CAVFrame& operator=(CAVFrame&&);

    void setPktPos(int64_t pos);
    void setTimingInfo(double new_pts, double new_duration);
    bool create(int w, int h, AVPixelFormat fmt);
    bool ensureParams(int w, int h, AVPixelFormat fmt);

    int serial() const;
    void setSerial(int s);

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
    bool isUploaded() const;
    void setUploaded(bool upl);

    //Audio-related
    AVSampleFormat sampleFmt() const;
    int sampleRate() const;
    const AVChannelLayout& chLayout() const;
    int chCount() const;
    int nbSamples() const;
    bool audioFrameValid() const;
};

#endif // CAVFRAME_H
