#include "cavpacket.hpp"

CAVPacket::CAVPacket() : pkt(av_packet_alloc()){
    if(!pkt) throw;
}

CAVPacket::~CAVPacket(){av_packet_free(&pkt);}

AVPacket* CAVPacket::av(){return pkt;}
const AVPacket* CAVPacket::constAv() const{return pkt;}
void CAVPacket::unref(){av_packet_unref(pkt); is_flush = false;}

void CAVPacket::copy_props(const CAVPacket& src, CAVPacket& dst){
    dst.is_flush = src.isFlush();
}

CAVPacket::CAVPacket(const CAVPacket& src) : CAVPacket() {
    av_packet_ref(pkt, src.constAv());
    copy_props(src, *this);
}

CAVPacket::CAVPacket(CAVPacket&& src) : CAVPacket(){
    av_packet_move_ref(pkt, src.av());
    copy_props(src, *this);
}

CAVPacket& CAVPacket::operator=(const CAVPacket& src){
    unref();
    av_packet_ref(pkt, src.constAv());
    copy_props(src, *this);
    return *this;
}

CAVPacket& CAVPacket::operator=(CAVPacket&& src){
    unref();
    av_packet_move_ref(pkt, src.av());
    copy_props(src, *this);
    return *this;
}

bool CAVPacket::isEmpty() const{
    return pkt->size <= 0;
}

bool CAVPacket::isFlush() const{
    return is_flush;
}

void CAVPacket::setFlush(bool flush){unref(); is_flush = flush;}
