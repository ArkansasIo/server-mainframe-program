#include "mf/util/csv.hpp"

#include "mf/util/strings.hpp"

#include <cstdlib>

namespace mf::csv
{

    namespace
    {
        std::string_view stripBom(std::string_view text)
        {
            if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
            {
                return text.substr(3);
            }
            return text;
        }
    } // namespace

    std::vector<Row> parse(std::string_view text, const ParseOptions &options)
    {
        if (options.stripBom)
            text = stripBom(text);

        std::vector<Row> rows;
        Row row;
        std::string field;
        bool inQuotes = false;
        field.reserve(32);

        const auto flushField = [&]()
        {
            row.push_back(field);
            field.clear();
        };
        const auto flushRow = [&]()
        {
            flushField();
            rows.push_back(row);
            row.clear();
        };

        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (inQuotes)
            {
                if (c == '"')
                {
                    if (i + 1 < text.size() && text[i + 1] == '"')
                    {
                        field.push_back('"');
                        ++i;
                    }
                    else
                    {
                        inQuotes = false;
                    }
                }
                else
                {
                    field.push_back(c);
                }
                continue;
            }

            if (c == '"')
            {
                inQuotes = true;
            }
            else if (c == options.delimiter)
            {
                flushField();
            }
            else if (c == '\r')
            {
                // swallow; \n terminates the record
            }
            else if (c == '\n')
            {
                flushRow();
            }
            else
            {
                field.push_back(c);
            }
        }

        if (!field.empty() || !row.empty())
            flushRow();
        return rows;
    }

    json::Array parseObjects(std::string_view text, const ParseOptions &options)
    {
        json::Array out;
        const std::vector<Row> rows = parse(text, options);
        if (rows.empty())
            return out;

        std::vector<std::string> headers = rows[0];
        if (options.trimHeaders)
        {
            for (auto &header : headers)
                header = str::trim(header);
        }

        for (std::size_t r = 1; r < rows.size(); ++r)
        {
            const Row &row = rows[r];
            bool allEmpty = true;
            for (const auto &value : row)
            {
                if (!str::trim(value).empty())
                {
                    allEmpty = false;
                    break;
                }
            }
            if (allEmpty)
                continue;

            json::Value object = json::Value::object();
            for (std::size_t c = 0; c < headers.size(); ++c)
            {
                object.set(headers[c], json::Value(c < row.size() ? row[c] : std::string()));
            }
            out.push_back(std::move(object));
        }
        return out;
    }

    std::string escapeCell(std::string_view value, char delimiter)
    {
        const bool needsQuotes = value.find(delimiter) != std::string_view::npos || value.find('"') != std::string_view::npos || value.find('\n') != std::string_view::npos || value.find('\r') != std::string_view::npos;
        if (!needsQuotes)
            return std::string(value);
        return "\"" + str::replaceAll(value, "\"", "\"\"") + "\"";
    }

    namespace
    {
        std::string cellToText(const json::Value &value)
        {
            switch (value.type())
            {
            case json::Type::Null:
                return "";
            case json::Type::Bool:
                return value.asBool() ? "true" : "false";
            case json::Type::Number:
                return value.isInteger() ? std::to_string(value.asInt())
                                         : std::to_string(value.asNumber());
            case json::Type::String:
                return value.asString();
            case json::Type::Array:
            case json::Type::Object:
                return value.dumpCompact();
            }
            return "";
        }
    } // namespace

    std::string write(const json::Array &rows, const WriteOptions &options)
    {
        std::vector<std::string> columns = options.columns;
        if (columns.empty())
        {
            for (const auto &row : rows)
            {
                if (row.isObject())
                {
                    for (const auto &[key, _] : row.fields())
                        columns.push_back(key);
                    break;
                }
            }
        }

        std::string out;
        if (options.header && !columns.empty())
        {
            for (std::size_t i = 0; i < columns.size(); ++i)
            {
                if (i)
                    out += options.delimiter;
                out += escapeCell(columns[i], options.delimiter);
            }
            out += options.eol;
        }

        for (const auto &row : rows)
        {
            if (row.isArray())
            {
                const auto &items = row.items();
                for (std::size_t i = 0; i < items.size(); ++i)
                {
                    if (i)
                        out += options.delimiter;
                    out += escapeCell(cellToText(items[i]), options.delimiter);
                }
            }
            else
            {
                for (std::size_t i = 0; i < columns.size(); ++i)
                {
                    if (i)
                        out += options.delimiter;
                    out += escapeCell(cellToText(row[columns[i]]), options.delimiter);
                }
            }
            out += options.eol;
        }
        return out;
    }

    std::string writeRows(const std::vector<Row> &rows, const WriteOptions &options)
    {
        std::string out;
        for (const auto &row : rows)
        {
            for (std::size_t i = 0; i < row.size(); ++i)
            {
                if (i)
                    out += options.delimiter;
                out += escapeCell(row[i], options.delimiter);
            }
            out += options.eol;
        }
        return out;
    }

    json::Value coerceCell(std::string_view value)
    {
        const std::string text = str::trim(value);
        if (text.empty())
            return json::Value();
        if (str::iequals(text, "null"))
            return json::Value();
        if (str::iequals(text, "true"))
            return json::Value(true);
        if (str::iequals(text, "false"))
            return json::Value(false);

        // Integers first, so "007" style values stay strings only when they cannot
        // be represented (leading zeros make them identifiers, not numbers).
        const bool leadingZero = text.size() > 1 && text[0] == '0' && text[1] != '.';
        if (!leadingZero)
        {
            if (const auto asInt = str::toInt(text))
                return json::Value(*asInt);
            if (const auto asDouble = str::toDouble(text))
                return json::Value(*asDouble);
        }
        return json::Value(text);
    }

    std::vector<std::string> parseHeader(std::string_view line, char delimiter)
    {
        const std::vector<Row> rows = parse(line, ParseOptions{delimiter, false, false});
        if (rows.empty())
            return {};
        std::vector<std::string> headers = rows[0];
        for (auto &header : headers)
            header = str::trim(header);
        return headers;
    }

} // namespace mf::csv
