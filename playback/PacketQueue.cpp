#include "PacketQueue.hpp"

PacketQueue::PacketQueue(): abort_request(true){}

int PacketQueue::put_private(AVPacket *pkt)
{
    if (abort_request)
        return -1;

    ++nb_packets;
    size += pkt->size + sizeof(AVPacket);
    duration += pkt->duration;

    pkt_list.push(CAVPacket(pkt, serial));

    /* XXX: should duplicate packet data in DV case */
    cond.notify_one();
    return 0;
}

int PacketQueue::put(AVPacket *pkt)
{
    std::unique_lock lck(mutex);
    const int ret = put_private(pkt);

    return ret;
}

int PacketQueue::put_nullpacket(int stream_index)
{
    CAVPacket pkt;
    pkt.av()->stream_index = stream_index;
    return put(pkt.av());
}

void PacketQueue::flush()
{
    std::unique_lock lck(mutex);
    pkt_list = std::queue<CAVPacket>();
    nb_packets = 0;
    size = 0;
    duration = 0;
    serial++;
}

void PacketQueue::destroy()
{
    flush();
}

void PacketQueue::abort()
{
    std::unique_lock lck(mutex);
    abort_request = 1;
    cond.notify_one();
}

void PacketQueue::start()
{
    std::unique_lock lck(mutex);
    abort_request = 0;
    serial++;
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
int PacketQueue::get(AVPacket *pkt, int block, int *serial)
{
    int ret;
    std::unique_lock lck(mutex);
    for (;;) {
        if (abort_request) {
            ret = -1;
            break;
        }

        if (!pkt_list.empty()) {
            auto pkt1 = pkt_list.front();
            pkt_list.pop();
            av_packet_move_ref(pkt, pkt1.pkt);
            nb_packets--;
            size -= pkt->size + sizeof(AVPacket);
            duration -= pkt->duration;
            if (serial)
                *serial = pkt1.serial();
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            cond.wait(lck);
        }
    }
    return ret;
}
