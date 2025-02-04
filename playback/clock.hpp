#ifndef CLOCK_HPP
#define CLOCK_HPP

extern "C"{
#include <libavutil/time.h>
}

#include <cmath>

inline double gettime(){
    return av_gettime_relative() / 1000000.0;
}

struct Clock final {
private:
    double pts = NAN;
    double last_updated = NAN;
    double speed = 1.0;
    bool paused = false;

public:
    double get() const
    {
        if (paused) {
            return pts;
        } else {
            const double time_drift = gettime() - last_updated;
            return pts + time_drift * speed;
        }
    }

    void set(double pts)
    {
        this->pts = pts;
        this->last_updated = gettime();
    }

    void set_speed(double speed)
    {
        set(get());
        this->speed = speed;
    }

    void setPaused(bool p){
        set(get());
        paused = p;
    }

    double updatedAt() const {return last_updated;}

    void resetTime(){pts = last_updated = NAN;}
};

#endif // CLOCK_HPP
