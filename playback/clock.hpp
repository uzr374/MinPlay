#ifndef CLOCK_H
#define CLOCK_H

#include <mutex>

#include <QtGlobal>

extern "C"{
#include <libavutil/time.h>
}

inline double gettime_s(){
    return av_gettime_relative() / 1000000.0;
}

class Clock {
private:
    double clock_pts = 0.0;           /* clock base */
    double clock_pts_drift = 0.0;     /* clock base minus time at which we updated the clock */
    double clock_last_updated = 0.0;
    double clock_speed = 1.0;
    int clock_serial = -1;           /* clock is based on a packet with this serial */
    bool clock_paused = false;
    int *queue_serial = nullptr;    /* pointer to the current packet queue serial, used for obsolete clock detection */
    mutable std::mutex mutex;

private:
    void set_at(double pts, int serial, double time);

public:
    Clock() = default;
    ~Clock() = default;

    Q_DISABLE_COPY_MOVE(Clock);

    double get() const;
    void set(double pts, int serial, double time = gettime_s());
    void init(int *queue_serial);
    int serial() const;
    double last_upd() const;
    bool is_paused() const;
    double pts() const;
    void set_paused(bool paused);
};

#endif // CLOCK_H
