#include "document/schema_registry.hpp"

#include "core/paths.hpp"
#include "document/json_util.hpp"
#include "document/resources.hpp"

#include <praenora/runtime/error.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <system_error>
#include <tuple>

namespace praenora::detail {

namespace {

using Key = std::tuple<std::string, std::uint64_t, std::uint64_t>; // kind, major, minor

struct FileName {
    std::string kind;
    std::uint64_t major = 0, minor = 0;
};

bool is_kind_char(char c, bool first) {
    return (c >= 'a' && c <= 'z') || (!first && ((c >= '0' && c <= '9') || c == '-'));
}

std::optional<std::uint64_t> parse_number(std::string_view text) {
    if (text.empty() || text.size() > 9 || (text.size() > 1 && text[0] == '0')) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return value;
}

// <kind>.<major>.<minor>.schema.json, kind matching [a-z][a-z0-9-]*, numbers without leading
// zeros.
std::optional<FileName> parse_file_name(std::string_view name) {
    constexpr std::string_view kSuffix = ".schema.json";
    if (name.size() <= kSuffix.size() ||
        name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
        return std::nullopt;
    }
    name.remove_suffix(kSuffix.size());
    const auto second_dot = name.rfind('.');
    if (second_dot == std::string_view::npos) {
        return std::nullopt;
    }
    const auto first_dot = name.rfind('.', second_dot - 1);
    if (first_dot == std::string_view::npos || first_dot == 0) {
        return std::nullopt;
    }
    const auto kind = name.substr(0, first_dot);
    for (std::size_t i = 0; i < kind.size(); ++i) {
        if (!is_kind_char(kind[i], i == 0)) {
            return std::nullopt;
        }
    }
    const auto major = parse_number(name.substr(first_dot + 1, second_dot - first_dot - 1));
    const auto minor = parse_number(name.substr(second_dot + 1));
    if (!major || !minor) {
        return std::nullopt;
    }
    return FileName{std::string(kind), *major, *minor};
}

std::string version_text(std::uint64_t major, std::uint64_t minor) {
    return std::to_string(major) + "." + std::to_string(minor);
}

} // namespace

struct SchemaRegistry::State {
    struct Entry {
        std::string origin; // resource path or file path, for messages
        std::string owned_text;
        std::string_view text;
        std::unique_ptr<CompiledSchema> compiled; // guarded by `mutex`
    };

    std::map<Key, Entry> entries; // fixed after construction
    std::mutex mutex;

    Entry& find(std::string_view kind, const SemVer& version) {
        const auto it = entries.find(Key{std::string(kind), version.major, version.minor});
        if (it != entries.end()) {
            return it->second;
        }
        std::string supported;
        for (const auto& [key, entry] : entries) {
            if (std::get<0>(key) == kind) {
                supported += (supported.empty() ? "" : ", ") +
                             version_text(std::get<1>(key), std::get<2>(key));
            }
        }
        if (supported.empty()) {
            throw Error(ErrorCode::FORMAT_VERSION_UNSUPPORTED,
                        "unknown document kind '" + std::string(kind) + "'");
        }
        throw Error(ErrorCode::FORMAT_VERSION_UNSUPPORTED,
                    "format_version " + to_string(version) + " of document kind '" +
                        std::string(kind) + "' is not supported (supported: " + supported + ")");
    }
};

SchemaRegistry::SchemaRegistry(std::unique_ptr<State> state) : state_(std::move(state)) {}
SchemaRegistry::SchemaRegistry(SchemaRegistry&&) noexcept = default;
SchemaRegistry& SchemaRegistry::operator=(SchemaRegistry&&) noexcept = default;
SchemaRegistry::~SchemaRegistry() = default;

const SchemaRegistry& SchemaRegistry::builtin() {
    static const SchemaRegistry registry = [] {
        auto state = std::make_unique<State>();
        constexpr std::string_view kPrefix = "schemas/";
        for (const auto& path : list_embedded_resources(kPrefix)) {
            const auto name = parse_file_name(std::string_view(path).substr(kPrefix.size()));
            const auto bytes = find_embedded_resource(path);
            if (!name || !bytes) {
                continue;
            }
            auto& entry = state->entries[Key{name->kind, name->major, name->minor}];
            entry.origin = path;
            entry.text = *bytes;
        }
        return SchemaRegistry(std::move(state));
    }();
    return registry;
}

SchemaRegistry SchemaRegistry::from_directory(const std::filesystem::path& directory) {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        throw Error(ErrorCode::IO_ERROR,
                    "schema directory '" + path_to_utf8(directory) + "' does not exist");
    }
    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            files.push_back(it->path());
        }
    }
    if (ec) {
        throw Error(ErrorCode::IO_ERROR, "cannot list schema directory '" +
                                             path_to_utf8(directory) + "': " + ec.message());
    }
    std::sort(files.begin(), files.end());

    auto state = std::make_unique<State>();
    for (const auto& file : files) {
        const auto name = parse_file_name(path_to_utf8(file.filename()));
        if (!name) {
            continue;
        }
        auto& entry = state->entries[Key{name->kind, name->major, name->minor}];
        entry.origin = path_to_utf8(file);
        entry.owned_text = read_file_bytes(file);
        entry.text = entry.owned_text;
    }
    return SchemaRegistry(std::move(state));
}

const CompiledSchema& SchemaRegistry::for_kind(std::string_view kind,
                                               const SemVer& format_version) const {
    auto& entry = state_->find(kind, format_version);
    const std::lock_guard<std::mutex> lock(state_->mutex);
    if (entry.compiled) {
        return *entry.compiled;
    }
    const std::string version = version_text(format_version.major, format_version.minor);
    const std::string expected_id = "urn:praenora:schema:" + std::string(kind) + ":" + version;
    const nlohmann::json schema = parse_json_text(entry.text, entry.origin);
    // Every schema names itself, exactly: references between schemas resolve through this URN.
    const auto id = schema.is_object() ? schema.find("$id") : schema.end();
    if (id == schema.end() || !id->is_string() || id->get<std::string>() != expected_id) {
        SourceLocation location;
        location.file = entry.origin;
        location.pointer = "/$id";
        throw Error(
            ErrorCode::SCHEMA_VIOLATION,
            "schema " + entry.origin + " must have \"$id\": \"" + expected_id + "\"" +
                (id == schema.end() ? std::string(" (it has none)") : "; it has " + id->dump()),
            location);
    }
    // Map nodes never move, so the compiled schema stays where it is for the registry's lifetime.
    entry.compiled = std::make_unique<CompiledSchema>(schema, std::string(kind) + "@" + version);
    return *entry.compiled;
}

std::string_view SchemaRegistry::text_for_kind(std::string_view kind,
                                               const SemVer& format_version) const {
    return state_->find(kind, format_version).text;
}

std::vector<std::pair<std::string, SemVer>> SchemaRegistry::entries() const {
    std::vector<std::pair<std::string, SemVer>> list;
    for (const auto& entry : state_->entries) {
        SemVer version;
        version.major = std::get<1>(entry.first);
        version.minor = std::get<2>(entry.first);
        list.emplace_back(std::get<0>(entry.first), version);
    }
    return list;
}

} // namespace praenora::detail
