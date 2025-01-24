#ifndef CAVCHANNELLAYOUT_HPP
#define CAVCHANNELLAYOUT_HPP

extern "C"{
#include <libavutil/channel_layout.h>
}

class CAVChannelLayout
{
private:
    AVChannelLayout ch_layout{};
public:
    CAVChannelLayout();
    ~CAVChannelLayout();
    CAVChannelLayout(const AVChannelLayout& src);
    CAVChannelLayout(const CAVChannelLayout&);
    CAVChannelLayout(CAVChannelLayout&&);
    CAVChannelLayout& operator=(const CAVChannelLayout&);
    CAVChannelLayout& operator=(CAVChannelLayout&& rhs);
    bool operator!=(const AVChannelLayout& rhs);
    bool operator==(const AVChannelLayout& rhs);
    bool operator!=(const CAVChannelLayout& rhs);
    bool operator==(const CAVChannelLayout& rhs);
    void reset();
    void make_default(int nb_channels);
    const AVChannelLayout& constAv() const;
    int nbChannels() const;
    bool copy(const CAVChannelLayout& src);
    bool isNative() const;
};

#endif // CAVCHANNELLAYOUT_HPP
