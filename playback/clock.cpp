#include "clock.hpp"

#include <cmath>

double Clock::get() const
{
    std::unique_lock lck(mutex);
    if (*queue_serial != clock_serial)
        return NAN;
    if (clock_paused) {
        return clock_pts;
    } else {
        const auto time = gettime_s();
        return clock_pts_drift + time - (time - clock_last_updated) * (1.0 - clock_speed);
    }
}

void Clock::set_at(double pts, int serial, double time)
{
    clock_pts = pts;
    clock_last_updated = time;
    clock_pts_drift = pts - time;
    clock_serial = serial;
}

void Clock::set(double pts, int serial, double time)
{
    std::unique_lock lck(mutex);
    set_at(pts, serial, time);
}

void Clock::init(int *queue_serial)
{
    clock_speed = 1.0;
    clock_paused = 0;
    this->queue_serial = queue_serial;
    set(NAN, -1);
}

int Clock::serial() const{return clock_serial;}
double Clock::last_upd() const {return clock_last_updated;}
bool Clock::is_paused() const{return clock_paused;}
double Clock::pts() const{return clock_pts;}
void Clock::set_paused(bool paused){
    std::unique_lock lck(mutex);
    clock_paused = paused;
}
