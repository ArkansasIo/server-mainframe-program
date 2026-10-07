// ---------------------------------------------------------------------------
// String helpers shared by every layer. Kept allocation-light and header
// declared / cpp defined so the whole project links against one instance.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mf::str
{

    // -- basic transforms -------------------------------------------------------
    std::string upper(std::string_view s);
    std::string lower(std::string_view s);
    std::string trim(std::string_view s);
    std::string ltrim(std::string_view s);
    std::string rtrim(std::string_view s);

    // -- predicates -------------------------------------------------------------
    bool startsWith(std::string_view s, std::string_view prefix);
    bool endsWith(std::string_view s, std::string_view suffix);
    bool iequals(std::string_view a, std::string_view b);
    bool contains(std::string_view haystack, std::string_view needle);

    // -- splitting / joining ----------------------------------------------------
    std::vector<std::string> split(std::string_view s, char delimiter, bool keepEmpty = false);
    std::vector<std::string> splitAny(std::string_view s, std::string_view delimiters, bool keepEmpty = false);
    std::vector<std::string> splitLines(std::string_view s);
    std::string join(const std::vector<std::string> &parts, std::string_view separator);
    std::string join(const std::vector<std::string_view> &parts, std::string_view separator);

    // -- mutation ---------------------------------------------------------------
    std::string replaceAll(std::string_view s, std::string_view from, std::string_view to);
    void replaceInPlace(std::string &s, std::string_view from, std::string_view to);
    std::string repeat(std::string_view s, std::size_t times);
    std::string padLeft(std::string_view s, std::size_t width, char fill = ' ');
    std::string padRight(std::string_view s, std::size_t width, char fill = ' ');
    std::string truncate(std::string_view s, std::size_t width, std::string_view ellipsis = "...");
    std::string reverse(std::string_view s);

    // -- parsing ----------------------------------------------------------------
    std::optional<long long> toInt(std::string_view s);
    std::optional<double> toDouble(std::string_view s);
    std::optional<bool> toBool(std::string_view s);

    // Percent-encoding used by the HTTP layer.
    std::string urlDecode(std::string_view s);
    std::string urlEncode(std::string_view s);

    // Query-string parsing: "a=1&b=two" -> { { "a", "1" }, { "b", "two" } }
    std::vector<std::pair<std::string, std::string>> parseQuery(std::string_view query);

    // HTML escaping for the server-rendered dashboard snippets.
    std::string escapeHtml(std::string_view s);

    // A non-cryptographic 64-bit hash, used for rate-limit and dedupe tables.
    std::uint64_t fnv1a(std::string_view s);

    // Format: substitute {} placeholders left to right.
    std::string format(std::string_view pattern, const std::vector<std::string> &args);

} // namespace mf::str
