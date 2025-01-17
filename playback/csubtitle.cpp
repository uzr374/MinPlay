#include "csubtitle.hpp"

#include <stdexcept>

CSubtitle::CSubtitle() {}
CSubtitle::~CSubtitle(){avsubtitle_free(&sub);}

CSubtitle::CSubtitle(CSubtitle&& other){
    *this = std::move(other);
}

CSubtitle& CSubtitle::operator=(CSubtitle&& other){
    move_from(std::move(other));
    return *this;
}

void CSubtitle::move_from(CSubtitle&& rhs){
    clear();
    sub = rhs.sub;
    rhs.sub = {};
    sub_width = rhs.sub_width;
    sub_height = rhs.sub_height;
    sub_pts = rhs.sub_pts;
    sub_duration = rhs.sub_duration;
    rhs.clear();
}

const AVSubtitleRect& CSubtitle::rectAt(int idx){
    if(idx < 0 || idx >= numRects())
        throw std::out_of_range("CSubtitle::rectAt: index out of range");
    return *sub.rects[idx];
}
