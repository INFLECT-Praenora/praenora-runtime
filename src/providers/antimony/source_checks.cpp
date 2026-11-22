// The antimony provider's rules that read a source's text alone (source_checks.hpp).
//
// The check of imports mirrors the lexer of libAntimony 3.3 (antimony.ypp, antimony_yylex), so that
// it never takes for a comment, a string or a block what libAntimony reads as code:
// - "//" and "#" start a line comment, which ends before the next CR or LF;
// - "/*" starts a block comment, which ends after the first "*/" that follows it;
// - '"' starts a string, in which a backslash takes the next character, whatever it is; the string
//   ends at the next quote without a backslash before it. A CR or a LF (without a backslash before
//   it) before that quote leaves the string unterminated: libAntimony then reads its content again
//   as code;
// - three backticks start a block, which ends after the next three backticks; four backticks are
//   one token of code, two an empty string, one a character;
// - a word is a letter or '_' followed by letters, digits and '_'; a number starts with a digit, or
//   with '.' and a digit, and libAntimony reads its digits, letters, '.', '+' and '-' as far as the
//   C++ library's number parser takes them, so a word may follow it directly.
// Each of these starts only where libAntimony starts a token, and in code every one of their first
// characters does: no word, number or operator of libAntimony holds '/', '#', '"' or '`'.

#include "source_checks.hpp"

#include <cstddef>

namespace praenora::antimony_provider {

namespace {

constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";

bool is_letter(char ch) noexcept { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }

bool is_digit(char ch) noexcept { return ch >= '0' && ch <= '9'; }

// A character of a word: a letter, a digit or '_'.
bool is_word_character(char ch) noexcept { return is_letter(ch) || is_digit(ch) || ch == '_'; }

// Whether `text` at `at` holds "import" in any case of letters.
bool import_at(std::string_view text, std::size_t at) noexcept {
    constexpr std::string_view kImport = "import";
    if (at + kImport.size() > text.size()) {
        return false;
    }
    for (std::size_t k = 0; k < kImport.size(); ++k) {
        char ch = text[at + k];
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
        if (ch != kImport[k]) {
            return false;
        }
    }
    return true;
}

// The position of the first character that is not white space, after an optional byte order
// mark, or the text's size when there is none.
std::size_t first_markup(std::string_view text) noexcept {
    const std::string_view rest = without_byte_order_mark(text);
    std::size_t at = text.size() - rest.size();
    while (at < text.size() && is_white_space(text[at])) {
        ++at;
    }
    return at;
}

} // namespace

bool is_white_space(char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' || ch == '\f' || ch == '\r';
}

std::string_view without_byte_order_mark(std::string_view source) noexcept {
    return source.substr(0, kByteOrderMark.size()) == kByteOrderMark
               ? source.substr(kByteOrderMark.size())
               : source;
}

std::string without_trailing_white_space(std::string text) {
    while (!text.empty() && is_white_space(text.back())) {
        text.pop_back();
    }
    return text;
}

std::string antimony_import_rule(std::string_view source) {
    const std::size_t size = source.size();
    std::size_t i = 0;
    while (i < size) {
        const char ch = source[i];
        const char next = i + 1 < size ? source[i + 1] : '\0';
        if ((ch == '/' && next == '/') || ch == '#') {
            // A line comment, to the next CR or LF.
            const std::size_t end = source.find_first_of("\r\n", i);
            if (end == std::string_view::npos) {
                return "";
            }
            i = end;
        } else if (ch == '/' && next == '*') {
            const std::size_t end = source.find("*/", i + 2);
            if (end == std::string_view::npos) {
                return "unterminated";
            }
            i = end + 2;
        } else if (ch == '"') {
            std::size_t j = i + 1;
            while (true) {
                if (j >= size) {
                    return "unterminated";
                }
                const char in_string = source[j];
                if (in_string == '\\') {
                    j += 2;
                } else if (in_string == '"') {
                    break;
                } else if (in_string == '\r' || in_string == '\n') {
                    return "unterminated";
                } else {
                    ++j;
                }
            }
            i = j + 1;
        } else if (ch == '`') {
            if (next == '`' && i + 2 < size && source[i + 2] == '`') {
                if (i + 3 >= size) {
                    return "unterminated";
                }
                if (source[i + 3] == '`') {
                    i += 4; // one token of code
                    continue;
                }
                const std::size_t end = source.find("```", i + 3);
                if (end == std::string_view::npos) {
                    return "unterminated";
                }
                i = end + 3;
            } else {
                i += next == '`' ? 2 : 1;
            }
        } else if (is_letter(ch) || ch == '_') {
            std::size_t j = i;
            while (j < size && is_word_character(source[j])) {
                ++j;
            }
            if (j - i == 6 && import_at(source, i)) {
                return "import";
            }
            i = j;
        } else if (is_digit(ch) || (ch == '.' && is_digit(next))) {
            // A number and what may follow it without a space: any import in it that a word could
            // end at.
            std::size_t j = i;
            while (j < size && (is_word_character(source[j]) || source[j] == '.')) {
                ++j;
            }
            for (std::size_t k = i; k < j; ++k) {
                if (import_at(source, k) && (k + 6 == size || !is_word_character(source[k + 6]))) {
                    return "import";
                }
            }
            i = j;
        } else {
            ++i;
        }
    }
    return "";
}

std::string antimony_engine_text(std::string_view source) {
    const std::string_view rest = without_byte_order_mark(source);
    std::string text;
    text.reserve(rest.size());
    for (std::size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] != '\r' || i + 1 >= rest.size() || rest[i + 1] != '\n') {
            text += rest[i];
        }
    }
    return text;
}

SbmlDocumentText sbml_engine_text(std::string_view source) {
    const std::string_view rest = without_byte_order_mark(source);
    SbmlDocumentText text;
    std::size_t at = 0;
    while (at < rest.size() && is_white_space(rest[at])) {
        if (rest[at] == '\n' || rest[at] == '\r') {
            if (rest[at] == '\r' && at + 1 < rest.size() && rest[at + 1] == '\n') {
                ++at;
            }
            ++text.lines_before;
            text.columns_before = 0;
        } else {
            ++text.columns_before;
        }
        ++at;
    }
    const std::string_view document = rest.substr(at);
    if (document.substr(0, 14) != "<?xml version=") {
        text.document = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        text.lines_added = 1;
    }
    text.document += document;
    return text;
}

SourceLocation source_location(const SbmlDocumentText& text, std::size_t line,
                               std::size_t column) noexcept {
    if (line <= text.lines_added) {
        return {};
    }
    const std::size_t document_line = line - text.lines_added;
    return {document_line + text.lines_before,
            column + 1 + (document_line == 1 ? text.columns_before : 0)};
}

std::string antimony_text_rule(std::string_view source) {
    const std::size_t at = first_markup(source);
    if (at < source.size() && source[at] == '<') {
        return "xml";
    }
    return antimony_import_rule(source);
}

std::string sbml_text_rule(std::string_view source) {
    const std::size_t at = first_markup(source);
    if (at >= source.size() || source[at] != '<') {
        return "not_document";
    }
    constexpr std::string_view kDoctype = "<!doctype";
    for (std::size_t i = at; i + kDoctype.size() <= source.size(); ++i) {
        bool found = true;
        for (std::size_t k = 0; k < kDoctype.size() && found; ++k) {
            char ch = source[i + k];
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
            found = ch == kDoctype[k];
        }
        if (found) {
            return "document_type";
        }
    }
    return "";
}

} // namespace praenora::antimony_provider
