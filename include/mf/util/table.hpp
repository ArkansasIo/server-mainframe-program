// ---------------------------------------------------------------------------
// Fixed width text tables - the way an ISPF panel or a JCL listing presents
// data. Both the CLI tools and the 3270 gateway render through this.
// ---------------------------------------------------------------------------
#pragma once

#include <map>
#include <string>
#include <vector>

#include "mf/util/json.hpp"

namespace mf::table
{

    struct RenderOptions
    {
        std::vector<std::string> columns;          // explicit column order (optional)
        std::map<std::string, std::size_t> widths; // per column width override
        std::string title;                         // optional heading
        std::size_t maxWidth = 132;                // a real 3270 line width
        std::size_t limit = 0;                     // 0 == all rows
        std::size_t defaultWidth = 18;
        bool uppercaseHeaders = true;
    };

    // Render an array of objects as a fixed width table.
    std::string render(const json::Array &rows, const RenderOptions &options = {});

    // Render a single object as a key/value panel:
    //   SYSTEM NAME   . . : MAINFRAME-1
    std::string renderPanel(const json::Value &object, const std::string &title = "");

    // Render "key . . : value" lines from an ordered list of pairs.
    std::string renderPanel(const std::vector<std::pair<std::string, std::string>> &pairs,
                            const std::string &title = "");

    // Render a banner box used when the terminal connects.
    std::string renderBanner(const std::vector<std::string> &lines, std::size_t width = 79);

    // Pad/truncate a value for fixed width output.
    std::string cell(std::string_view value, std::size_t width);

    std::string valueToText(const json::Value &value);

} // namespace mf::table
