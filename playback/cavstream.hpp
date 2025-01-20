#ifndef CAVSTREAM_HPP
#define CAVSTREAM_HPP

extern "C"{
#include <libavformat/avformat.h>
}

#include <string>

class CAVStream final
{
    AVCodecParameters* codecpar = nullptr;
    AVRational frame_rate{};
    AVRational sample_ar{};
    AVRational time_base{};
    bool is_attached_pic = false;
    int64_t start_time = AV_NOPTS_VALUE;
    int64_t stream_duration = AV_NOPTS_VALUE;
    int index = -1;// In AVFormatContext
    std::string title_str, lang_str;

    void copyFrom(const CAVStream& rhs);

public:
    CAVStream();
    CAVStream(AVFormatContext* ctx, int stream_index);
    ~CAVStream();
    CAVStream(const CAVStream& rhs);
    CAVStream(CAVStream&& rhs);
    CAVStream& operator=(const CAVStream& rhs);
    CAVStream& operator=(CAVStream&& rhs);

    void clear();

    const AVCodec* getCodec() const;
    const AVCodecParameters& codecPar() const;
    AVRational frameRate() const;
    AVRational sampleAR() const;
    AVRational tb() const;
    bool isVideo() const;
    bool isAudio() const;
    bool isSub() const;
    bool isAttachedPic() const;
    AVMediaType type() const;
    int64_t startTime() const;
    int64_t duration() const;
    int idx() const;
    std::string titleStr() const;
    std::string langStr() const;
    int width() const;
    int height() const;
    int sampleRate() const;
    int channelCount() const;
    std::string chLayoutStr() const;
};

#endif // CAVSTREAM_HPP
