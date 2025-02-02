#include "packetqueue.hpp"

PacketQueue::PacketQueue() {}

bool PacketQueue::put(CAVPacket&& pkt)
{
    std::unique_lock lck(mutex);
    if(int_abort_req)
        return false;

    pkt.setSerial(int_serial);
    const double dur = pkt.dur();
    const auto size = pkt.size();
    try{
        pkt_buf.push(std::move(pkt));
    } catch(...){
        return false;
    }

    duration_s += dur;
    byte_size += size;
    ++nb_packets;
    cond.notify_one();

    return true;
}

bool PacketQueue::put_nullpacket(int stream_index)
{
    CAVPacket pkt;
    pkt.av()->stream_index = stream_index;
    return put(std::move(pkt));
}

void PacketQueue::flush()
{
    std::scoped_lock lck(mutex);
    pkt_buf = {};
    nb_packets = 0;
    byte_size = 0;
    duration_s = 0.0;
    ++ext_serial;
    ++int_serial;
}

void PacketQueue::abort()
{
    std::scoped_lock lck(mutex);
    ext_abort_req = int_abort_req = true;
    cond.notify_one();
}

void PacketQueue::start()
{
    std::scoped_lock lck(mutex);
    ext_abort_req = int_abort_req = false;
    ++ext_serial;
    ++int_serial;
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
int PacketQueue::get(CAVPacket& dst, bool block)
{
    std::unique_lock lck(mutex);

    int ret = 0;
    for (;;) {
        if (int_abort_req) {
            ret = -1;
            break;
        }

        if (!pkt_buf.empty()) {
            CAVPacket pkt = std::move(pkt_buf.front());
            pkt_buf.pop();
            --nb_packets;
            byte_size -= pkt.size();
            duration_s -= pkt.dur();
            dst = std::move(pkt);
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

std::tuple<int, int, double> PacketQueue::getParams(){
    std::scoped_lock lck(mutex);
    return {byte_size, nb_packets, duration_s};
}

int PacketQueue::serial() const{
    return ext_serial.load();
}

bool PacketQueue::isAborted() const{
    return ext_abort_req.load();
}

bool PacketQueue::isEmpty() const{
    std::scoped_lock lck(mutex);
    return pkt_buf.empty();
}

int PacketQueue::size() const{
    return byte_size;
}
