#include "mf/util/strings.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace mf::str
{

    std::string upper(std::string_view s)
    {
        std::string out(s);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c)
                       { return static_cast<char>(std::toupper(c)); });
        return out;
    }

    std::string lower(std::string_view s)
    {
        std::string out(s);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    std::string ltrim(std::string_view s)
    {
        std::size_t i = 0;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        return std::string(s.substr(i));
    }

    std::string rtrim(std::string_view s)
    {
        std::size_t end = s.size();
        while (end > 0 && std::isspace(static_cast<unsigned char>(s[end - 1])))
            --end;
        return std::string(s.substr(0, end));
    }

    std::string trim(std::string_view s)
    {
        std::size_t start = 0;
        std::size_t end = s.size();
        while (start < end && std::isspace(static_cast<unsigned char>(s[start])))
            ++start;
        while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
            --end;
        return std::string(s.substr(start, end - start));
    }

    bool startsWith(std::string_view s, std::string_view prefix)
    {
        return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    }

    bool endsWith(std::string_view s, std::string_view suffix)
    {
        return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    bool iequals(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            {
                return false;
            }
        }
        return true;
    }

    bool contains(std::string_view haystack, std::string_view needle)
    {
        return haystack.find(needle) != std::string_view::npos;
    }

    std::vector<std::string> split(std::string_view s, char delimiter, bool keepEmpty)
    {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (start <= s.size())
        {
            const std::size_t pos = s.find(delimiter, start);
            const std::string_view piece = pos == std::string_view::npos
                                               ? s.substr(start)
                                               : s.substr(start, pos - start);
            if (keepEmpty || !piece.empty())
                out.emplace_back(piece);
            if (pos == std::string_view::npos)
                break;
            start = pos + 1;
        }
        return out;
    }

    std::vector<std::string> splitAny(std::string_view s, std::string_view delimiters, bool keepEmpty)
    {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (start <= s.size())
        {
            const std::size_t pos = s.find_first_of(delimiters, start);
            const std::string_view piece = pos == std::string_view::npos
                                               ? s.substr(start)
                                               : s.substr(start, pos - start);
            if (keepEmpty || !piece.empty())
                out.emplace_back(piece);
            if (pos == std::string_view::npos)
                break;
            start = pos + 1;
        }
        return out;
    }

    std::vector<std::string> splitLines(std::string_view s)
    {
        std::vector<std::string> out;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= s.size(); ++i)
        {
            if (i == s.size() || s[i] == '\n')
            {
                std::string_view line = s.substr(start, i - start);
                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);
                out.emplace_back(line);
                start = i + 1;
            }
        }
        if (!out.empty() && out.back().empty() && !s.empty() && s.back() == '\n')
            out.pop_back();
        return out;
    }

    std::string join(const std::vector<std::string> &parts, std::string_view separator)
    {
        std::string out;
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            if (i)
                out += separator;
            out += parts[i];
        }
        return out;
    }

    std::string join(const std::vector<std::string_view> &parts, std::string_view separator)
    {
        std::string out;
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            if (i)
                out += separator;
            out += parts[i];
        }
        return out;
    }

    std::string replaceAll(std::string_view s, std::string_view from, std::string_view to)
    {
        if (from.empty())
            return std::string(s);
        std::string out;
        out.reserve(s.size());
        std::size_t pos = 0;
        while (true)
        {
            const std::size_t hit = s.find(from, pos);
            if (hit == std::string_view::npos)
            {
                out.append(s.substr(pos));
                break;
            }
            out.append(s.substr(pos, hit - pos));
            out.append(to);
            pos = hit + from.size();
        }
        return out;
    }

    void replaceInPlace(std::string &s, std::string_view from, std::string_view to)
    {
        s = replaceAll(s, from, to);
    }

    std::string repeat(std::string_view s, std::size_t times)
    {
        std::string out;
        out.reserve(s.size() * times);
        for (std::size_t i = 0; i < times; ++i)
            out.append(s);
        return out;
    }

    std::string padLeft(std::string_view s, std::size_t width, char fill)
    {
        if (s.size() >= width)
            return std::string(s);
        return std::string(width - s.size(), fill) + std::string(s);
    }

    std::string padRight(std::string_view s, std::size_t width, char fill)
    {
        if (s.size() >= width)
            return std::string(s);
        return std::string(s) + std::string(width - s.size(), fill);
    }

    std::string truncate(std::string_view s, std::size_t width, std::string_view ellipsis)
    {
        if (s.size() <= width)
            return std::string(s);
        if (width <= ellipsis.size())
            return std::string(s.substr(0, width));
        std::string out(s.substr(0, width - ellipsis.size()));
        out += ellipsis;
        return out;
    }

    std::string reverse(std::string_view s)
    {
        return std::string(s.rbegin(), s.rend());
    }

    std::optional<long long> toInt(std::string_view s)
    {
        const std::string t = trim(s);
        if (t.empty())
            return std::nullopt;
        char *end = nullptr;
        errno = 0;
        const long long value = std::strtoll(t.c_str(), &end, 10);
        if (errno != 0 || end == t.c_str() || *end != '\0')
            return std::nullopt;
        return value;
    }

    std::optional<double> toDouble(std::string_view s)
    {
        const std::string t = trim(s);
        if (t.empty())
            return std::nullopt;
        char *end = nullptr;
        errno = 0;
        const double value = std::strtod(t.c_str(), &end);
        if (errno != 0 || end == t.c_str() || *end != '\0')
            return std::nullopt;
        return value;
    }

    std::optional<bool> toBool(std::string_view s)
    {
        const std::string t = lower(trim(s));
        if (t == "true" || t == "1" || t == "yes" || t == "on")
            return true;
        if (t == "false" || t == "0" || t == "no" || t == "off")
            return false;
        return std::nullopt;
    }

    namespace
    {
        int hexValue(char c)
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        }
    } // namespace

    std::string urlDecode(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            const char c = s[i];
            if (c == '+')
            {
                out.push_back(' ');
            }
            else if (c == '%' && i + 2 < s.size())
            {
                const int hi = hexValue(s[i + 1]);
                const int lo = hexValue(s[i + 2]);
                if (hi >= 0 && lo >= 0)
                {
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                }
                else
                {
                    out.push_back(c);
                }
            }
            else
            {
                out.push_back(c);
            }
        }
        return out;
    }

    std::string urlEncode(std::string_view s)
    {
        static const char *hex = "0123456789ABCDEF";
        std::string out;
        out.reserve(s.size());
        for (const char c : s)
        {
            const unsigned char uc = static_cast<unsigned char>(c);
            const bool safe = std::isalnum(uc) || c == '-' || c == '_' || c == '.' || c == '~';
            if (safe)
            {
                out.push_back(c);
            }
            else
            {
                out.push_back('%');
                out.push_back(hex[uc >> 4]);
                out.push_back(hex[uc & 0x0F]);
            }
        }
        return out;
    }

    std::vector<std::pair<std::string, std::string>> parseQuery(std::string_view query)
    {
        std::vector<std::pair<std::string, std::string>> out;
        std::size_t start = 0;
        if (!query.empty() && query.front() == '?')
            start = 1;
        while (start <= query.size())
        {
            const std::size_t amp = query.find('&', start);
            const std::string_view pair = amp == std::string_view::npos
                                              ? query.substr(start)
                                              : query.substr(start, amp - start);
            if (!pair.empty())
            {
                const std::size_t eq = pair.find('=');
                if (eq == std::string_view::npos)
                {
                    out.emplace_back(urlDecode(pair), std::string());
                }
                else
                {
                    out.emplace_back(urlDecode(pair.substr(0, eq)), urlDecode(pair.substr(eq + 1)));
                }
            }
            if (amp == std::string_view::npos)
                break;
            start = amp + 1;
        }
        return out;
    }

    std::string escapeHtml(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (const char c : s)
        {
            switch (c)
            {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#39;";
                break;
            default:
                out.push_back(c);
            }
        }
        return out;
    }

    std::uint64_t fnv1a(std::string_view s)
    {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const char c : s)
        {
            hash ^= static_cast<unsigned char>(c);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    std::string format(std::string_view pattern, const std::vector<std::string> &args)
    {
        std::string out;
        std::size_t next = 0;
        for (std::size_t i = 0; i < pattern.size(); ++i)
        {
            if (pattern[i] == '{' && i + 1 < pattern.size() && pattern[i + 1] == '}')
            {
                if (next < args.size())
                {
                    out += args[next++];
                }
                ++i;
            }
            else
            {
                out.push_back(pattern[i]);
            }
        }
        return out;
    }

} // namespace mf::str
