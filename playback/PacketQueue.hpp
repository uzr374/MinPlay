#ifndef PACKETQUEUE_HPP
#define PACKETQUEUE_HPP

#include "cavpacket.hpp"

#include <queue>
#include <mutex>
#include <condition_variable>

struct PacketQueue {
    std::queue<CAVPacket> pkt_list;
    int nb_packets = 0;
    int size = 0;
    int64_t duration = 0;
    bool abort_request = false;
    int serial = -1;
    std::mutex mutex;
    std::condition_variable cond;

    PacketQueue();
    ~PacketQueue() = default;

    int put_private(AVPacket *pkt);
    int put(AVPacket *pkt);
    int put_nullpacket(int stream_index);
    void flush();
    void destroy();
    void abort();
    void start();
    /* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
    int get(AVPacket *pkt, int block, int *serial);
};

#endif // PACKETQUEUE_HPP
