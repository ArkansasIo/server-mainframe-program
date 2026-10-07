#include "mf/util/time.hpp"
#include "mf/util/strings.hpp"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace mf::timeutil
{

    namespace
    {
        std::time_t toTimeT(TimePoint tp)
        {
            return Clock::to_time_t(tp);
        }
    } // namespace

    std::string toIso(TimePoint tp)
    {
        const std::time_t tt = toTimeT(tp);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &tt);
#else
        gmtime_r(&tt, &tm);
#endif
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
        return std::string(buffer);
    }

    std::string nowIso()
    {
        return toIso(Clock::now());
    }

    std::int64_t epochMs(TimePoint tp)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
    }

    std::int64_t epochMs()
    {
        return epochMs(Clock::now());
    }

    TimePoint parseIso(std::string_view text)
    {
        const std::string t = str::trim(text);
        if (t.size() < 10)
            return TimePoint{};

        std::tm tm{};
        int year = 0, month = 1, day = 1, hour = 0, minute = 0, second = 0;
        const int parsed = std::sscanf(t.c_str(), "%d-%d-%d%*[ T]%d:%d:%d",
                                       &year, &month, &day, &hour, &minute, &second);
        if (parsed < 4)
        {
            if (std::sscanf(t.c_str(), "%d-%d-%d", &year, &month, &day) != 3)
                return TimePoint{};
        }
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_sec = second;

#if defined(_WIN32)
        const std::time_t tt = _mkgmtime(&tm);
#else
        const std::time_t tt = timegm(&tm);
#endif
        if (tt == static_cast<std::time_t>(-1))
            return TimePoint{};
        return Clock::from_time_t(tt);
    }

    bool isValidIso(std::string_view text)
    {
        const std::string t = str::trim(text);
        if (t.size() < 19)
            return false;
        if (t[4] != '-' || t[7] != '-' || (t[10] != ' ' && t[10] != 'T') || t[13] != ':' || t[16] != ':')
        {
            return false;
        }
        return parseIso(t).time_since_epoch().count() != 0;
    }

    TimePoint addMinutes(TimePoint tp, double minutes)
    {
        return tp + std::chrono::milliseconds(static_cast<std::int64_t>(minutes * 60000.0));
    }

    TimePoint addDays(TimePoint tp, double days)
    {
        return addMinutes(tp, days * 1440.0);
    }

    double minutesBetween(TimePoint from, TimePoint to)
    {
        return std::chrono::duration<double>(to - from).count() / 60.0;
    }

    std::string humanDuration(std::int64_t milliseconds)
    {
        const std::int64_t n = milliseconds < 0 ? 0 : milliseconds;
        if (n < 1000)
            return std::to_string(n) + "ms";
        char buffer[48];
        if (n < 60000)
        {
            std::snprintf(buffer, sizeof(buffer), "%.2fs", static_cast<double>(n) / 1000.0);
        }
        else if (n < 3600000)
        {
            std::snprintf(buffer, sizeof(buffer), "%.2fm", static_cast<double>(n) / 60000.0);
        }
        else
        {
            std::snprintf(buffer, sizeof(buffer), "%.2fh", static_cast<double>(n) / 3600000.0);
        }
        return std::string(buffer);
    }

    std::string formatDurationShort(double seconds)
    {
        return humanDuration(static_cast<std::int64_t>(seconds * 1000.0));
    }

} // namespace mf::timeutil
