#ifndef CAVPACKET_HPP
#define CAVPACKET_HPP

extern "C" {
#include <libavformat/avformat.h>
}

class CAVPacket final {
    AVPacket *pkt = nullptr;
    bool is_flush = false;

    static void copy_props(const CAVPacket& src, CAVPacket& dst);

public:
    CAVPacket();
    ~CAVPacket();

    AVPacket* av();
    const AVPacket* constAv() const;
    void unref();
    void setFlush(bool flush);
    bool isEmpty() const;
    bool isFlush() const;

    CAVPacket(const CAVPacket& src);
    CAVPacket(CAVPacket&& src);

    CAVPacket& operator=(const CAVPacket& src);
    CAVPacket& operator=(CAVPacket&& src);
};

#endif // CAVPACKET_HPP
