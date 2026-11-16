// JSON helpers: canonical serialization, parsing with positioned errors and the nesting limit, and
// small text utilities shared by the document loaders.
#ifndef PRAENORA_RUNTIME_DOCUMENT_JSON_UTIL_HPP
#define PRAENORA_RUNTIME_DOCUMENT_JSON_UTIL_HPP

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace praenora::detail {

// Documents and JSON texts may nest objects and arrays at most this deep. The outermost object or
// array is level 1.
inline constexpr int kMaxNestingDepth = 128;

// Object keys sorted, no insignificant whitespace, UTF-8, numbers in nlohmann/json's
// locale-independent shortest round-trip form: nlohmann `dump()` without indentation. Throws
// praenora::Error(DATA_INVALID) if a string is not valid UTF-8.
std::string canonical_json(const nlohmann::json& value);

// Parses one JSON text (RFC 8259). Throws praenora::Error(PARSE_ERROR) with `origin` as the file
// and the line and column of the problem, including "nesting too deep" beyond kMaxNestingDepth.
// There is no size limit here: callers limit the text themselves.
nlohmann::json parse_json_text(std::string_view text, std::string_view origin);

// Escapes one JSON-pointer reference token: "~" becomes "~0" and "/" becomes "~1".
std::string json_pointer_escape(std::string_view token);

bool is_valid_utf8(std::string_view text);

// A decimal number written [+-]digits[.digits][(e|E)[+-]digits] (".5" and "5." are accepted too),
// read independently of the global locale; nullopt for any other text, and for values beyond the
// range of a double.
std::optional<double> parse_decimal(std::string_view text);

// 1-based line and column (in bytes) of the byte at `offset`; lines end at '\n'.
struct TextPosition {
    std::int64_t line = 1, column = 1;
};
TextPosition text_position(std::string_view text, std::size_t offset);

// The offset of the first '[' or '{' that opens level max_depth + 1, or nullopt. Meant for JSON
// and YAML text. Brackets count everywhere outside quoted scalars and comments, inside plain and
// block scalars too. The quoted scalars and comments are found by splitting the text into tokens
// as yaml-cpp does, and as long as that split agrees with yaml-cpp's, the count is at least the
// flow nesting yaml-cpp finds. A quote opens a string only where a token starts, never inside a
// plain scalar such as a:'x or don't, and where a scalar ends depends on indentation, which the
// split follows too. Text with a NUL byte is not split (it is UTF-16 or UTF-32, which yaml-cpp
// reads too, or not text): every opening bracket in it counts, and no closing one.
std::optional<std::size_t> find_excessive_nesting(std::string_view text, int max_depth);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_DOCUMENT_JSON_UTIL_HPP
