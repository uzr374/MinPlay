#include "subtrack.hpp"

SubTrack::SubTrack(const CAVStream& st, std::condition_variable& cond) : AVTrack(st, cond), sub_pool(pkts, SUBPICTURE_QUEUE_SIZE, 0) {
    dec.decoder_thr = std::thread(&SubTrack::run, this);
}

SubTrack::~SubTrack(){
    flush();
    pkts.abort();
    sub_pool.notify();
    if(dec.decoder_thr.joinable())
        dec.decoder_thr.join();
}

void SubTrack::run() {
    for (;;) {
        auto sp = sub_pool.peek_writable();
        if (!sp)
            break;
        AVSubtitle& sub = sp->av();
        const auto got_subtitle = dec.decode_frame(nullptr, &sub);
        if (got_subtitle < 0)
            break;
        if (got_subtitle) {
            sp->ensureDimensions(dec.avctx->width, dec.avctx->height);
            sp->setSerial(dec.pkt_serial);
            sp->setUploaded(false);

            sub_pool.push();
        }
    }
}

CSubtitle* SubTrack::peekCurrent(){
    return sub_pool.peek();
}

CSubtitle* SubTrack::peekNext(){
    if (sub_pool.nb_remaining() > 1)
        return sub_pool.peek_next();
    return nullptr;
}

int SubTrack::subsAvailable(){
    return sub_pool.nb_remaining();
}

void SubTrack::nextSub(){
    sub_pool.next();
}

