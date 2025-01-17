#ifndef CAVPACKET_HPP
#define CAVPACKET_HPP

extern "C" {
#include <libavformat/avformat.h>
}

class CAVPacket final {
    AVPacket *pkt = nullptr;
    bool is_flush = false;
    AVRational tb{};

    static void copy_props(const CAVPacket& src, CAVPacket& dst);

public:
    CAVPacket();
    ~CAVPacket();

    AVPacket* av();
    const AVPacket* constAv() const;
    void unref();
    void setFlush(bool flush);
    void setTb(AVRational src_tb);
    bool isEmpty() const;
    bool isFlush() const;
    int size() const;
    double dur() const;

    CAVPacket(const CAVPacket& src);
    CAVPacket(CAVPacket&& src);

    CAVPacket& operator=(const CAVPacket& src);
    CAVPacket& operator=(CAVPacket&& src);
};

#endif // CAVPACKET_HPP
