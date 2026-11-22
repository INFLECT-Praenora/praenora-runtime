// Loads Praenora documents (YAML or JSON) into JSON, with a source map from JSON pointers to line
// and column.
//
// YAML subset:
// - one document per stream; an empty document is an error;
// - plain scalars resolve per the YAML 1.2 core schema: null, true/false (not yes/no), decimal,
//   0o octal and 0x hexadecimal integers (int64 or uint64; overflow is an error), floats; .inf
//   and .nan are errors because JSON cannot represent them; anything else is a string;
// - quoted and block scalars are strings; the tags !!str, !!int, !!float, !!bool and !!null are
//   honored and every other tag is an error;
// - anchors and aliases are allowed; merge keys (<<), duplicate keys and keys that are not
//   strings are errors;
// - all text must be UTF-8.
// JSON:
// - a document whose origin ends in ".json" (any case) is strict JSON (RFC 8259) and is never
//   read as YAML;
// - any other document whose first character is '{' or '[' and that is valid JSON is read with
//   the JSON parser too, which keeps JSON's exact semantics (escapes, numbers);
// - duplicate keys are errors in JSON documents as well.
//
// Limits: 16 MiB per document, nesting depth 128 (json_util.hpp), and 2^18 values after alias
// expansion, checked before anything is built. Every problem is reported as
// praenora::Error(PARSE_ERROR) with file, line, column and, where known, the JSON pointer.
#ifndef PRAENORA_RUNTIME_DOCUMENT_YAML_LOADER_HPP
#define PRAENORA_RUNTIME_DOCUMENT_YAML_LOADER_HPP

#include <praenora/runtime/error.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace praenora::detail {

inline constexpr std::size_t kMaxDocumentBytes = std::size_t{16} << 20;
inline constexpr std::size_t kMaxDocumentValues = std::size_t{1} << 18;

// 1-based line and column; columns count bytes.
struct SourcePosition {
    std::int64_t line = 0, column = 0;
};

inline bool operator==(const SourcePosition& a, const SourcePosition& b) {
    return a.line == b.line && a.column == b.column;
}
inline bool operator!=(const SourcePosition& a, const SourcePosition& b) { return !(a == b); }

// Key: a JSON pointer ("" is the whole document). Every value of the document has an entry.
// Scalars are located where they start; a mapping entry whose value is a mapping or sequence, or
// is empty, is located at its key; sequence items where the item starts.
using SourceMap = std::map<std::string, SourcePosition>;

struct LoadedDocument {
    nlohmann::json json;
    SourceMap positions;
    std::string origin; // file name used in locations, for example the path of the file
};

// Parses YAML or JSON text. A leading UTF-8 byte order mark is ignored; CRLF line endings read
// like LF.
LoadedDocument parse_document_text(std::string_view text, std::string origin);

// Refuses a file larger than kMaxDocumentBytes before anything reads it:
// praenora::Error(PARSE_ERROR, "document too large ..."), located at the file. A file whose size
// cannot be determined (one that does not exist, for example) passes; reading it reports why.
void check_document_size(const std::filesystem::path& file);

// Reads and parses a file; the origin is the file's path. Throws praenora::Error(IO_ERROR) if the
// file cannot be read, and refuses a file beyond the size limit before reading it
// (check_document_size()).
LoadedDocument load_document_file(const std::filesystem::path& file);

// The location of `pointer`: the document's origin, the position of the pointer or of its nearest
// ancestor that has one, and `pointer` itself.
SourceLocation locate(const LoadedDocument& document, const std::string& pointer);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_DOCUMENT_YAML_LOADER_HPP
