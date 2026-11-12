#include "document/json_util.hpp"

#include <praenora/runtime/error.hpp>

#include <algorithm>

namespace praenora::detail {

namespace {

// nlohmann/json messages look like "[json.exception.parse_error.101] parse error at line 3,
// column 8: syntax error while parsing value - ..."; keep only the description.
std::string describe(const nlohmann::json::exception& e) {
    std::string message = e.what();
    if (!message.empty() && message[0] == '[') {
        const auto close = message.find("] ");
        if (close != std::string::npos) {
            message.erase(0, close + 2);
        }
    }
    const auto column = message.find(", column ");
    if (column != std::string::npos) {
        const auto colon = message.find(": ", column);
        if (colon != std::string::npos) {
            message.erase(0, colon + 2);
        }
    }
    return message;
}

SourceLocation at(std::string_view origin, std::string_view text, std::size_t offset) {
    const auto position = text_position(text, offset);
    SourceLocation location;
    location.file = std::string(origin);
    location.line = position.line;
    location.column = position.column;
    return location;
}

} // namespace

std::string canonical_json(const nlohmann::json& value) {
    try {
        return value.dump();
    } catch (const nlohmann::json::exception& e) {
        throw Error(ErrorCode::DATA_INVALID, "cannot serialize JSON: " + describe(e));
    }
}

nlohmann::json parse_json_text(std::string_view text, std::string_view origin) {
    if (const auto offset = find_excessive_nesting(text, kMaxNestingDepth)) {
        throw Error(ErrorCode::PARSE_ERROR,
                    "nesting too deep: more than " + std::to_string(kMaxNestingDepth) + " levels",
                    at(origin, text, *offset));
    }
    try {
        return nlohmann::json::parse(text.begin(), text.end());
    } catch (const nlohmann::json::parse_error& e) {
        // `byte` counts the characters read, so the problem is at byte - 1.
        const std::size_t offset = e.byte == 0 ? 0 : std::min(e.byte - 1, text.size());
        throw Error(ErrorCode::PARSE_ERROR, "invalid JSON: " + describe(e),
                    at(origin, text, offset));
    } catch (const nlohmann::json::exception& e) {
        SourceLocation location;
        location.file = std::string(origin);
        throw Error(ErrorCode::PARSE_ERROR, "invalid JSON: " + describe(e), location);
    }
}

std::string json_pointer_escape(std::string_view token) {
    std::string escaped;
    escaped.reserve(token.size());
    for (const char c : token) {
        if (c == '~') {
            escaped += "~0";
        } else if (c == '/') {
            escaped += "~1";
        } else {
            escaped += c;
        }
    }
    return escaped;
}

bool is_valid_utf8(std::string_view text) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t size = text.size();
    std::size_t i = 0;
    while (i < size) {
        const unsigned char lead = bytes[i];
        if (lead < 0x80) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        unsigned char low = 0x80, high = 0xBF; // allowed range of the second byte
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead == 0xE0) {
            length = 3;
            low = 0xA0; // no overlong forms
        } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
            length = 3;
        } else if (lead == 0xED) {
            length = 3;
            high = 0x9F; // no surrogates
        } else if (lead == 0xF0) {
            length = 4;
            low = 0x90;
        } else if (lead >= 0xF1 && lead <= 0xF3) {
            length = 4;
        } else if (lead == 0xF4) {
            length = 4;
            high = 0x8F; // nothing above U+10FFFF
        } else {
            return false;
        }
        if (i + length > size) {
            return false;
        }
        if (bytes[i + 1] < low || bytes[i + 1] > high) {
            return false;
        }
        for (std::size_t k = 2; k < length; ++k) {
            if (bytes[i + k] < 0x80 || bytes[i + k] > 0xBF) {
                return false;
            }
        }
        i += length;
    }
    return true;
}

std::optional<double> parse_decimal(std::string_view text) {
    const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    const auto digits_from = [&](std::size_t i) {
        while (i < text.size() && is_digit(text[i])) {
            ++i;
        }
        return i;
    };
    std::size_t i = 0;
    const bool negative = i < text.size() && text[i] == '-';
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
        ++i;
    }
    std::size_t end = digits_from(i);
    std::string_view whole = text.substr(i, end - i);
    i = end;
    std::string_view fraction;
    bool point = false;
    if (i < text.size() && text[i] == '.') {
        point = true;
        end = digits_from(i + 1);
        fraction = text.substr(i + 1, end - i - 1);
        i = end;
    }
    if (whole.empty() && (!point || fraction.empty())) {
        return std::nullopt;
    }
    std::string_view exponent;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        const std::size_t start = i + 1;
        std::size_t digits = start;
        if (digits < text.size() && (text[digits] == '-' || text[digits] == '+')) {
            ++digits;
        }
        end = digits_from(digits);
        if (end == digits) {
            return std::nullopt;
        }
        exponent = text.substr(start, end - start);
        i = end;
    }
    if (i != text.size()) {
        return std::nullopt;
    }
    // Rewritten in JSON's number syntax (no '+', no leading zeros, digits on both sides of the
    // point) for nlohmann/json's parser, which reads numbers the same way in every locale.
    while (whole.size() > 1 && whole[0] == '0') {
        whole.remove_prefix(1);
    }
    std::string normalized = negative ? "-" : "";
    normalized += whole.empty() ? std::string("0") : std::string(whole);
    normalized += "." + (fraction.empty() ? std::string("0") : std::string(fraction));
    if (!exponent.empty()) {
        normalized += "e" + std::string(exponent);
    }
    const auto value = nlohmann::json::parse(normalized, nullptr, false);
    if (!value.is_number()) {
        return std::nullopt; // also a value beyond the range of a double
    }
    return value.get<double>();
}

TextPosition text_position(std::string_view text, std::size_t offset) {
    offset = std::min(offset, text.size());
    TextPosition position;
    std::size_t line_start = 0;
    for (std::size_t i = 0; i < offset; ++i) {
        if (text[i] == '\n') {
            ++position.line;
            line_start = i + 1;
        }
    }
    position.column = static_cast<std::int64_t>(offset - line_start) + 1;
    return position;
}

std::optional<std::size_t> find_excessive_nesting(std::string_view text, int max_depth) {
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    // A quote starts a string only where a token can start, so the apostrophe in a YAML plain
    // scalar such as don't does not.
    const auto starts_token = [&](std::size_t i) {
        if (i == 0) {
            return true;
        }
        const char before = text[i - 1];
        return is_space(before) || before == '[' || before == '{' || before == ',' || before == ':';
    };
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        switch (text[i]) {
        case '"':
            if (starts_token(i)) {
                for (++i; i < text.size() && text[i] != '"'; ++i) {
                    if (text[i] == '\\') {
                        ++i;
                    }
                }
            }
            break;
        case '\'':
            if (starts_token(i)) {
                for (++i; i < text.size(); ++i) {
                    if (text[i] == '\'') {
                        if (i + 1 < text.size() && text[i + 1] == '\'') {
                            ++i; // '' is an escaped quote
                        } else {
                            break;
                        }
                    }
                }
            }
            break;
        case '#':
            if (i == 0 || is_space(text[i - 1])) {
                while (i + 1 < text.size() && text[i + 1] != '\n') {
                    ++i;
                }
            }
            break;
        case '[':
        case '{':
            if (++depth > max_depth) {
                return i;
            }
            break;
        case ']':
        case '}':
            if (depth > 0) {
                --depth;
            }
            break;
        default:
            break;
        }
    }
    return std::nullopt;
}

} // namespace praenora::detail
