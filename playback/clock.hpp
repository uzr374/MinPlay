#ifndef CLOCK_H
#define CLOCK_H

#include "../src/utils.hpp"

#include <mutex>
#include <QtGlobal>

class Clock {
private:
    double clock_pts = 0.0;
    double clock_last_updated = 0.0;
    double clock_speed = 1.0;
    bool clock_paused = false, eos = false;
    mutable std::mutex mutex;

public:
    Clock() = default;
    ~Clock() = default;

    Q_DISABLE_COPY_MOVE(Clock);

    double get() const;
    void set(double pts, double time = Utils::gettime_s());
    double get_nolock() const;
    void set_nolock(double pts, double time = Utils::gettime_s());
    void deactivate();
    double last_upd() const;
    bool is_paused() const;
    double pts() const;
    void set_paused(bool paused);
    void set_eos(bool eos, double ts);
};

#endif // CLOCK_H
