#include "csubtitle.hpp"

#include <stdexcept>

extern "C"{
#include <libavutil/mem.h>
}

CSubtitle::CSubtitle() {}
CSubtitle::~CSubtitle(){avsubtitle_free(&sub);}

CSubtitle::CSubtitle(CSubtitle&& other){
    *this = std::move(other);
}

CSubtitle& CSubtitle::operator=(CSubtitle&& other){
    move_from(std::move(other));
    return *this;
}

void CSubtitle::copyParams(const CSubtitle& from){
    sub_width = from.width();
    sub_height = from.height();
    sub_pts = from.ts();
    sub_duration = from.duration();
    start_time = from.startTime();
    end_time = from.endTime();
    serial = from.ser();
    uploaded = false;
}

void CSubtitle::move_from(CSubtitle&& rhs){
    clear();
    copyParams(rhs);
    sub = rhs.sub;
    rhs.sub = {};
    rhs.clear();
}

const AVSubtitleRect& CSubtitle::rectAt(int idx){
    if(idx < 0 || idx >= numRects())
        throw std::out_of_range("CSubtitle::rectAt: index out of range");
    return *sub.rects[idx];
}

static int copy_av_subtitle(AVSubtitle *dst, const AVSubtitle *src)
{
    int ret = AVERROR_BUG;
    AVSubtitle tmp = {
        .format = src->format,
        .start_display_time = src->start_display_time,
        .end_display_time = src->end_display_time,
        .num_rects = 0,
        .rects = NULL,
        .pts = src->pts
    };

    if (!src->num_rects)
        goto success;

    if (!(tmp.rects = (AVSubtitleRect**)av_calloc(src->num_rects, sizeof(*tmp.rects))))
        return AVERROR(ENOMEM);

    for (int i = 0; i < src->num_rects; i++) {
        AVSubtitleRect *src_rect = src->rects[i];
        AVSubtitleRect *dst_rect;

        if (!(dst_rect = tmp.rects[i] = (AVSubtitleRect*)av_mallocz(sizeof(*tmp.rects[0])))) {
            ret = AVERROR(ENOMEM);
            goto cleanup;
        }

        tmp.num_rects++;

        dst_rect->type      = src_rect->type;
        dst_rect->flags     = src_rect->flags;

        dst_rect->x         = src_rect->x;
        dst_rect->y         = src_rect->y;
        dst_rect->w         = src_rect->w;
        dst_rect->h         = src_rect->h;
        dst_rect->nb_colors = src_rect->nb_colors;

        if (src_rect->text)
            if (!(dst_rect->text = av_strdup(src_rect->text))) {
                ret = AVERROR(ENOMEM);
                goto cleanup;
            }

        if (src_rect->ass)
            if (!(dst_rect->ass = av_strdup(src_rect->ass))) {
                ret = AVERROR(ENOMEM);
                goto cleanup;
            }

        for (int j = 0; j < 4; j++) {
            // SUBTITLE_BITMAP images are special in the sense that they
            // are like PAL8 images. first pointer to data, second to
            // palette. This makes the size calculation match this.
            size_t buf_size = src_rect->type == SUBTITLE_BITMAP && j == 1 ?
                                  AVPALETTE_SIZE :
                                  src_rect->h * src_rect->linesize[j];

            if (!src_rect->data[j])
                continue;

            if (!(dst_rect->data[j] = (uint8_t*)av_memdup(src_rect->data[j], buf_size))) {
                ret = AVERROR(ENOMEM);
                goto cleanup;
            }
            dst_rect->linesize[j] = src_rect->linesize[j];
        }
    }

success:
    *dst = tmp;

    return 0;

cleanup:
    avsubtitle_free(&tmp);

    return ret;
}

CSubtitle::CSubtitle(const CSubtitle& other){
    *this = other;
}

CSubtitle& CSubtitle::operator=(const CSubtitle& other){
    clear();
    if(copy_av_subtitle(&sub, &other.constAv())){
        throw std::runtime_error("CSubtitle: failed to copy!");
    }
    copyParams(other);

    return *this;
}
