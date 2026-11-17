// The process-wide provider loader: loads a service provider's library, checks it, starts it once
// per process and keeps it loaded until the process ends (docs/specs/runtime-services.md).
//
// The state is the process's, not a Runtime's: whichever Runtime, run or check loads a provider
// first loads and starts it, and every later load in the process reuses it. A mutex guards it.
#ifndef PRAENORA_RUNTIME_PROVIDER_PROVIDER_LOADER_HPP
#define PRAENORA_RUNTIME_PROVIDER_PROVIDER_LOADER_HPP

#include "provider/provider_abi.h"
#include "provider/provider_manifest.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace praenora::detail {

class LogHub;
class ProviderHost;
class SharedLibrary;

// A provider in the process. Once its library has opened it stays, whatever its later checks or
// its start() do: it is never unloaded, stopped or destroyed, and references to it stay valid until
// the process ends. load_provider() returns only providers that started.
class LoadedProvider {
public:
    LoadedProvider(const LoadedProvider&) = delete;
    LoadedProvider& operator=(const LoadedProvider&) = delete;
    ~LoadedProvider();

    // The manifest of the load that started it.
    const ProviderManifest& manifest() const noexcept;
    // The SHA-256 of its library file when it was loaded.
    const std::string& library_sha256() const noexcept;
    // Its describe() document, which agrees with the manifest.
    const nlohmann::json& describe() const noexcept;
    // The table of service `name` at `major` that get_service() returned to the latest load that
    // asked for it, or nullptr when no load asked for it. Thread-safe.
    const void* service(std::string_view name, std::uint32_t major) const;

private:
    friend class ProviderRegistry;
    LoadedProvider() = default;

    // opened: the library is in the process, start() has not been called (a check after the
    // library opened failed, or start() is next); start_failed: start() failed; started.
    enum class State { opened, start_failed, started };

    State state_ = State::opened;
    ProviderManifest manifest_; // of the load that opened the library, then of the one that started
    std::filesystem::path canonical_library_;
    std::string library_sha256_;
    std::unique_ptr<SharedLibrary> library_; // pinned
    nlohmann::json describe_;
    praenora_provider_api_v0 api_{};
    std::unique_ptr<ProviderHost> host_; // made once, before the first start(); kept for good
    std::optional<std::pair<std::string, std::string>> start_failure_; // message, details JSON
    mutable std::mutex services_mutex_;
    std::map<std::pair<std::string, std::uint32_t>, const void*, std::less<>> services_;
};

// Checks the library file of the provider of `manifest` without loading it, as validation does and
// as load_provider() does first: it exists (LIBRARY_NOT_FOUND) and is readable (IO_ERROR), both at
// /library/path, and has the SHA-256 the manifest pins, if any (LIBRARY_DIGEST_MISMATCH at
// /library/sha256). Every error is located in the manifest, with the message
// "provider <name>@<version>: ...". Returns the file's SHA-256.
std::string check_provider_library_file(const ProviderManifest& manifest);

// Loads the provider of `manifest` into the process, unless it is loaded already, and gets the
// tables of `services` from it. Every error is a praenora::Error located in the manifest, with the
// message "provider <name>@<version>: ...". The checks, in order:
//   0. the manifest offers each of `services` (its name and major, at a minor of at least the
//      one given); otherwise INVALID_ARGUMENT: the caller resolved the services to this provider;
//   1. the library file exists (LIBRARY_NOT_FOUND) and is readable (IO_ERROR), at /library/path;
//      its SHA-256 equals the manifest's pinned library.sha256 (LIBRARY_DIGEST_MISMATCH at
//      /library/sha256) and `validated_sha256`, when given (LIBRARY_DIGEST_MISMATCH at
//      /library/path: the library changed after validation);
//   2. when a library of the manifest's name is in the process already, whatever became of it:
//      it is the same library file (by its canonical path) with the same SHA-256; otherwise
//      LIBRARY_LOAD_FAILED with details.reason "conflict" at /library/path: one process never
//      holds two libraries, and so never two engines, of one provider. Otherwise the library loads
//      (dlopen(RTLD_NOW | RTLD_LOCAL) or LoadLibraryExW()): LIBRARY_LOAD_FAILED with reason
//      "library" and the operating system's text, at /library/path. From here on the library is in
//      the process and pinned: it is never unloaded, whatever fails later;
// Then, by what became of the provider:
//   - its start() failed: that failure again (LIBRARY_LOAD_FAILED, reason "start"): start() is
//     called once per process;
//   - it started: its describe() agrees with this manifest (reason "identity", as below);
//   - start() was not called yet (the library opened now, or a check after it opened failed
//     before): these checks, which are free of side effects, run again with this manifest:
//     3. the library exports praenora_provider_get_api (ENTRY_POINT_MISSING);
//     4. the entry point returns a table whose header reports provider interface major
//        PRAENORA_PROVIDER_ABI_MAJOR, reserved 0 and a struct_size that covers the v0 table, and
//        none of whose functions is NULL (API_TABLE_INVALID);
//     5. describe() succeeds and writes {"name", "version", "services", "engine"} that equals the
//        manifest in name, version (SemVer equality), services (as a set) and engine (JSON
//        equality): LIBRARY_LOAD_FAILED with reason "identity" and, when the document has that
//        shape, details.differences [{"field", "manifest", "describe"}];
//     6. start() succeeds, called exactly once in the process, under the loader's lock: otherwise
//        LIBRARY_LOAD_FAILED with reason "start" and the provider's error document as
//        details.cause; the failure is kept, and every later load reports it;
//     7. the event provider.loaded is logged (info).
// Then, for every load:
//   - with `logs`, the provider's records go to its logger "provider.<name>" of `logs` from now
//     on (without, they go where they went). When those logs are destroyed, the provider's records
//     go nowhere until a load gives it logs again: it holds none of their sinks or files;
//   - get_service() returns a table for each of `services`; otherwise LIBRARY_LOAD_FAILED with
//     reason "service", details.services ["<name>@<major>", ...] and the pointer of the first one
//     in the manifest's services.
// Thread-safe: concurrent loads of one provider load and start it once.
const LoadedProvider& load_provider(const ProviderManifest& manifest,
                                    const std::vector<ServiceVersion>& services,
                                    const std::optional<std::string>& validated_sha256,
                                    LogHub* logs);

// The provider loaded and started in this process under `name`, or nullptr. Thread-safe.
const LoadedProvider* loaded_provider(std::string_view name);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_PROVIDER_PROVIDER_LOADER_HPP
