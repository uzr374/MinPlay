#ifndef FORMATCONTEXT_HPP
#define FORMATCONTEXT_HPP

#include <QtGlobal>
#include <vector>
#include <QString>

#include "cavstream.hpp"
#include "cavpacket.hpp"

extern "C"{
#include <libavformat/avformat.h>
}

struct SeekInfo final {
    enum SeekType{
        SEEK_NONE, SEEK_PERCENT, SEEK_INCREMENT, SEEK_CHAPTER, SEEK_STREAM_SWITCH
    };

    SeekType type = SEEK_NONE;
    double percent = 0.0, increment = 0.0;
    int chapter_incr = 0, chapter_nb = -1;

    /*if stream_idx is negative and stream_type is set, then the streams of given type are cycled*/
    int stream_idx = -1;
    AVMediaType stream_type = AVMEDIA_TYPE_UNKNOWN;
};

class FormatContext final
{
    Q_DISABLE_COPY_MOVE(FormatContext);

private:
    AVFormatContext* ic = nullptr;
    std::vector<CAVStream> cstreams;
    bool realtime = false, seek_by_bytes = false, eof = false,
        dynamic_streams = false, seekable = false, local_paused = false, rtsp_or_mmsh = false;
    double max_frame_duration = 0.0, duration_s = 0.0;
    int video_idx = -1, video_last_idx = -1, audio_idx = -1, audio_last_idx = -1, sub_idx = -1, sub_last_idx = -1;
    int64_t last_seek_pos = 0, last_seek_rel = 0;
    QString title;

public:
    FormatContext() = delete;
    FormatContext(QString url, decltype(AVFormatContext::interrupt_callback.callback) int_cb, void* cb_opaque);
    ~FormatContext();

    bool isRealtime() const;
    bool isRTSPorMMSH() const;
    bool byteSeek() const;
    double maxFrameDuration() const;
    double duration() const;
    int64_t size() const;
    bool seek(const SeekInfo& info, double last_pts, int64_t last_pos);
    bool setStreamEnabled(int idx, bool enabled);
    int read(CAVPacket& into);//Returns the error codes from av_read_frame()
    const std::vector<CAVStream>& streams() const;
    int streamCount() const;
    int videoStIdx() const;
    int audioStIdx() const;
    int subStIdx() const;
    bool eofReached() const;
    int setPaused(bool paused);
    inline AVFormatContext* av(){return ic;}; //must be eventually removed
};

#endif // FORMATCONTEXT_HPP
