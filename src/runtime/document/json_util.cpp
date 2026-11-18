#include "document/json_util.hpp"

#include <praenora/runtime/error.hpp>

#include <algorithm>
#include <limits>
#include <vector>

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

std::optional<std::uint32_t> uint32_of(const nlohmann::json& value) {
    constexpr auto kMax = std::numeric_limits<std::uint32_t>::max();
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number <= kMax) {
            return static_cast<std::uint32_t>(number);
        }
    } else if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        if (number >= 0 && number <= std::int64_t{kMax}) {
            return static_cast<std::uint32_t>(number);
        }
    }
    return std::nullopt;
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

std::string replace_invalid_utf8(std::string_view text) {
    if (is_valid_utf8(text)) {
        return std::string(text);
    }
    // nlohmann/json's serializer replaces what is not UTF-8; reading its output back gives the
    // text itself.
    const std::string quoted = nlohmann::json(std::string(text))
                                   .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    return nlohmann::json::parse(quoted).get<std::string>();
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

namespace {

bool is_white(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool is_flow_indicator(char c) { return c == ',' || c == '[' || c == ']' || c == '{' || c == '}'; }

// A character of a tag after its first '!': an ASCII word character, or a URI character other
// than a flow indicator.
bool is_tag_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           std::string_view("-%!#;/?:@&=+$_.~*'()").find(c) != std::string_view::npos;
}

// The scan follows block collections this deep at most. An accepted document nests at most
// kMaxNestingDepth deep, and yaml-cpp gives up long before this, so past it the scan just counts
// every opening bracket, and its memory stays small.
constexpr std::size_t kMaxBlockIndents = 4096;

// Counts the brackets of YAML text (JSON is YAML too) outside quoted scalars and comments, so that
// as long as it splits the text into tokens as yaml-cpp, the parser the scan protects, does, its
// count is never below the flow nesting yaml-cpp will find. Brackets inside plain and
// block scalars count too, though yaml-cpp reads them as text: one bracket too many is safe, one
// too few is not.
//
// A quote opens a quoted scalar only where a token starts; inside a plain or block scalar, an
// anchor or a tag it is text. So the scan splits the text into tokens as yaml-cpp does. Where a
// plain or a block scalar ends depends on indentation, so it follows yaml-cpp's indentation levels
// too, including those it opens for a possible key. In
//
//     key: it's
//       'still the same scalar
//
// the second line continues the plain scalar, because it is indented past the mapping.
class NestingScanner {
public:
    NestingScanner(std::string_view text, int max_depth) : text_(text), max_depth_(max_depth) {
        // Like yaml-cpp, count columns and lines by the kind of the first line end in the text: a
        // lone "\r" makes "\r" the line end, otherwise "\n" is. Either way, all three kinds end
        // tokens.
        const auto first = text_.find_first_of("\r\n");
        if (first != std::string_view::npos && at_break(first) && text_[first] == '\r') {
            line_end_ = '\r';
        }
        // Text with a NUL byte is not UTF-8: yaml-cpp reads UTF-16 and UTF-32, where every
        // bracket has one, and the scan cannot split those into tokens.
        raw_ = text_.find('\0') != std::string_view::npos;
    }

    std::optional<std::size_t> run() {
        while (i_ < text_.size() && !found_) {
            const char c = text_[i_];
            if (raw_) {
                for (; i_ < text_.size(); ++i_) {
                    if ((text_[i_] == '[' || text_[i_] == '{') && count(i_)) {
                        break;
                    }
                }
            } else if (at_break(i_)) {
                ++i_;
                if (!in_flow()) {
                    invalidate_key(); // a key does not span lines
                    key_allowed_ = true;
                }
            } else if (is_white(c)) {
                if (c == '\t' && !in_flow()) {
                    key_allowed_ = false; // yaml-cpp starts no key after a tab
                }
                ++i_;
            } else if (c == '#') {
                skip_comment(); // even one right after a token, as in [a]#b
            } else {
                scan_token();
            }
        }
        return found_;
    }

private:
    // A block collection's indentation level, as yaml-cpp keeps them: at the column of its first
    // key or entry, or of a possible key that may yet turn out not to be one.
    struct BlockIndent {
        std::int64_t column;
        bool sequence;
        bool possible; // a possible key's level, before the key is confirmed
        bool invalid;  // a possible key's level, after the key turned out not to be one
    };

    bool in_flow() const { return depth_ > 0; }

    // A line ends at "\n", at "\r\n" or at a lone "\r".
    bool at_break(std::size_t offset) const {
        return text_[offset] == '\n' ||
               (text_[offset] == '\r' && (offset + 1 == text_.size() || text_[offset + 1] != '\n'));
    }

    // Whether the line, or the text, ends at `offset`.
    bool line_ends_at(std::size_t offset) const {
        return offset == text_.size() || text_[offset] == '\n' || text_[offset] == '\r';
    }

    // The column of `offset`, which may not be before any offset asked for earlier.
    std::int64_t column(std::size_t offset) {
        for (; columns_seen_ < offset; ++columns_seen_) {
            if (text_[columns_seen_] == line_end_) {
                line_start_ = columns_seen_ + 1;
            }
        }
        return static_cast<std::int64_t>(offset - line_start_);
    }

    // The column of the innermost level; -1 at the top level.
    std::int64_t block_indent() const { return indents_.empty() ? -1 : indents_.back().column; }

    // Whether whitespace or the end of the text follows `offset`.
    bool separated_after(std::size_t offset) const {
        return offset + 1 >= text_.size() || is_white(text_[offset + 1]);
    }

    // "---" or "...", then whitespace or the end of the text; only a marker at column 0.
    bool is_document_marker(std::size_t offset) const {
        const auto marker = text_.substr(offset, 3);
        return (marker == "---" || marker == "...") && separated_after(offset + 2);
    }

    // Counts the character at `offset` if it is a bracket. Returns true, with found_ set, when it
    // opens a level beyond max_depth_. A closing bracket that closes no flow collection does not
    // take the count below the number of those that are open.
    bool count(std::size_t offset) {
        const char c = text_[offset];
        if (c == '[' || c == '{') {
            if (++count_ > max_depth_) {
                found_ = offset;
                return true;
            }
        } else if ((c == ']' || c == '}') && count_ > depth_) {
            --count_;
        }
        return false;
    }

    void skip_comment() {
        while (i_ < text_.size() && !at_break(i_)) {
            ++i_;
        }
    }

    // The rest of a line that holds no token the scan follows: its brackets count.
    void count_to_line_end() {
        for (; i_ < text_.size() && !at_break(i_); ++i_) {
            if (count(i_)) {
                return;
            }
        }
    }

    // Opens a level at `column` unless the innermost one is there or further right already (a
    // sequence may open at the column of the mapping it is a value of). Returns whether it did.
    // A possible key's level is `possible` until a colon confirms the key.
    bool push_indent(std::int64_t column, bool sequence, bool possible = false) {
        if (!indents_.empty()) {
            const BlockIndent& top = indents_.back();
            if (column < top.column || (column == top.column && !(sequence && !top.sequence))) {
                return false;
            }
        }
        if (indents_.size() == kMaxBlockIndents) {
            raw_ = true;
            return false;
        }
        indents_.push_back({column, sequence, possible, false});
        return true;
    }

    // A token ends the levels right of it, and a sequence at its own column unless it is an entry
    // of that sequence; then the levels of keys that turned out not to be keys.
    void pop_indents(std::int64_t column, bool entry) {
        while (!indents_.empty() &&
               (indents_.back().column > column ||
                (indents_.back().column == column && indents_.back().sequence && !entry))) {
            pop_indent();
        }
        while (!indents_.empty() && indents_.back().invalid) {
            pop_indent();
        }
    }

    // Like yaml-cpp, ending a level that no confirmed key opened ends the pending key, whichever
    // it is.
    void pop_indent() {
        const bool confirmed = !indents_.back().possible && !indents_.back().invalid;
        indents_.pop_back();
        if (!confirmed) {
            invalidate_key();
        }
    }

    // The node that starts at `column` may be the first key of a block mapping. Where a key may
    // start and none is pending, it becomes the pending key, and like yaml-cpp the scan opens its
    // mapping's level already.
    void start_node(std::int64_t column) {
        if (!in_flow() && key_allowed_ && !key_active_) {
            key_active_ = true;
            key_line_ = line_start_;
            key_offset_ = i_;
            key_indent_ = push_indent(column, false, true) ? indents_.size() - 1 : kNoIndent;
        }
        key_allowed_ = false;
    }

    // The pending key is not a key: its level ends at the next token, unless another level is open
    // inside it.
    void invalidate_key() {
        if (key_active_ && key_indent_ < indents_.size()) {
            indents_[key_indent_].invalid = true;
        }
        key_active_ = false;
        key_indent_ = kNoIndent;
    }

    // ": " in a block collection. A pending key that starts on this line, at most 1024 bytes back,
    // is a key, and its level stays open; no key may follow on this line. Otherwise a mapping opens
    // here, and a key may follow.
    void value_indicator(std::int64_t column) {
        if (key_active_ && key_line_ == line_start_ && i_ - key_offset_ <= 1024) {
            if (key_indent_ != kNoIndent) {
                indents_[key_indent_].possible = false;
            }
            key_active_ = false;
            key_indent_ = kNoIndent;
            key_allowed_ = false;
        } else {
            invalidate_key();
            push_indent(column, false);
            key_allowed_ = true;
        }
    }

    // Scans the token at i_.
    void scan_token() {
        const char c = text_[i_];
        const std::int64_t col = column(i_);
        if (col == 0 && (c == '%' || is_document_marker(i_))) {
            // A directive or a document marker ends every level and the pending key.
            indents_.clear();
            key_active_ = false;
            key_indent_ = kNoIndent;
            key_allowed_ = false;
            json_like_ = false;
            if (c == '%') {
                count_to_line_end();
            } else {
                i_ += 3;
            }
            return;
        }
        if (!in_flow()) {
            pop_indents(col, c == '-' && separated_after(i_));
        }
        // In a flow collection, ':' right after a quoted scalar or a collection is an indicator.
        const bool after_json_like = json_like_;
        json_like_ = false;
        switch (c) {
        case '[':
        case '{':
            start_node(col);
            ++depth_;
            if (!count(i_)) {
                ++i_;
            }
            return;
        case ']':
        case '}':
            if (depth_ > 0) {
                --depth_;
                --count_;
            } else {
                count(i_);
            }
            key_allowed_ = false;
            json_like_ = true;
            ++i_;
            return;
        case ',':
            ++i_;
            return;
        case '\'':
        case '"':
            start_node(col);
            skip_quoted(c);
            json_like_ = true;
            return;
        case '&': // an anchor
        case '*': // an alias
            start_node(col);
            for (++i_; i_ < text_.size() && !is_white(text_[i_]) && !is_flow_indicator(text_[i_]);
                 ++i_) {
            }
            return;
        case '!':
            start_node(col);
            skip_tag();
            return;
        case '|':
        case '>':
            if (!in_flow()) {
                skip_block_scalar();
                return;
            }
            break;
        case '-': // an entry of a block sequence
        case '?': // an explicit key
            if (separated_after(i_)) {
                if (!in_flow()) {
                    push_indent(col, c == '-');
                    key_allowed_ = true;
                }
                ++i_;
                return;
            }
            break;
        case ':':
            if (separated_after(i_) ||
                (in_flow() && (after_json_like || is_flow_indicator(text_[i_ + 1])))) {
                if (!in_flow()) {
                    value_indicator(col);
                }
                ++i_;
                return;
            }
            break;
        default:
            break;
        }
        // A plain scalar. Its lines must be indented past the level open where it starts.
        const std::int64_t min_indent = in_flow() ? 0 : block_indent() + 1;
        start_node(col);
        skip_plain_scalar(min_indent);
    }

    void skip_quoted(char quote) {
        for (++i_; i_ < text_.size(); ++i_) {
            if (quote == '"' && text_[i_] == '\\' && i_ + 1 < text_.size()) {
                ++i_; // the escaped character, which may be a line break
            } else if (text_[i_] == quote) {
                if (quote == '"' || i_ + 1 == text_.size() || text_[i_ + 1] != '\'') {
                    ++i_;
                    return;
                }
                ++i_; // '' is a quote
                continue;
            }
        }
    }

    void skip_tag() {
        ++i_;
        if (i_ < text_.size() && text_[i_] == '<') { // a verbatim tag, !<...>
            for (; i_ < text_.size() && text_[i_] != '>' && !is_white(text_[i_]); ++i_) {
                if (count(i_)) {
                    return;
                }
            }
            if (i_ < text_.size() && text_[i_] == '>') {
                ++i_;
            }
            return;
        }
        while (i_ < text_.size() && is_tag_char(text_[i_])) {
            ++i_;
        }
    }

    // A literal (|) or folded (>) scalar: the rest of its header line, then every empty line and
    // every line indented at least as far as the content. The header's indentation indicator
    // gives that indentation, counted from the innermost level; otherwise the first line that is
    // not empty does, and it is at least one column past that level.
    void skip_block_scalar() {
        const std::int64_t base = std::max<std::int64_t>(block_indent(), 0);
        std::int64_t indent = base + 1;
        bool detect = true;
        for (++i_; i_ < text_.size() &&
                   (text_[i_] == '+' || text_[i_] == '-' || (text_[i_] >= '0' && text_[i_] <= '9'));
             ++i_) {
            if (detect && text_[i_] >= '1' && text_[i_] <= '9') {
                indent = base + (text_[i_] - '0');
                detect = false;
            }
        }
        while (i_ < text_.size() && (text_[i_] == ' ' || text_[i_] == '\t')) {
            ++i_;
        }
        if (i_ < text_.size() && text_[i_] == '#') {
            skip_comment();
        } else {
            count_to_line_end(); // text the parser rejects
        }
        bool found_text = false;
        while (i_ < text_.size() && !found_) { // at a line break
            const std::size_t start = i_ + 1;
            std::size_t end = start;
            while (end < text_.size() && text_[end] == ' ') {
                ++end;
            }
            const std::int64_t line_indent = column(end);
            const bool empty = line_ends_at(end);
            if (detect && !found_text) {
                indent = std::max(indent, line_indent); // an empty line before the text counts too
            }
            if (!empty) {
                if (line_indent < indent) { // the first line after the scalar
                    i_ = end;
                    key_allowed_ = true;
                    return;
                }
                found_text = true;
            }
            i_ = end;
            count_to_line_end();
        }
    }

    // A plain scalar ends before ": " and " #", in a flow collection also before a flow
    // indicator, and at the end of a line that does not continue it.
    void skip_plain_scalar(std::int64_t min_indent) {
        while (i_ < text_.size()) {
            const char c = text_[i_];
            if (at_break(i_)) {
                if (!continue_plain_scalar(min_indent)) {
                    key_allowed_ = true;
                    return;
                }
            } else if ((c == ':' &&
                        (separated_after(i_) || (in_flow() && is_flow_indicator(text_[i_ + 1])))) ||
                       (c == '#' && i_ > 0 && (text_[i_ - 1] == ' ' || text_[i_ - 1] == '\t')) ||
                       (in_flow() && is_flow_indicator(c))) {
                return;
            } else if (count(i_)) {
                return;
            } else {
                ++i_;
            }
        }
    }

    // At the end of a line of a plain scalar: moves past any empty lines to the first character of
    // the next line that is not empty, and returns whether that line continues the scalar. A line
    // indented less than `min_indent`, a comment and a document marker do not.
    // The scalar takes these line breaks, so they leave the pending key alone, unlike one between
    // tokens.
    bool continue_plain_scalar(std::int64_t min_indent) {
        std::size_t first = i_ + 1;
        for (;;) {
            while (first < text_.size() && is_white(text_[first]) && !at_break(first)) {
                ++first;
            }
            if (first == text_.size() || !at_break(first)) {
                break;
            }
            ++first;
        }
        i_ = first;
        if (first == text_.size()) {
            return false;
        }
        const std::int64_t indent = column(first);
        return indent >= min_indent && text_[first] != '#' &&
               !(indent == 0 && is_document_marker(first));
    }

    static constexpr std::size_t kNoIndent = static_cast<std::size_t>(-1);

    std::string_view text_;
    int max_depth_;
    std::size_t i_ = 0;
    char line_end_ = '\n';
    std::size_t line_start_ = 0; // where the line of the offsets column() was last asked for starts
    std::size_t columns_seen_ = 0;       // column() has looked for line ends before this offset
    std::optional<std::size_t> found_;   // the bracket one level too deep
    int depth_ = 0;                      // open flow collections; flow context when above 0
    int count_ = 0;                      // brackets counted: at least depth_
    std::vector<BlockIndent> indents_;   // open levels, innermost last
    bool raw_ = false;                   // counting every opening bracket and no closing one
    bool key_allowed_ = true;            // whether the next node may be a key
    bool key_active_ = false;            // whether a key is pending
    std::size_t key_line_ = 0;           // where the pending key's line starts
    std::size_t key_offset_ = 0;         // where the pending key starts
    std::size_t key_indent_ = kNoIndent; // the index of its level in indents_, if it opened one
    bool json_like_ = false;             // the last token was a quoted scalar or a collection's end
};

} // namespace

std::optional<std::size_t> find_excessive_nesting(std::string_view text, int max_depth) {
    return NestingScanner(text, max_depth).run();
}

} // namespace praenora::detail
