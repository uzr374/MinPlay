#ifndef CAVPACKET_HPP
#define CAVPACKET_HPP

extern "C" {
#include <libavformat/avformat.h>
}

struct CAVPacket {
    AVPacket *pkt = nullptr;
    int pkt_serial = -1;

    CAVPacket();
    ~CAVPacket();

    AVPacket* av();
    const AVPacket* constAv() const;
    void reset();
    int serial() const;

    static void copy_props(const CAVPacket& src, CAVPacket& dst);

    CAVPacket(const CAVPacket& src);
    CAVPacket(CAVPacket&& src);
    CAVPacket(AVPacket* src, int serial);
    CAVPacket& operator=(const CAVPacket& src);
    CAVPacket& operator=(CAVPacket&& src);
};

#endif // CAVPACKET_HPP
