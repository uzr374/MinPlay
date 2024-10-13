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
    const auto res = av_channel_layout_copy(&ch_layout, &src.ch_layout);
    return res == 0;
}
bool CAVChannelLayout::isNative() const{return ch_layout.order == AV_CHANNEL_ORDER_NATIVE;}

