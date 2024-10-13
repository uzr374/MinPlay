#ifndef PACKETQUEUE_HPP
#define PACKETQUEUE_HPP

#include "cavpacket.hpp"

#include <queue>
#include <mutex>

struct PktQParams{
    int nb_packets = 0;
    int size = 0;
    int64_t duration = 0;
};

class PacketQueue final {
    std::queue<CAVPacket> pkt_list;
    std::mutex mutex;
    PktQParams params;

public:
    bool flush_req = false;

public:
    PacketQueue() = default;
    ~PacketQueue() = default;

    bool put(CAVPacket&& pkt);
    bool put_nullpacket();
    void flush();
    bool get(CAVPacket& dst);
    PktQParams getParams();
    inline std::unique_lock<std::mutex> getLocker(){return std::unique_lock(mutex);}
    inline bool isEmpty() const{return pkt_list.empty();}
};

#endif // PACKETQUEUE_HPP
