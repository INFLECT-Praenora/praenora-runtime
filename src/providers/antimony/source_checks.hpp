// The antimony provider's rules that read a source's text alone, before the call waits for the
// provider's lock (docs/specs/model-conversion-service.md, the antimony provider's section). Pure
// C++17: no engine, no third-party type; the unit tests compile source_checks.cpp in every build.
#ifndef PRAENORA_PROVIDERS_ANTIMONY_SOURCE_CHECKS_HPP
#define PRAENORA_PROVIDERS_ANTIMONY_SOURCE_CHECKS_HPP

#include <cstddef>
#include <string>
#include <string_view>

namespace praenora::antimony_provider {

// The check of imports in an Antimony source: the first rule the source breaks, or "" when it
// passes: "import" or "unterminated". It finds comments, strings and triple-backtick blocks as
// libAntimony 3.3's lexer does, and refuses
// - "import": the word import, in any case of letters, in code: a run of letters, digits and '_'
//   that starts with a letter or '_' and is import, or a run of letters, digits, '_' and '.' that
//   starts with a digit (a number, which libAntimony may end before the letters) and holds import
//   followed by a character other than a letter, a digit and '_' (or the end);
// - "unterminated": a block comment, a string or a triple-backtick block that the source does not
//   close (a string is closed only on its own line).
std::string antimony_import_rule(std::string_view source);

// The rules of format "antimony", in order: "xml" for a source that is an XML document (its first
// character other than white space, after an optional UTF-8 byte order mark, is '<'), which
// libAntimony would read as SBML; then the check of imports. "" when the source passes.
std::string antimony_text_rule(std::string_view source);

// The rules of format "sbml" that read the text alone, in order: "not_document" for a source whose
// first character other than white space, after an optional UTF-8 byte order mark, is not '<' (a
// path or a URI is not a document); "document_type" for a source that holds a document type
// declaration (<!DOCTYPE, in any case of letters). "" when the source passes.
std::string sbml_text_rule(std::string_view source);

// Space, tab, line feed, vertical tab, form feed and carriage return: the white space of the
// service's rule "empty".
bool is_white_space(char ch) noexcept;

// The source without its UTF-8 byte order mark, when it starts with one.
std::string_view without_byte_order_mark(std::string_view source) noexcept;

// Text without the white space at its end (an engine's message).
std::string without_trailing_white_space(std::string text);

// What libAntimony reads of an Antimony source: the source without its byte order mark, with each
// CR LF replaced by a LF. libAntimony itself refuses a byte order mark, and keeps the CR of a CR LF
// inside a block that spans lines.
std::string antimony_engine_text(std::string_view source);

// What libSBML and libAntimony read of an SBML source: the document from its first '<', without
// the byte order mark and the white space before it (with which libSBML would refuse an XML
// declaration), after an XML declaration line that the provider adds when the document does not
// start with "<?xml version=" (libSBML would add the same line itself); and where the document is
// in the source: after `lines_before` line ends (a CR LF, a CR or a LF each), and
// `columns_before` characters into its line.
struct SbmlDocumentText {
    std::string document;
    std::size_t lines_added = 0;
    std::size_t lines_before = 0;
    std::size_t columns_before = 0;
};
SbmlDocumentText sbml_engine_text(std::string_view source);

// Where libSBML's line and 0-based column in `text.document` are in the source: {line, column},
// both from 1; {0, 0} when libSBML does not locate the problem in the source's text.
struct SourceLocation {
    std::size_t line = 0;
    std::size_t column = 0;
};
SourceLocation source_location(const SbmlDocumentText& text, std::size_t line,
                               std::size_t column) noexcept;

} // namespace praenora::antimony_provider

#endif // PRAENORA_PROVIDERS_ANTIMONY_SOURCE_CHECKS_HPP
