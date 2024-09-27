#include "cavpacket.hpp"

CAVPacket::CAVPacket() : pkt(av_packet_alloc()), pkt_serial(-1){
    if(!pkt) throw;
}

CAVPacket::~CAVPacket(){av_packet_free(&pkt);}

AVPacket* CAVPacket::av(){return pkt;}
const AVPacket* CAVPacket::constAv() const{return pkt;}
void CAVPacket::reset(){av_packet_unref(pkt); pkt_serial = -1;}
int CAVPacket::serial() const{return pkt_serial;}

void CAVPacket::copy_props(const CAVPacket& src, CAVPacket& dst){
    dst.pkt_serial = src.serial();
}

CAVPacket::CAVPacket(const CAVPacket& src) {
    pkt = av_packet_clone(src.constAv());
    copy_props(src, *this);
}

CAVPacket::CAVPacket(CAVPacket&& src) : CAVPacket(){
    av_packet_move_ref(pkt, src.av());
    copy_props(src, *this);
}

CAVPacket::CAVPacket(AVPacket* src, int serial): CAVPacket(){
    av_packet_move_ref(pkt, src);
    pkt_serial = serial;
}

CAVPacket& CAVPacket::operator=(const CAVPacket& src){
    reset();
    av_packet_ref(pkt, src.constAv());
    copy_props(src, *this);
    return *this;
}

CAVPacket& CAVPacket::operator=(CAVPacket&& src){
    reset();
    av_packet_move_ref(pkt, src.av());
    copy_props(src, *this);
    return *this;
}
