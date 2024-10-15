#include "utils.hpp"

extern "C"{
#include <libavutil/time.h>
#include <libavutil/avutil.h>
}

#include <QString>

namespace Utils {
QString secToHMS(double seconds){
    const int hours = static_cast<int>(seconds) / 3600;
    const int minutes = (static_cast<int>(seconds) % 3600) / 60;
    const int secs = static_cast<int>(seconds) % 60;
    return QString::asprintf("%d:%02d:%02d", hours, minutes, secs);
}

QString posToHMS(double timeS, double durS){
    const auto currentTime = secToHMS(timeS);
    const auto totalTime = secToHMS(durS);

    return QString("%1/%2").arg(currentTime, totalTime);
}

double gettime_s(){
    return av_gettime_relative() / double(AV_TIME_BASE);
}

void sleep_s(double sec){
    av_usleep(sec * AV_TIME_BASE);
}
}
