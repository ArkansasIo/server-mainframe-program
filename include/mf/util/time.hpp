// ---------------------------------------------------------------------------
// Time helpers. The database stores timestamps as UTC strings in the form
// "YYYY-MM-DD HH:MM:SS" which sorts lexicographically - the same convention
// SQLite's datetime('now') produces.
// ---------------------------------------------------------------------------
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace mf::timeutil
{

    using Clock = std::chrono::system_clock;
    using TimePoint = Clock::time_point;

    // "2026-10-07 14:03:22"
    std::string nowIso();
    std::string toIso(TimePoint tp);

    // Milliseconds since the Unix epoch.
    std::int64_t epochMs();
    std::int64_t epochMs(TimePoint tp);

    // Parse "YYYY-MM-DD HH:MM:SS" (or ISO with 'T'). Returns zero time on failure.
    TimePoint parseIso(std::string_view text);
    bool isValidIso(std::string_view text);

    // Arithmetic.
    TimePoint addMinutes(TimePoint tp, double minutes);
    TimePoint addDays(TimePoint tp, double days);
    double minutesBetween(TimePoint from, TimePoint to);

    // Human readable helpers.
    std::string humanDuration(std::int64_t milliseconds);
    std::string formatDurationShort(double seconds);

    // Monotonic stopwatch for the job scheduler and request timing.
    class Stopwatch
    {
    public:
        Stopwatch() : start_(std::chrono::steady_clock::now()) {}

        void reset() { start_ = std::chrono::steady_clock::now(); }

        std::int64_t elapsedMs() const
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start_)
                .count();
        }

        double elapsedSeconds() const
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
        }

    private:
        std::chrono::steady_clock::time_point start_;
    };

} // namespace mf::timeutil
