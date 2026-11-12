// Text tables for the praenora tool's human-readable output. Header-only.
#ifndef PRAENORA_CLI_TABLE_HPP
#define PRAENORA_CLI_TABLE_HPP

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace praenora::cli {

// The width of UTF-8 text in columns, counted as code points: every byte but a continuation byte
// (10xxxxxx) starts one. Wide East Asian characters, which take two columns, and combining marks,
// which take none, are not handled.
inline std::size_t display_width(const std::string& text) {
    std::size_t width = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u) {
            ++width;
        }
    }
    return width;
}

// Rows of text, one line each, every column but the last padded to its widest cell (by
// display_width()) plus two spaces; lines end without spaces.
inline std::string table(const std::vector<std::vector<std::string>>& rows) {
    std::vector<std::size_t> widths;
    for (const auto& row : rows) {
        widths.resize(std::max(widths.size(), row.size()));
        for (std::size_t i = 0; i < row.size(); ++i) {
            widths[i] = std::max(widths[i], display_width(row[i]));
        }
    }
    std::string text;
    for (const auto& row : rows) {
        std::string line;
        for (std::size_t i = 0; i < row.size(); ++i) {
            line += row[i];
            if (i + 1 < row.size()) {
                line += std::string(widths[i] - display_width(row[i]) + 2, ' ');
            }
        }
        line.erase(line.find_last_not_of(' ') + 1); // no padding after an empty last cell
        text += line + "\n";
    }
    return text;
}

} // namespace praenora::cli

#endif // PRAENORA_CLI_TABLE_HPP
