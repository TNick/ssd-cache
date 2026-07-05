#include "ssd_cache/time.h"

#include <array>
#include <cstdio>
#include <ctime>

namespace ssd_cache {
namespace {

std::time_t utc_timegm(std::tm* value) {
#ifdef _WIN32
    return _mkgmtime(value);
#else
    return timegm(value);
#endif
}

}  // namespace

std::string format_utc(std::chrono::system_clock::time_point time_point) {
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(
        time_point
    );
    const std::time_t raw = std::chrono::system_clock::to_time_t(seconds);

    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &raw);
#else
    gmtime_r(&raw, &utc);
#endif

    std::array<char, 32> buffer{};
    std::snprintf(
        buffer.data(),
        buffer.size(),
        "%04d-%02d-%02dT%02d:%02d:%02dZ",
        utc.tm_year + 1900,
        utc.tm_mon + 1,
        utc.tm_mday,
        utc.tm_hour,
        utc.tm_min,
        utc.tm_sec
    );

    return buffer.data();
}

std::optional<std::chrono::system_clock::time_point> parse_utc(
    std::string_view value
) {
    std::tm utc{};
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;

    if (std::sscanf(
            std::string(value).c_str(),
            "%d-%d-%dT%d:%d:%dZ",
            &year,
            &month,
            &day,
            &hour,
            &minute,
            &second
        ) != 6) {
        return std::nullopt;
    }

    utc.tm_year = year - 1900;
    utc.tm_mon = month - 1;
    utc.tm_mday = day;
    utc.tm_hour = hour;
    utc.tm_min = minute;
    utc.tm_sec = second;
    utc.tm_isdst = 0;

    const std::time_t raw = utc_timegm(&utc);
    if (raw == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }

    return std::chrono::system_clock::from_time_t(raw);
}

}  // namespace ssd_cache
