#include "cavchannellayout.hpp"

CAVChannelLayout::CAVChannelLayout() {}

CAVChannelLayout::~CAVChannelLayout(){reset();}
CAVChannelLayout::CAVChannelLayout(const AVChannelLayout& src){
    const auto res = av_channel_layout_copy(&ch_layout, &src);
}
CAVChannelLayout::CAVChannelLayout(const CAVChannelLayout& rhs){
    copy(rhs);
}
CAVChannelLayout::CAVChannelLayout(CAVChannelLayout&& rhs){
    copy(rhs);
}
CAVChannelLayout& CAVChannelLayout::operator=(const CAVChannelLayout& rhs){
    copy(rhs);
    return *this;
}
CAVChannelLayout& CAVChannelLayout::operator=(CAVChannelLayout&& rhs){
    copy(rhs);
    return *this;
}
void CAVChannelLayout::reset(){
    av_channel_layout_uninit(&ch_layout);
}
int CAVChannelLayout::nbChannels() const{
    return ch_layout.nb_channels;
}
void CAVChannelLayout::make_default(int nb_channels){
    reset();
    av_channel_layout_default(&ch_layout, nb_channels);
}
const AVChannelLayout& CAVChannelLayout::constAv() const{return ch_layout;}

bool CAVChannelLayout::copy(const CAVChannelLayout& src){
    reset();
    return av_channel_layout_copy(&ch_layout, &src.ch_layout) == 0;
}
bool CAVChannelLayout::isNative() const{return ch_layout.order == AV_CHANNEL_ORDER_NATIVE;}

bool CAVChannelLayout::operator!=(const AVChannelLayout& rhs){
    return !(*this == rhs);
}

bool CAVChannelLayout::operator==(const AVChannelLayout& rhs){
    return av_channel_layout_compare(&ch_layout, &rhs) == 0;
}

bool CAVChannelLayout::operator!=(const CAVChannelLayout& rhs){
    return !(*this == rhs.ch_layout);
}

bool CAVChannelLayout::operator==(const CAVChannelLayout& rhs){
    return *this == rhs.ch_layout;
}

