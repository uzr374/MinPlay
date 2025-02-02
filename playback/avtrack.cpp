#include "avtrack.hpp"

AVTrack::AVTrack(const CAVStream& st, std::condition_variable& empty_q_cond) : dec(st, pkts, empty_q_cond), rel_st(st) {
    pkts.start();
}

AVTrack::~AVTrack(){
    pkts.flush();
    dec.destroy();
}

void AVTrack::flush(){
    pkts.flush();
}

std::tuple<int, int, double> AVTrack::getQueueParams(){return pkts.getParams();}
int AVTrack::serial() {return pkts.serial();}

void AVTrack::putPacket(CAVPacket&& pkt){
    pkts.put(std::move(pkt));
}

void AVTrack::putFinalPacket(int st_idx){
    pkts.put_nullpacket(st_idx);
}
