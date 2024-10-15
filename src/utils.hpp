#ifndef MINPLAY_UTILS_HPP
#define MINPLAY_UTILS_HPP

#include <QtGlobal>

namespace Utils {
QString secToHMS(double timeS);
QString posToHMS(double timeS, double durS);
double gettime_s();
void sleep_s(double sec);
}

#endif
