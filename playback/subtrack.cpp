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

void SubTrack::run()
{
    CSubtitle *sp;
    int got_subtitle;
    double pts;

    // for (;;) {
    //     if (!(sp = frame_queue_peek_writable(&is->subpq)))
    //         return 0;

    //     if ((got_subtitle = decoder_decode_frame(&is->subdec, NULL, &sp->sub)) < 0)
    //         break;

    //     pts = 0;

    //     if (got_subtitle && sp->sub.format == 0) {
    //         if (sp->sub.pts != AV_NOPTS_VALUE)
    //             pts = sp->sub.pts / (double)AV_TIME_BASE;
    //         sp->pts = pts;
    //         sp->serial = is->subdec.pkt_serial;
    //         sp->width = is->subdec.avctx->width;
    //         sp->height = is->subdec.avctx->height;
    //         sp->uploaded = 0;

    //         /* now we can update the picture count */
    //         frame_queue_push(&is->subpq);
    //     } else if (got_subtitle) {
    //         avsubtitle_free(&sp->sub);
    //     }
    // }
}

