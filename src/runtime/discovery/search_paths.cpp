#include "discovery/search_paths.hpp"

#include "core/paths.hpp"
#include "platform/platform.hpp"

#include <algorithm>
#include <string>
#include <system_error>
#include <utility>

namespace praenora::detail {

namespace fs = std::filesystem;

namespace {

std::string quoted(const fs::path& path) { return "'" + path_to_utf8(path) + "'"; }

bool has_suffix(const std::string& name, std::string_view suffix) {
    return name.size() > suffix.size() &&
           name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void warn(std::vector<Diagnostic>& diagnostics, ErrorCode code, std::string message,
          const fs::path& file) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::warning;
    diagnostic.code = code;
    diagnostic.message = std::move(message);
    diagnostic.location.file = path_to_utf8(file);
    diagnostics.push_back(std::move(diagnostic));
}

struct DirectoryListing {
    std::vector<fs::path> manifests, directories; // each sorted by UTF-8 file name
};

// The manifest files and subdirectories directly inside `directory`. A directory that cannot be
// read is reported as a warning; what was listed until then is kept.
DirectoryListing list_directory(const fs::path& directory, std::string_view suffix,
                                std::string_view kind, std::vector<Diagnostic>& diagnostics) {
    DirectoryListing listing;
    std::error_code ec;
    fs::directory_iterator it(directory, ec);
    const fs::directory_iterator end;
    while (!ec && it != end) {
        const fs::directory_entry& entry = *it;
        std::error_code type_ec;
        if (entry.is_directory(type_ec)) {
            listing.directories.push_back(entry.path());
        } else if (entry.is_regular_file(type_ec) &&
                   has_suffix(path_to_utf8(entry.path().filename()), suffix)) {
            listing.manifests.push_back(entry.path());
        }
        it.increment(ec);
    }
    if (ec) {
        warn(diagnostics, ErrorCode::IO_ERROR,
             "cannot read " + std::string(kind) + " directory " + quoted(directory) + ": " +
                 ec.message(),
             directory);
    }
    const auto by_name = [](const fs::path& a, const fs::path& b) {
        return path_to_utf8(a.filename()) < path_to_utf8(b.filename());
    };
    std::sort(listing.manifests.begin(), listing.manifests.end(), by_name);
    std::sort(listing.directories.begin(), listing.directories.end(), by_name);
    return listing;
}

} // namespace

fs::path normalized_search_path(const fs::path& path) {
    std::error_code ec;
    fs::path result = fs::absolute(path, ec);
    if (ec) {
        result = path;
    }
    result = result.lexically_normal();
    if (!result.has_filename() && result.has_relative_path()) {
        result = result.parent_path();
    }
    return result;
}

std::string to_string(SearchPathOrigin origin) {
    switch (origin) {
    case SearchPathOrigin::caller:
        return "caller";
    case SearchPathOrigin::configuration:
        return "configuration";
    case SearchPathOrigin::environment:
        return "environment";
    case SearchPathOrigin::installation:
        return "installation";
    }
    return "caller"; // not reached: every origin is listed
}

std::vector<fs::path> paths_of(const std::vector<SearchPath>& search_paths) {
    std::vector<fs::path> paths;
    paths.reserve(search_paths.size());
    for (const SearchPath& search_path : search_paths) {
        paths.push_back(search_path.path);
    }
    return paths;
}

std::vector<SearchPath> caller_search_paths(const std::vector<fs::path>& paths) {
    std::vector<SearchPath> search_paths;
    search_paths.reserve(paths.size());
    for (std::size_t i = 0; i < paths.size(); ++i) {
        search_paths.push_back({paths[i], SearchPathOrigin::caller, i});
    }
    return search_paths;
}

void append_search_path(std::vector<SearchPath>& paths, const fs::path& path,
                        SearchPathOrigin origin, std::optional<std::size_t> index) {
    if (path.empty()) {
        return;
    }
    fs::path normal = normalized_search_path(path);
    const auto same = [&](const SearchPath& earlier) { return earlier.path == normal; };
    if (std::none_of(paths.begin(), paths.end(), same)) {
        paths.push_back({std::move(normal), origin, index});
    }
}

void append_environment_search_paths(std::vector<SearchPath>& paths, const char* name) {
    if (const auto list = environment_variable(name)) {
        const std::vector<std::string> entries = split_path_list(*list); // without empty ones
        for (std::size_t i = 0; i < entries.size(); ++i) {
            append_search_path(paths, path_from_utf8(entries[i]), SearchPathOrigin::environment, i);
        }
    }
}

std::optional<fs::path> installation_praenora_dir() {
    const auto binary = runtime_binary_path();
    if (!binary) {
        return std::nullopt;
    }
#ifdef _WIN32
    return (binary->parent_path() / ".." / "lib" / "praenora").lexically_normal();
#else
    return (binary->parent_path() / "praenora").lexically_normal();
#endif
}

std::vector<fs::path> manifest_files(const fs::path& search_path, std::string_view suffix,
                                     std::string_view kind, std::vector<Diagnostic>& diagnostics) {
    std::error_code ec;
    if (!fs::exists(search_path, ec)) {
        return {};
    }
    if (!fs::is_directory(search_path, ec)) {
        warn(diagnostics, ErrorCode::NOT_FOUND,
             std::string(kind) + " search path " + quoted(search_path) + " is not a directory",
             search_path);
        return {};
    }
    const DirectoryListing top = list_directory(search_path, suffix, kind, diagnostics);
    std::vector<fs::path> files = top.manifests;
    for (const auto& directory : top.directories) {
        const DirectoryListing sub = list_directory(directory, suffix, kind, diagnostics);
        files.insert(files.end(), sub.manifests.begin(), sub.manifests.end());
    }
    return files;
}

ManifestLocation manifest_location(const SearchPath& search_path, const fs::path& file) {
    ManifestLocation location;
    location.origin = search_path.origin;
    location.index = search_path.index;
    // manifest_files() names each file <search path>/<file> or <search path>/<directory>/<file>;
    // both sides are normalized first, so a trailing separator on the search path changes nothing.
    // The generic format uses '/' on every platform.
    location.file = normalized_search_path(file)
                        .lexically_relative(normalized_search_path(search_path.path))
                        .generic_u8string();
    return location;
}

} // namespace praenora::detail
