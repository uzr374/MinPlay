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
    bool clock_paused = false, eos = false;
    mutable std::mutex mutex;

private:
    void set_at(double pts, double time);

public:
    Clock() = default;
    ~Clock() = default;

    Q_DISABLE_COPY_MOVE(Clock);

    double get() const;
    void set(double pts, double time = gettime_s());
    void deactivate();
    double last_upd() const;
    bool is_paused() const;
    double pts() const;
    void set_paused(bool paused);
    void set_eos(bool eos, double ts);
};

#endif // CLOCK_H
