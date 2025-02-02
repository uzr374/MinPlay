#ifndef FRAMEQUEUE_HPP
#define FRAMEQUEUE_HPP

#include "packetqueue.hpp"

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE std::max(SAMPLE_QUEUE_SIZE, std::max(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

template <typename T>
class FrameQueue final {
    Q_DISABLE_COPY_MOVE(FrameQueue);

    T queue[FRAME_QUEUE_SIZE];
    int rindex = 0;
    int windex = 0;
    int size = 0;
    int max_size = 0;
    int keep_last = 0;
    int rindex_shown = 0;
    std::mutex mutex;
    std::condition_variable cond;
    PacketQueue& pktq;

public:
    FrameQueue(PacketQueue &q, int max_size, int keep_last) : pktq(q)
    {
        this->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
        this->keep_last = !!keep_last;
    }

    void flush()
    {
        for(auto& el : queue)
            el.clear();
    }

    void notify()
    {
        std::scoped_lock lck(mutex);
        cond.notify_one();
    }

    T *peek()
    {
        return &queue[(rindex + rindex_shown) % max_size];
    }

    T* peek_next()
    {
        return &queue[(rindex + rindex_shown + 1) % max_size];
    }

    T *peek_last()
    {
        return &queue[rindex];
    }

    T *peek_writable()
    {
        /* wait until we have space to put a new frame */
        std::unique_lock lck(mutex);
        while (size >= max_size &&
               !pktq.isAborted()) {
            cond.wait(lck);
        }

        if (pktq.isAborted())
            return NULL;

        return &queue[windex];
    }

    T *peek_readable()
    {
        /* wait until we have a readable a new frame */
        std::unique_lock lck(mutex);
        while (size - rindex_shown <= 0 &&
               !pktq.isAborted()) {
            cond.wait(lck);
        }

        if (pktq.isAborted())
            return NULL;

        return &queue[(rindex + rindex_shown) % max_size];
    }

    void push()
    {
        if (++windex == max_size)
            windex = 0;
        std::scoped_lock lck(mutex);
        size++;
        cond.notify_one();
    }

    void next()
    {
        if (keep_last && !rindex_shown) {
            rindex_shown = 1;
            return;
        }
        queue[rindex].clear();
        if (++rindex == max_size)
            rindex = 0;
        std::scoped_lock lck(mutex);
        size--;
        cond.notify_one();
    }

    /* return the number of undisplayed frames in the queue */
    int nb_remaining()
    {
        std::scoped_lock lck(mutex);
        return size - rindex_shown;
    }

    int rindexShown() const{
        return rindex_shown;
    }

    /* return last shown position */
    int64_t last_pos()
    {
        std::scoped_lock lck(mutex);
        const T& fp = queue[rindex];
        if (rindexShown() && fp.serial() == pktq.serial())
            return fp.pktPos();
        else
            return -1LL;
    }
};



#endif // FRAMEQUEUE_HPP
