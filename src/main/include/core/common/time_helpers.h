#pragma once

#include <ctime>

#if defined(_WIN32)
inline bool reqpack_gmtime_utc(std::tm* tm, const std::time_t* timeValue) {
    return gmtime_s(tm, timeValue) == 0;
}

inline bool reqpack_localtime(std::tm* tm, const std::time_t* timeValue) {
    return localtime_s(tm, timeValue) == 0;
}
#else
inline bool reqpack_gmtime_utc(std::tm* tm, const std::time_t* timeValue) {
    return gmtime_r(timeValue, tm) != nullptr;
}

inline bool reqpack_localtime(std::tm* tm, const std::time_t* timeValue) {
    return localtime_r(timeValue, tm) != nullptr;
}
#endif
