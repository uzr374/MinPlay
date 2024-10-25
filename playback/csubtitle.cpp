#include "csubtitle.hpp"

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
