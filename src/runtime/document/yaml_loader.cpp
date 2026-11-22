#include "document/yaml_loader.hpp"

#include "core/paths.hpp"
#include "document/json_util.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

namespace praenora::detail {

namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";
constexpr std::string_view kTagPrefix = "tag:yaml.org,2002:";

SourceLocation make_location(const std::string& origin, const SourcePosition& position,
                             const std::string& pointer) {
    SourceLocation location;
    location.file = origin;
    location.line = position.line;
    location.column = position.column;
    location.pointer = pointer;
    return location;
}

[[noreturn]] void fail(const std::string& origin, const std::string& message,
                       const SourcePosition& position, const std::string& pointer) {
    throw Error(ErrorCode::PARSE_ERROR, message, make_location(origin, position, pointer));
}

std::string nesting_message() {
    return "nesting too deep: more than " + std::to_string(kMaxNestingDepth) + " levels";
}

// A scalar's text for messages: quoted and shortened.
std::string shown(std::string_view text) {
    constexpr std::size_t kMax = 64;
    if (text.size() > kMax) {
        return "'" + std::string(text.substr(0, kMax)) + "...'";
    }
    return "'" + std::string(text) + "'";
}

std::string shown_tag(const std::string& tag) {
    if (tag.compare(0, kTagPrefix.size(), kTagPrefix) == 0) {
        return "!!" + tag.substr(kTagPrefix.size());
    }
    return tag;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool all_of(std::string_view s, bool (*predicate)(char)) {
    if (s.empty()) {
        return false;
    }
    for (const char c : s) {
        if (!predicate(c)) {
            return false;
        }
    }
    return true;
}

bool is_octal_digit(char c) { return c >= '0' && c <= '7'; }

bool is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_null_text(std::string_view s) {
    return s.empty() || s == "~" || s == "null" || s == "Null" || s == "NULL";
}

std::optional<bool> bool_value(std::string_view s) {
    if (s == "true" || s == "True" || s == "TRUE") {
        return true;
    }
    if (s == "false" || s == "False" || s == "FALSE") {
        return false;
    }
    return std::nullopt;
}

std::string_view without_sign(std::string_view s) {
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
        s.remove_prefix(1);
    }
    return s;
}

// [-+]?[0-9]+
bool is_decimal_integer(std::string_view s) { return all_of(without_sign(s), is_digit); }

// 0o[0-7]+
bool is_octal_integer(std::string_view s) {
    return s.size() > 2 && s.compare(0, 2, "0o") == 0 && all_of(s.substr(2), is_octal_digit);
}

// 0x[0-9a-fA-F]+
bool is_hex_integer(std::string_view s) {
    return s.size() > 2 && s.compare(0, 2, "0x") == 0 && all_of(s.substr(2), is_hex_digit);
}

bool is_integer(std::string_view s) {
    return is_decimal_integer(s) || is_octal_integer(s) || is_hex_integer(s);
}

// [-+]?(\.(inf|Inf|INF)) or \.(nan|NaN|NAN)
bool is_infinity_or_nan(std::string_view s) {
    const auto unsigned_part = without_sign(s);
    if (unsigned_part == ".inf" || unsigned_part == ".Inf" || unsigned_part == ".INF") {
        return true;
    }
    return s == ".nan" || s == ".NaN" || s == ".NAN";
}

struct FloatParts {
    bool negative = false;
    std::string_view whole, fraction, exponent; // exponent: digits with an optional sign
    bool has_exponent = false;
};

// [-+]?(\.[0-9]+|[0-9]+(\.[0-9]*)?)([eE][-+]?[0-9]+)?
std::optional<FloatParts> split_float(std::string_view s) {
    FloatParts parts;
    std::size_t i = 0;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
        parts.negative = s[i] == '-';
        ++i;
    }
    const std::size_t whole_start = i;
    while (i < s.size() && is_digit(s[i])) {
        ++i;
    }
    parts.whole = s.substr(whole_start, i - whole_start);
    bool has_point = false;
    if (i < s.size() && s[i] == '.') {
        has_point = true;
        const std::size_t fraction_start = ++i;
        while (i < s.size() && is_digit(s[i])) {
            ++i;
        }
        parts.fraction = s.substr(fraction_start, i - fraction_start);
    }
    if (parts.whole.empty() && (!has_point || parts.fraction.empty())) {
        return std::nullopt;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        const std::size_t exponent_start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
            ++i;
        }
        const std::size_t digits_start = i;
        while (i < s.size() && is_digit(s[i])) {
            ++i;
        }
        if (i == digits_start) {
            return std::nullopt;
        }
        parts.exponent = s.substr(exponent_start, i - exponent_start);
        parts.has_exponent = true;
    }
    if (i != s.size()) {
        return std::nullopt;
    }
    return parts;
}

std::string_view strip_leading_zeros(std::string_view digits) {
    while (digits.size() > 1 && digits[0] == '0') {
        digits.remove_prefix(1);
    }
    return digits;
}

// Converts and checks scalars on behalf of one document.
class ScalarResolver {
public:
    ScalarResolver(const std::string& origin, const SourcePosition& position,
                   const std::string& pointer)
        : origin_(origin), position_(position), pointer_(pointer) {}

    [[noreturn]] void error(const std::string& message) const {
        fail(origin_, message, position_, pointer_);
    }

    nlohmann::json resolve_plain(const std::string& text) const {
        if (is_null_text(text)) {
            return nullptr;
        }
        if (const auto b = bool_value(text)) {
            return *b;
        }
        if (is_integer(text)) {
            return integer(text);
        }
        if (is_infinity_or_nan(text)) {
            error("infinity and NaN (" + shown(text) + ") cannot be represented in JSON");
        }
        if (split_float(text)) {
            return floating(text);
        }
        return text;
    }

    nlohmann::json tagged(const std::string& tag, const std::string& text) const {
        const std::string name = tag.substr(kTagPrefix.size());
        if (name == "str") {
            return text;
        }
        if (name == "int") {
            if (!is_integer(text)) {
                error(shown(text) + " is not an integer (tag !!int)");
            }
            return integer(text);
        }
        if (name == "float") {
            if (is_infinity_or_nan(text)) {
                error("infinity and NaN (" + shown(text) + ") cannot be represented in JSON");
            }
            if (!split_float(text)) {
                error(shown(text) + " is not a number (tag !!float)");
            }
            return floating(text);
        }
        if (name == "bool") {
            const auto b = bool_value(text);
            if (!b) {
                error(shown(text) + " is not true or false (tag !!bool)");
            }
            return *b;
        }
        if (name == "null") {
            if (!is_null_text(text)) {
                error(shown(text) + " is not null (tag !!null)");
            }
            return nullptr;
        }
        error("unsupported tag '" + shown_tag(tag) + "'");
    }

private:
    nlohmann::json integer(const std::string& text) const {
        if (is_octal_integer(text) || is_hex_integer(text)) {
            const std::uint64_t radix = text[1] == 'o' ? 8 : 16;
            std::uint64_t value = 0;
            for (const char c : std::string_view(text).substr(2)) {
                const std::uint64_t digit = is_digit(c)              ? std::uint64_t(c - '0')
                                            : (c >= 'a' && c <= 'f') ? std::uint64_t(c - 'a' + 10)
                                                                     : std::uint64_t(c - 'A' + 10);
                if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / radix) {
                    error("integer " + shown(text) + " is out of range");
                }
                value = value * radix + digit;
            }
            return value;
        }
        // Decimal: normalized to JSON syntax (no '+', no leading zeros), then read by the JSON
        // parser, which is locale-independent. Integers beyond 64 bits come back as floats.
        const bool negative = text[0] == '-';
        const std::string normalized =
            (negative ? "-" : "") + std::string(strip_leading_zeros(without_sign(text)));
        const auto value = nlohmann::json::parse(normalized, nullptr, false);
        if (!value.is_number_integer()) {
            error("integer " + shown(text) + " is out of range");
        }
        return value;
    }

    nlohmann::json floating(const std::string& text) const {
        const auto parts = split_float(text);
        std::string normalized = parts->negative ? "-" : "";
        normalized += parts->whole.empty() ? "0" : std::string(strip_leading_zeros(parts->whole));
        // Always a fraction, so the JSON parser yields a floating-point number.
        normalized +=
            "." + (parts->fraction.empty() ? std::string("0") : std::string(parts->fraction));
        if (parts->has_exponent) {
            normalized += "e" + std::string(parts->exponent);
        }
        try {
            auto value = nlohmann::json::parse(normalized);
            if (!value.is_number_float()) {
                error("number " + shown(text) + " is out of range");
            }
            return value;
        } catch (const nlohmann::json::exception&) {
            error("number " + shown(text) + " is out of range");
        }
    }

    const std::string& origin_;
    const SourcePosition& position_;
    const std::string& pointer_;
};

// Converts a yaml-cpp node graph into JSON and fills the source map.
class YamlConverter {
public:
    YamlConverter(std::string_view text, const std::string& origin, SourceMap& positions)
        : text_(text), origin_(origin), positions_(positions) {}

    // `entry_position` is the key's position when the node is the value of a mapping entry.
    // `fallback` locates empty values that have no position of their own.
    nlohmann::json convert(const YAML::Node& node, const std::string& pointer, int depth,
                           const SourcePosition* entry_position, const SourcePosition& fallback) {
        const SourcePosition own = position_of(node, fallback);
        switch (node.Type()) {
        case YAML::NodeType::Null: {
            // An empty value has no text of its own: locate it at its key or parent.
            const bool explicit_null = is_null_literal_at(node.Mark());
            positions_[pointer] =
                explicit_null ? own : (entry_position != nullptr ? *entry_position : fallback);
            // A tagged empty value reads like a tagged empty scalar.
            const std::string& tag = node.Tag();
            if (tag.empty() || tag == "?") {
                return nullptr;
            }
            if (tag == "!") {
                return std::string();
            }
            const ScalarResolver resolver(origin_, own, pointer);
            if (tag.compare(0, kTagPrefix.size(), kTagPrefix) == 0) {
                return resolver.tagged(tag, std::string());
            }
            resolver.error("unsupported tag '" + shown_tag(tag) + "'");
        }
        case YAML::NodeType::Scalar:
            positions_[pointer] = own;
            return scalar(node, own, pointer);
        case YAML::NodeType::Sequence:
        case YAML::NodeType::Map:
            break;
        case YAML::NodeType::Undefined:
        default:
            fail(origin_, "unsupported YAML node", own, pointer);
        }

        positions_[pointer] = entry_position != nullptr ? *entry_position : own;
        const std::string& tag = node.Tag();
        if (!tag.empty() && tag != "?") {
            fail(origin_, "unsupported tag '" + shown_tag(tag) + "'", own, pointer);
        }
        if (depth + 1 > kMaxNestingDepth) {
            fail(origin_, nesting_message(), own, pointer);
        }
        if (node.IsSequence()) {
            auto array = nlohmann::json::array();
            std::size_t index = 0;
            for (const auto& item : node) {
                array.push_back(convert(item, pointer + "/" + std::to_string(index++), depth + 1,
                                        nullptr, own));
            }
            return array;
        }
        auto object = nlohmann::json::object();
        for (const auto& entry : node) {
            const YAML::Node& key = entry.first;
            const SourcePosition key_position = position_of(key, own);
            const std::string name = key_name(key, key_position, pointer);
            const std::string child = pointer + "/" + json_pointer_escape(name);
            if (object.contains(name)) {
                fail(origin_, "duplicate key " + shown(name), key_position, child);
            }
            object[name] = convert(entry.second, child, depth + 1, &key_position, key_position);
        }
        return object;
    }

    bool is_null_literal_at(const YAML::Mark& mark) const {
        if (mark.is_null() || mark.pos < 0 || static_cast<std::size_t>(mark.pos) >= text_.size()) {
            return false;
        }
        const std::string_view rest = text_.substr(static_cast<std::size_t>(mark.pos));
        for (const std::string_view literal : {"~", "null", "Null", "NULL"}) {
            if (rest.compare(0, literal.size(), literal) == 0) {
                if (rest.size() == literal.size()) {
                    return true;
                }
                const char next = rest[literal.size()];
                if (next == ' ' || next == '\t' || next == '\r' || next == '\n' || next == ',' ||
                    next == ']' || next == '}' || next == '#') {
                    return true;
                }
            }
        }
        return false;
    }

private:
    static SourcePosition position_of(const YAML::Node& node, const SourcePosition& fallback) {
        const YAML::Mark mark = node.Mark();
        if (mark.is_null() || mark.line < 0 || mark.column < 0) {
            return fallback;
        }
        return SourcePosition{mark.line + 1, mark.column + 1};
    }

    std::string key_name(const YAML::Node& key, const SourcePosition& position,
                         const std::string& pointer) const {
        if (!key.IsScalar()) {
            fail(origin_,
                 key.IsNull() ? "a mapping key must be a string; quote it"
                              : "a mapping key must be a string, not a mapping or sequence",
                 position, pointer);
        }
        const std::string& tag = key.Tag();
        const std::string& text = key.Scalar();
        if (!is_valid_utf8(text)) {
            fail(origin_, "invalid UTF-8 in a mapping key", position, pointer);
        }
        if (tag == "?" && text == "<<") {
            fail(origin_, "merge keys (<<) are not supported", position,
                 pointer + "/" + json_pointer_escape(text));
        }
        if (!tag.empty() && tag != "?" && tag != "!" && tag != std::string(kTagPrefix) + "str") {
            fail(origin_, "unsupported tag '" + shown_tag(tag) + "' on a mapping key", position,
                 pointer);
        }
        return text;
    }

    nlohmann::json scalar(const YAML::Node& node, const SourcePosition& position,
                          const std::string& pointer) const {
        const std::string& text = node.Scalar();
        const std::string& tag = node.Tag();
        const ScalarResolver resolver(origin_, position, pointer);
        if (!is_valid_utf8(text)) {
            resolver.error("invalid UTF-8");
        }
        if (tag == "?") {
            return resolver.resolve_plain(text);
        }
        if (tag == "!" || tag.empty()) {
            return text;
        }
        if (tag.compare(0, kTagPrefix.size(), kTagPrefix) == 0) {
            return resolver.tagged(tag, text);
        }
        resolver.error("unsupported tag '" + shown_tag(tag) + "'");
    }

    std::string_view text_;
    const std::string& origin_;
    SourceMap& positions_;
};

// Records the positions of a document that the JSON parser has accepted, and rejects duplicate
// keys, which the JSON parser silently merges. The nesting depth was checked beforehand.
class JsonPositionScanner {
public:
    JsonPositionScanner(std::string_view text, const std::string& origin, SourceMap& positions)
        : text_(text), origin_(origin), positions_(positions) {}

    void run() {
        skip_whitespace();
        value("", nullptr);
    }

private:
    SourcePosition here() const {
        return SourcePosition{line_, static_cast<std::int64_t>(pos_ - line_start_) + 1};
    }

    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    [[noreturn]] void unexpected() const {
        fail(origin_, "invalid JSON: unexpected character", here(), "");
    }

    void skip_whitespace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == '\n') {
                ++line_;
                line_start_ = pos_ + 1;
            } else if (c != ' ' && c != '\t' && c != '\r') {
                return;
            }
            ++pos_;
        }
    }

    void value(const std::string& pointer, const SourcePosition* entry_position) {
        const char c = peek();
        const bool collection = c == '{' || c == '[';
        positions_[pointer] = (collection && entry_position != nullptr) ? *entry_position : here();
        if (c == '{') {
            object(pointer);
        } else if (c == '[') {
            array(pointer);
        } else if (c == '"') {
            string_token();
        } else {
            const std::size_t start = pos_;
            while (pos_ < text_.size() && text_[pos_] != ',' && text_[pos_] != ']' &&
                   text_[pos_] != '}' && text_[pos_] != ' ' && text_[pos_] != '\t' &&
                   text_[pos_] != '\r' && text_[pos_] != '\n') {
                ++pos_;
            }
            if (pos_ == start) {
                unexpected();
            }
        }
    }

    void object(const std::string& pointer) {
        ++pos_; // '{'
        std::set<std::string> keys;
        skip_whitespace();
        if (peek() == '}') {
            ++pos_;
            return;
        }
        while (true) {
            skip_whitespace();
            if (peek() != '"') {
                unexpected();
            }
            const SourcePosition key_position = here();
            const std::string key = string_token();
            const std::string child = pointer + "/" + json_pointer_escape(key);
            if (!keys.insert(key).second) {
                fail(origin_, "duplicate key " + shown(key), key_position, child);
            }
            skip_whitespace();
            if (peek() != ':') {
                unexpected();
            }
            ++pos_;
            skip_whitespace();
            value(child, &key_position);
            skip_whitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                return;
            }
            unexpected();
        }
    }

    void array(const std::string& pointer) {
        ++pos_; // '['
        skip_whitespace();
        if (peek() == ']') {
            ++pos_;
            return;
        }
        for (std::size_t index = 0;; ++index) {
            skip_whitespace();
            value(pointer + "/" + std::to_string(index), nullptr);
            skip_whitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                return;
            }
            unexpected();
        }
    }

    // Consumes a string token and returns its decoded value, decoded by the JSON parser itself.
    std::string string_token() {
        const std::size_t start = pos_++;
        while (pos_ < text_.size() && text_[pos_] != '"') {
            pos_ += text_[pos_] == '\\' ? 2 : 1;
        }
        if (pos_ >= text_.size()) {
            unexpected();
        }
        ++pos_; // closing quote
        const auto token = text_.substr(start, pos_ - start);
        return nlohmann::json::parse(token.begin(), token.end()).get<std::string>();
    }

    std::string_view text_;
    const std::string& origin_;
    SourceMap& positions_;
    std::size_t pos_ = 0;
    std::int64_t line_ = 1;
    std::size_t line_start_ = 0;
};

// Walks the node graph before anything is built. Aliases can make a small document expand into a
// huge one, and a recursive alias into an endless one, so the number of values and the nesting
// are checked on the expanded graph first, which is cheap: it stops at the first value too many.
class ExpansionChecker {
public:
    explicit ExpansionChecker(const std::string& origin) : origin_(origin) {}

    void check(const YAML::Node& node, int depth) {
        if (++values_ > kMaxDocumentValues) {
            fail(origin_,
                 "document too large: more than " + std::to_string(kMaxDocumentValues) +
                     " values after alias expansion",
                 position(node), pointer());
        }
        if (!node.IsMap() && !node.IsSequence()) {
            return;
        }
        if (depth + 1 > kMaxNestingDepth) {
            fail(origin_, nesting_message(), position(node), pointer());
        }
        if (node.IsSequence()) {
            std::size_t index = 0;
            for (const auto& item : node) {
                path_.push_back(std::to_string(index++));
                check(item, depth + 1);
                path_.pop_back();
            }
            return;
        }
        for (const auto& entry : node) {
            path_.push_back(entry.first.IsScalar() ? json_pointer_escape(entry.first.Scalar())
                                                   : std::string("?"));
            check(entry.second, depth + 1);
            path_.pop_back();
        }
    }

private:
    static SourcePosition position(const YAML::Node& node) {
        const YAML::Mark mark = node.Mark();
        if (mark.is_null() || mark.line < 0 || mark.column < 0) {
            return SourcePosition{};
        }
        return SourcePosition{mark.line + 1, mark.column + 1};
    }

    std::string pointer() const {
        std::string text;
        for (const auto& token : path_) {
            text += "/" + token;
        }
        return text;
    }

    const std::string& origin_;
    std::vector<std::string> path_;
    std::size_t values_ = 0;
};

bool ends_with_json(const std::string& origin) {
    if (origin.size() < 5) {
        return false;
    }
    std::string suffix = origin.substr(origin.size() - 5);
    for (auto& c : suffix) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return suffix == ".json";
}

SourceLocation file_location(const std::string& origin) {
    SourceLocation location;
    location.file = origin;
    return location;
}

void check_size(std::size_t size, const std::string& origin) {
    if (size > kMaxDocumentBytes) {
        throw Error(ErrorCode::PARSE_ERROR,
                    "document too large: " + std::to_string(size) + " bytes; the limit is " +
                        std::to_string(kMaxDocumentBytes) + " bytes (16 MiB)",
                    file_location(origin));
    }
}

LoadedDocument parse_yaml(std::string_view text, LoadedDocument document) {
    const std::string& origin = document.origin;
    std::vector<YAML::Node> documents;
    try {
        documents = YAML::LoadAll(std::string(text));
    } catch (const YAML::Exception& e) {
        SourcePosition position;
        if (!e.mark.is_null() && e.mark.line >= 0) {
            position = SourcePosition{e.mark.line + 1, e.mark.column + 1};
        }
        // yaml-cpp gives up on deep nesting with its own recursion limit, reported as "bad file"
        // at the place where it stopped (YAML::DeepRecursion, which cannot be caught by type
        // through a shared yaml-cpp); a file that cannot be read has no position.
        const bool too_deep = e.msg == YAML::ErrorMsg::BAD_FILE && !e.mark.is_null();
        fail(origin, too_deep ? nesting_message() : "invalid YAML: " + e.msg, position, "");
    }
    if (documents.empty()) {
        throw Error(ErrorCode::PARSE_ERROR, "document is empty", file_location(origin));
    }
    if (documents.size() > 1) {
        const auto mark = documents[1].Mark();
        SourcePosition position;
        if (!mark.is_null() && mark.line >= 0) {
            position = SourcePosition{mark.line + 1, mark.column + 1};
        }
        fail(origin, "more than one document; a file holds exactly one", position, "");
    }
    YamlConverter converter(text, origin, document.positions);
    const YAML::Node& root = documents[0];
    if (root.IsNull() && root.Tag().empty() && !converter.is_null_literal_at(root.Mark())) {
        throw Error(ErrorCode::PARSE_ERROR, "document is empty", file_location(origin));
    }
    try {
        ExpansionChecker(origin).check(root, 0);
        document.json = converter.convert(root, "", 0, nullptr, SourcePosition{1, 1});
    } catch (const YAML::Exception& e) {
        fail(origin, "invalid YAML: " + e.msg, SourcePosition{}, "");
    }
    return document;
}

} // namespace

LoadedDocument parse_document_text(std::string_view text, std::string origin) {
    check_size(text.size(), origin);
    if (text.compare(0, kBom.size(), kBom) == 0) {
        text.remove_prefix(kBom.size());
    }
    LoadedDocument document;
    document.origin = std::move(origin);

    // Bracket nesting is checked on the raw text, before any parser recurses into it. (Block
    // nesting is checked on the parsed graph.)
    if (const auto offset = find_excessive_nesting(text, kMaxNestingDepth)) {
        const auto position = text_position(text, *offset);
        fail(document.origin, nesting_message(), SourcePosition{position.line, position.column},
             "");
    }

    if (ends_with_json(document.origin)) {
        // A *.json document is JSON, strictly: it is never read as YAML.
        document.json = parse_json_text(text, document.origin);
        JsonPositionScanner(text, document.origin, document.positions).run();
        return document;
    }
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first != std::string_view::npos && (text[first] == '{' || text[first] == '[')) {
        // JSON content in any other file keeps JSON's exact semantics (escapes, numbers); what
        // is not JSON is YAML flow style.
        auto json = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
        if (!json.is_discarded()) {
            document.json = std::move(json);
            JsonPositionScanner(text, document.origin, document.positions).run();
            return document;
        }
    }
    return parse_yaml(text, std::move(document));
}

void check_document_size(const std::filesystem::path& file) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (!ec && size > kMaxDocumentBytes) {
        check_size(static_cast<std::size_t>(
                       std::min<std::uintmax_t>(size, std::numeric_limits<std::size_t>::max())),
                   path_to_utf8(file));
    }
}

LoadedDocument load_document_file(const std::filesystem::path& file) {
    check_document_size(file);
    return parse_document_text(read_file_bytes(file), path_to_utf8(file));
}

SourceLocation locate(const LoadedDocument& document, const std::string& pointer) {
    SourceLocation location;
    location.file = document.origin;
    location.pointer = pointer;
    std::string current = pointer;
    while (true) {
        const auto it = document.positions.find(current);
        if (it != document.positions.end()) {
            location.line = it->second.line;
            location.column = it->second.column;
            break;
        }
        if (current.empty()) {
            break;
        }
        const auto slash = current.rfind('/');
        current.erase(slash == std::string::npos ? 0 : slash);
    }
    return location;
}

} // namespace praenora::detail
