#ifndef AVTRACK_HPP
#define AVTRACK_HPP

#include "decoder.hpp"
#include "cavstream.hpp"

class AVTrack
{
protected:
    Decoder dec;
    PacketQueue pkts;
    CAVStream rel_st;

    /*For dynamic error reporting on the decoder thread*/
    std::atomic_bool has_error = false;
    std::string errors;

public:
    AVTrack() = delete;
    AVTrack(const CAVStream& st, std::condition_variable& empty_q_cond);
    ~AVTrack();

    void flush();
    void putPacket(CAVPacket&& pkt);
    void putFinalPacket(int st_idx);
    std::tuple<int, int, double> getQueueParams();
    int serial();
};

#endif // AVTRACK_HPP
