#include "clock.hpp"

#include <cmath>

double Clock::get() const
{
    std::unique_lock lck(mutex);
    if (clock_paused || std::isnan(clock_pts)) {
        return clock_pts;
    } else {
        const auto time = gettime_s();
        return clock_pts_drift + time - (time - clock_last_updated) * (1.0 - clock_speed);
    }
}

void Clock::set_at(double pts, double time)
{
    clock_pts = pts;
    clock_last_updated = time;
    clock_pts_drift = pts - time;
}

void Clock::set(double pts, double time)
{
    std::unique_lock lck(mutex);
    set_at(pts, time);
}

void Clock::init()
{
    clock_speed = 1.0;
    clock_paused = 0;
    set(NAN, -1);
}

double Clock::last_upd() const {return clock_last_updated;}
bool Clock::is_paused() const{return clock_paused;}
double Clock::pts() const{return clock_pts;}
void Clock::set_paused(bool paused){
    const auto value = get();
    std::unique_lock lck(mutex);
    if(paused){
        set_at(value, gettime_s());
    }
    clock_paused = paused;
}
