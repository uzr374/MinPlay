#ifndef PACKETQUEUE_HPP
#define PACKETQUEUE_HPP

#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include <QtGlobal>

#include "cavpacket.hpp"

class PacketQueue
{
    Q_DISABLE_COPY_MOVE(PacketQueue);
private:
    std::queue<CAVPacket> pkt_buf;
    double duration_s = 0.0;
    int byte_size = 0, nb_packets = 0;
    int int_serial = -1;
    std::atomic<int> ext_serial = int_serial;
    bool int_abort_req = true;
    std::atomic_bool ext_abort_req = int_abort_req;

    mutable std::mutex mutex;
    std::condition_variable cond;

public:
    PacketQueue();

    bool put(CAVPacket&& pkt);
    bool put_nullpacket(int stream_index);
    void flush();
    void abort();
    void start();
    /* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
    int get(CAVPacket& dst, bool block);
    /*returns the size, number of packets stored and duration of the queue*/
    std::tuple<int, int, double> getParams();
    int serial() const;
    bool isAborted() const;
    bool isEmpty() const;
    int size() const;
};

#endif // PACKETQUEUE_HPP
