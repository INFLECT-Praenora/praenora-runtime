// praenora_python_start_probe: resolves praenora.python@0 on provider search paths, as the runtime
// does, loads and starts the provider that serves it, and writes what came of it to a file. The
// tests of tests/negative/python_environment_test.cpp run it in a process of its own for each
// case: a process loads the Python core once, and a provider's start() is called once.
//
//   praenora_python_start_probe [--load <library>] <result file> <provider search path>...
//
// --load first loads <library> into the process's global scope (dlopen(RTLD_NOW | RTLD_GLOBAL);
// on Windows LoadLibraryExW()), as a process that runs another Python interpreter has it. The
// search paths are those of the caller (provider_search_paths()), so the first manifest of a
// provider shadows a later one of its name. The result file holds one JSON object:
//   "python_manifests"  every manifest of the provider python found, in search order, each
//                       {"manifest", "shadowed_by"} (null when it is not shadowed)
//   "manifest"          the manifest that serves the service, or null when none does
//   "started"           true when the provider started
//   "describe"          the service's describe document, when it started
//   "error"             the error (praenora::Error::to_json()), when it did not
// Exit code 0 when the provider started, 1 when it did not, 2 when the probe itself could not run.
#include "discovery/search_paths.hpp"
#include "provider/provider_index.hpp"
#include "provider/provider_loader.hpp"
#include "text_sink.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/sdk/python.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

namespace fs = std::filesystem;
namespace detail = praenora::detail;
using nlohmann::json;

std::string utf8(const fs::path& path) { return path.u8string(); }

using praenora::test::TextSink;

// Loads `library` for the rest of the process, into its global scope.
void load_globally(const fs::path& library) {
#ifdef _WIN32
    fs::path native = library;
    native.make_preferred();
    if (LoadLibraryExW(native.c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) ==
        nullptr) {
        throw std::runtime_error("cannot load " + utf8(library) + ": error " +
                                 std::to_string(GetLastError()));
    }
#else
    if (dlopen(library.c_str(), RTLD_NOW | RTLD_GLOBAL) == nullptr) {
        const char* error = dlerror();
        throw std::runtime_error("cannot load " + utf8(library) + ": " +
                                 (error != nullptr ? error : "unknown error"));
    }
#endif
}

// `args` are the arguments after the program's name, in UTF-8.
int run(std::vector<std::string> args) {
    std::optional<fs::path> preload;
    if (args.size() >= 2 && args[0] == "--load") {
        preload = fs::u8path(args[1]);
        args.erase(args.begin(), args.begin() + 2);
    }
    if (args.size() < 2) {
        std::cerr << "usage: praenora_python_start_probe [--load <library>] <result file> "
                     "<provider search path>...\n";
        return 2;
    }
    const fs::path result_file = fs::u8path(args[0]);
    const std::vector<std::string> search_paths(args.begin() + 1, args.end());
    if (preload) {
        load_globally(*preload);
    }

    const detail::ServiceVersion service{PRAENORA_PYTHON_SERVICE, PRAENORA_PYTHON_SERVICE_MAJOR,
                                         PRAENORA_PYTHON_SERVICE_MINOR};
    const detail::ProviderIndex index =
        detail::ProviderIndex::scan(detail::provider_search_paths(search_paths, false));
    json result = {{"python_manifests", json::array()}, {"manifest", nullptr}, {"started", false}};
    for (const detail::IndexedProvider& entry : index.entries()) {
        if (entry.manifest.name == "python") {
            result["python_manifests"].push_back(
                {{"manifest", utf8(entry.manifest.manifest_path)},
                 {"shadowed_by", entry.shadowed_by ? json(utf8(*entry.shadowed_by)) : json()}});
        }
    }
    const detail::ServiceResolution resolution = index.resolve(service);
    int code = 1;
    if (resolution.provider == nullptr) {
        result["error"] = {{"name", "SERVICE_UNAVAILABLE"},
                           {"details", json::parse(index.unresolved_details(service, resolution))}};
    } else {
        result["manifest"] = utf8(resolution.provider->manifest_path);
        try {
            const detail::LoadedProvider& provider =
                detail::load_provider(*resolution.provider, {service}, std::nullopt, nullptr);
            const auto* table = static_cast<const praenora_python_v0*>(
                provider.service(PRAENORA_PYTHON_SERVICE, PRAENORA_PYTHON_SERVICE_MAJOR));
            TextSink described;
            if (table != nullptr &&
                table->describe(table->context, described.get()) == PRAENORA_STATUS_OK) {
                result["started"] = true;
                result["describe"] = json::parse(described.text());
                code = 0;
            }
        } catch (const praenora::Error& error) {
            result["error"] = json::parse(error.to_json());
        }
    }
    std::ofstream out(result_file, std::ios::binary);
    out << result.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
    if (!out) {
        std::cerr << "praenora_python_start_probe: cannot write " << utf8(result_file) << "\n";
        return 2;
    }
    return code;
}

int guarded(std::vector<std::string> args) {
    try {
        return run(std::move(args));
    } catch (const std::exception& error) {
        std::cerr << "praenora_python_start_probe: " << error.what() << "\n";
    }
    return 2;
}

} // namespace

#ifdef _WIN32
// The arguments arrive as UTF-16 (paths that are not ASCII survive) and become UTF-8.
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.push_back(fs::path(argv[i]).u8string());
    }
    return guarded(std::move(args));
}
#else
int main(int argc, char** argv) { return guarded(std::vector<std::string>(argv + 1, argv + argc)); }
#endif
