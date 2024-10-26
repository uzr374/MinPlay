#include "PacketQueue.hpp"

bool PacketQueue::put(CAVPacket&& pkt)
{
    auto lck = getLocker();
    ++params.nb_packets;
    params.size += pkt.constAv()->size + sizeof(AVPacket);
    params.duration += pkt.constAv()->duration;
    pkt_list.push(std::move(CAVPacket(pkt)));

    return true;
}

bool PacketQueue::put_nullpacket()
{
    CAVPacket pkt;
    pkt.setFlush(true);
    return put(std::move(pkt));
}

PktQParams PacketQueue::getParams() {
    auto lck = getLocker();
    return params;
}

/*One should acquire the lock by calling getLocker() before calling this function*/
void PacketQueue::flush()
{
    pkt_list = std::queue<CAVPacket>();
    params = PktQParams();
}

bool PacketQueue::get(CAVPacket& dst)
{
    if (!pkt_list.empty()) {
        dst = std::move(pkt_list.front());
        pkt_list.pop();
        --params.nb_packets;
        params.size -= dst.constAv()->size + sizeof(AVPacket);
        params.duration -= dst.constAv()->duration;
        return true;
    }
    return false;
}
