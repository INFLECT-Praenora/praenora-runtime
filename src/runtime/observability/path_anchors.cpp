#include "observability/path_anchors.hpp"

#include "core/paths.hpp"

#include <algorithm>
#include <cstddef>
#include <system_error>

namespace praenora::detail {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

bool is_separator(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

// Whether `c` can continue a path segment: an ASCII letter or digit, '.', '_', '-', '~', a byte
// outside ASCII (part of a UTF-8 sequence) or a separator. (No <cctype>: it depends on the locale.)
bool continues_segment(char c) {
    const auto byte = static_cast<unsigned char>(c);
    return byte >= 0x80 || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '.' || c == '_' || c == '-' || c == '~' ||
           is_separator(c);
}

// Whether the character of the text, `c`, matches the anchor's character `a`: equal bytes; on
// Windows also two separators, or two ASCII letters that differ only in case.
bool same_character(char c, char a) {
    if (c == a) {
        return true;
    }
#ifdef _WIN32
    if (is_separator(c) && is_separator(a)) {
        return true;
    }
    const auto lower = [](char x) {
        return x >= 'A' && x <= 'Z' ? static_cast<char>(x - 'A' + 'a') : x;
    };
    return lower(c) == lower(a);
#else
    return false;
#endif
}

bool same_text(std::string_view text, std::string_view anchor) {
    return text.size() == anchor.size() &&
           std::equal(text.begin(), text.end(), anchor.begin(), same_character);
}

// Whether text[at] begins a segment: at the start of the text, or after a character that does not
// continue a segment.
bool starts_whole(std::string_view text, std::size_t at) {
    return at == 0 || !continues_segment(text[at - 1]);
}

#ifdef __APPLE__
// What dyld (macOS 13 and later) puts before a library's absolute path when it lists the paths it
// tried in the OS cryptex: /System/Volumes/Preboot/Cryptexes/<one segment>.
constexpr std::string_view kCryptexes = "/System/Volumes/Preboot/Cryptexes/";

// Whether text[at], a separator, directly follows kCryptexes and one whole segment, the prefix
// itself beginning a segment.
bool follows_cryptex_prefix(std::string_view text, std::size_t at) {
    if (!is_separator(text[at])) {
        return false;
    }
    std::size_t segment = at;
    while (segment > 0 && continues_segment(text[segment - 1]) &&
           !is_separator(text[segment - 1])) {
        --segment;
    }
    if (segment == at || segment < kCryptexes.size()) {
        return false;
    }
    const std::size_t prefix = segment - kCryptexes.size();
    return text.compare(prefix, kCryptexes.size(), kCryptexes) == 0 && starts_whole(text, prefix);
}
#endif

// Whether an anchor may begin at text[at]: a whole-path boundary on its left.
bool boundary_before(std::string_view text, std::size_t at) {
#ifdef __APPLE__
    return starts_whole(text, at) || follows_cryptex_prefix(text, at);
#else
    return starts_whole(text, at);
#endif
}

// Whether an anchor may end before text[end]: the end of the text, a separator, a character that
// does not continue a segment, or a run of '.' that is followed by the end of the text or by a
// character that neither continues a segment nor is a separator (the end of a sentence).
bool boundary_after(std::string_view text, std::size_t end) {
    if (end == text.size() || is_separator(text[end]) || !continues_segment(text[end])) {
        return true;
    }
    std::size_t after_dots = end;
    while (after_dots < text.size() && text[after_dots] == '.') {
        ++after_dots;
    }
    return after_dots > end &&
           (after_dots == text.size() ||
            (!continues_segment(text[after_dots]) && !is_separator(text[after_dots])));
}

// `directory` as an anchor: absolute, lexically normal, without a trailing separator, as UTF-8; or
// nullopt for an empty or relative path and for a root directory.
std::optional<std::string> anchor_text(const fs::path& directory) {
    if (directory.empty() || !directory.is_absolute()) {
        return std::nullopt;
    }
    fs::path normal = directory.lexically_normal();
    if (!normal.has_filename() && normal.has_relative_path()) {
        normal = normal.parent_path();
    }
    if (!normal.has_relative_path()) {
        return std::nullopt; // a root directory
    }
    return path_to_utf8(normal);
}

std::string search_path_placeholder(const char* kind, const SearchPath& search_path) {
    std::string placeholder = std::string("<") + kind + ":" + to_string(search_path.origin);
    if (search_path.index) {
        placeholder += ":" + std::to_string(*search_path.index);
    }
    return placeholder + ">";
}

void apply_to_strings(const PathAnchors& anchors, json& value) {
    if (value.is_string()) {
        value = anchors.apply(value.get_ref<const std::string&>());
    } else if (value.is_structured()) {
        for (json& element : value) { // an object's values, never its keys
            apply_to_strings(anchors, element);
        }
    }
}

} // namespace

PathAnchors::PathAnchors(const RunDirectories& directories) {
    if (directories.output) {
        add(*directories.output, "<output>");
    }
    add(directories.configuration, "<configuration>");
    for (const SearchPath& search_path : directories.modules) {
        add(search_path.path, search_path_placeholder("modules", search_path));
    }
    for (const SearchPath& search_path : directories.providers) {
        add(search_path.path, search_path_placeholder("providers", search_path));
    }
    if (directories.home) {
        add(*directories.home, "<home>");
    }
    // The longest anchor first; a stable sort keeps the order of precedence among equal lengths.
    std::stable_sort(anchors_.begin(), anchors_.end(), [](const Anchor& a, const Anchor& b) {
        return a.text.size() > b.text.size();
    });
}

void PathAnchors::add(const fs::path& directory, const std::string& placeholder) {
    const std::optional<std::string> lexical = anchor_text(directory);
    if (!lexical) {
        return;
    }
    std::vector<std::string> forms = {*lexical};
    std::error_code ec;
    const fs::path resolved = fs::weakly_canonical(path_from_utf8(*lexical), ec);
    if (!ec) {
        if (const std::optional<std::string> canonical = anchor_text(resolved)) {
            forms.push_back(*canonical);
        }
    }
    for (const std::string& form : forms) {
        const auto same = [&form](const Anchor& anchor) { return same_text(form, anchor.text); };
        if (std::none_of(anchors_.begin(), anchors_.end(), same)) {
            anchors_.push_back({form, placeholder});
        }
    }
}

std::string PathAnchors::apply(std::string_view text) const {
    std::string result;
    result.reserve(text.size());
    std::size_t at = 0;
    while (at < text.size()) {
        const Anchor* found = nullptr;
        if (boundary_before(text, at)) {
            for (const Anchor& anchor : anchors_) {
                const std::size_t end = at + anchor.text.size();
                if (end <= text.size() &&
                    same_text(text.substr(at, anchor.text.size()), anchor.text) &&
                    boundary_after(text, end)) {
                    found = &anchor;
                    break;
                }
            }
        }
        if (found != nullptr) {
            result += found->placeholder;
            at += found->text.size();
        } else {
            result += text[at];
            ++at;
        }
    }
    return result;
}

void PathAnchors::apply_to_error_record(json& record) const {
    if (!record.is_object()) {
        return;
    }
    // The parts that can name files; never the name, the time or a location's JSON pointer.
    for (const char* part : {"message", "details", "cause"}) {
        if (const auto found = record.find(part); found != record.end()) {
            apply_to_strings(*this, *found);
        }
    }
    if (const auto location = record.find("location");
        location != record.end() && location->is_object()) {
        if (const auto file = location->find("file"); file != location->end()) {
            apply_to_strings(*this, *file);
        }
    }
}

void PathAnchors::apply_to_status_snapshot(json& snapshot) const {
    if (!snapshot.is_object()) {
        return;
    }
    const auto apply_to_last_error = [this](json& owner) {
        if (!owner.is_object()) {
            return;
        }
        const auto error = owner.find("last_error");
        if (error != owner.end() && error->is_object()) {
            apply_to_error_record(*error);
        }
    };
    if (const auto workflow = snapshot.find("workflow"); workflow != snapshot.end()) {
        apply_to_last_error(*workflow);
    }
    if (const auto steps = snapshot.find("steps"); steps != snapshot.end() && steps->is_array()) {
        for (json& step : *steps) {
            apply_to_last_error(step);
        }
    }
}

} // namespace praenora::detail
