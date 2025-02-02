#ifndef SUBTRACK_HPP
#define SUBTRACK_HPP

#include "avtrack.hpp"
#include "framequeue.hpp"
#include "csubtitle.hpp"

class SubTrack : public AVTrack
{
private:
    FrameQueue<CSubtitle> sub_pool;

    void run();

public:
    SubTrack(const CAVStream& st, std::condition_variable&);
    ~SubTrack();
};

#endif // SUBTRACK_HPP
