#include "mf/util/table.hpp"

#include "mf/util/strings.hpp"

#include <algorithm>
#include <sstream>

namespace mf::table
{

    std::string valueToText(const json::Value &value)
    {
        switch (value.type())
        {
        case json::Type::Null:
            return "";
        case json::Type::Bool:
            return value.asBool() ? "true" : "false";
        case json::Type::Number:
        {
            const double d = value.asNumber();
            if (value.isInteger())
                return std::to_string(value.asInt());
            std::ostringstream stream;
            stream << d;
            return stream.str();
        }
        case json::Type::String:
            return value.asString();
        case json::Type::Array:
        case json::Type::Object:
            return value.dumpCompact();
        }
        return "";
    }

    std::string cell(std::string_view value, std::size_t width)
    {
        const std::string text = str::truncate(value, width, "\xE2\x80\xA6"); // U+2026
        if (text.size() >= width)
            return text;
        return text + std::string(width - text.size(), ' ');
    }

    std::string render(const json::Array &rows, const RenderOptions &options)
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
        if (columns.empty())
            return "(no columns)";

        std::map<std::string, std::size_t> widths;
        for (const auto &column : columns)
        {
            const auto override = options.widths.find(column);
            if (override != options.widths.end())
            {
                widths[column] = override->second;
                continue;
            }
            std::size_t longest = column.size();
            for (const auto &row : rows)
            {
                longest = std::max(longest, valueToText(row[column]).size());
            }
            widths[column] = std::min(std::max<std::size_t>(longest, 4), options.defaultWidth + 8);
        }

        std::vector<std::string> lines;
        if (!options.title.empty())
        {
            lines.push_back(options.title);
            lines.push_back(std::string(std::min(options.title.size(), options.maxWidth), '-'));
        }

        std::string header;
        std::string rule;
        for (std::size_t i = 0; i < columns.size(); ++i)
        {
            if (i)
            {
                header += " | ";
                rule += "-+-";
            }
            const std::string label = options.uppercaseHeaders ? str::upper(columns[i]) : columns[i];
            header += cell(label, widths[columns[i]]);
            rule += std::string(widths[columns[i]], '-');
        }
        lines.push_back(header);
        lines.push_back(rule);

        const std::size_t limit = options.limit > 0 ? options.limit : rows.size();
        const std::size_t shown = std::min(limit, rows.size());
        for (std::size_t r = 0; r < shown; ++r)
        {
            std::string line;
            for (std::size_t c = 0; c < columns.size(); ++c)
            {
                if (c)
                    line += " | ";
                line += cell(valueToText(rows[r][columns[c]]), widths[columns[c]]);
            }
            lines.push_back(line);
        }

        if (shown < rows.size())
        {
            lines.push_back("... " + std::to_string(rows.size() - shown) + " more row(s) not shown");
        }

        std::string out;
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            if (i)
                out += '\n';
            out += lines[i];
        }
        return out;
    }

    std::string renderPanel(const std::vector<std::pair<std::string, std::string>> &pairs,
                            const std::string &title)
    {
        std::vector<std::string> lines;
        if (!title.empty())
        {
            lines.push_back(title);
            lines.push_back(std::string(std::min<std::size_t>(title.size(), 79), '-'));
        }

        std::size_t labelWidth = 0;
        for (const auto &[key, _] : pairs)
            labelWidth = std::max(labelWidth, key.size());

        for (const auto &[key, value] : pairs)
        {
            lines.push_back(str::padRight(str::upper(key), labelWidth, ' ') + " . . : " + value);
        }

        std::string out;
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            if (i)
                out += '\n';
            out += lines[i];
        }
        return out;
    }

    std::string renderPanel(const json::Value &object, const std::string &title)
    {
        std::vector<std::pair<std::string, std::string>> pairs;
        if (object.isObject())
        {
            for (const auto &[key, value] : object.fields())
            {
                pairs.emplace_back(key, valueToText(value));
            }
        }
        return renderPanel(pairs, title);
    }

    std::string renderBanner(const std::vector<std::string> &lines, std::size_t width)
    {
        std::size_t contentWidth = 0;
        for (const auto &line : lines)
            contentWidth = std::max(contentWidth, line.size());
        const std::size_t inner = std::min(width - 2, contentWidth + 2);

        std::string out;
        out += "+" + std::string(inner, '=') + "+\n";
        for (const auto &line : lines)
        {
            out += "| " + str::padRight(line, inner - 2, ' ') + " |\n";
        }
        out += "+" + std::string(inner, '=') + "+";
        return out;
    }

} // namespace mf::table
