// ---------------------------------------------------------------------------
// RFC-4180 style CSV reader/writer. Handles quoted fields, embedded
// delimiters, embedded newlines and escaped quotes ("" -> ").
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "mf/util/json.hpp"

namespace mf::csv
{

    struct ParseOptions
    {
        char delimiter = ',';
        bool stripBom = true;
        bool trimHeaders = true;
    };

    struct WriteOptions
    {
        char delimiter = ',';
        std::string eol = "\r\n";
        bool header = true;
        std::vector<std::string> columns; // explicit order (optional)
    };

    using Row = std::vector<std::string>;

    // Raw row parsing.
    std::vector<Row> parse(std::string_view text, const ParseOptions &options = {});

    // Parse into objects keyed by the first row's headers.
    json::Array parseObjects(std::string_view text, const ParseOptions &options = {});

    // Serialize objects (or raw rows) to CSV text.
    std::string write(const json::Array &rows, const WriteOptions &options = {});
    std::string writeRows(const std::vector<Row> &rows, const WriteOptions &options = {});

    // Escape a single cell.
    std::string escapeCell(std::string_view value, char delimiter = ',');

    // Coerce a CSV string into a JSON scalar when it looks like a number, bool or NULL.
    json::Value coerceCell(std::string_view value);

    // Quote-aware split of a single header line (used by the import tool).
    std::vector<std::string> parseHeader(std::string_view line, char delimiter = ',');

} // namespace mf::csv
