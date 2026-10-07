#include "mf/util/ids.hpp"

#include "mf/util/strings.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>

namespace mf::ids
{

    namespace
    {
        std::mt19937_64 &engine()
        {
            static thread_local std::mt19937_64 gen([]
                                                    {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count())};
        return std::mt19937_64(seq); }());
            return gen;
        }

        std::string base36(std::uint64_t value)
        {
            static const char *digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
            if (value == 0)
                return "0";
            std::string out;
            while (value > 0)
            {
                out.push_back(digits[value % 36]);
                value /= 36;
            }
            return std::string(out.rbegin(), out.rend());
        }
    } // namespace

    std::string randomHex(int bytes)
    {
        std::uniform_int_distribution<int> dist(0, 255);
        static const char *hex = "0123456789abcdef";
        std::string out;
        out.reserve(static_cast<std::size_t>(bytes) * 2);
        for (int i = 0; i < bytes; ++i)
        {
            const int byte = dist(engine());
            out.push_back(hex[(byte >> 4) & 0x0F]);
            out.push_back(hex[byte & 0x0F]);
        }
        return out;
    }

    std::string randomBytes(int count)
    {
        std::uniform_int_distribution<int> dist(0, 255);
        std::string out;
        out.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i)
            out.push_back(static_cast<char>(dist(engine())));
        return out;
    }

    std::string make(std::string_view prefix, int randomBytesCount)
    {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        return str::upper(std::string(prefix)) + "-" + base36(static_cast<std::uint64_t>(now)) + "-" + str::upper(randomHex(randomBytesCount));
    }

    std::string job() { return make("JOB"); }
    std::string transaction() { return make("TXN"); }
    std::string session() { return make("SES"); }
    std::string correlation() { return make("COR"); }

    std::string uuid()
    {
        const std::string raw = randomHex(16);
        std::string out = raw;
        out[12] = '4'; // version 4
        const int variant = (std::stoi(std::string(1, raw[16]), nullptr, 16) & 0x3) | 0x8;
        static const char *hex = "0123456789abcdef";
        out[16] = hex[variant];
        return out.substr(0, 8) + "-" + out.substr(8, 4) + "-" + out.substr(12, 4) + "-" + out.substr(16, 4) + "-" + out.substr(20, 12);
    }

} // namespace mf::ids
