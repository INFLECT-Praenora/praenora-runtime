// How the runtime starts a module host for a module (LoadedModule::load_in_child(),
// docs/specs/process-isolation.md), and how it describes a module host's crash. Apart from the
// runtime's side of the channel (isolation/child_module.hpp), so that the loader and the engine
// need nothing else of it.
#ifndef PRAENORA_RUNTIME_ISOLATION_CHILD_SETTINGS_HPP
#define PRAENORA_RUNTIME_ISOLATION_CHILD_SETTINGS_HPP

#include "instance/services.hpp"
#include "isolation/sandbox.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace spdlog {
class logger;
} // namespace spdlog

namespace praenora::detail {

struct ProviderManifest;

// A service provider that the module host loads and starts, with the runtime's provider loader,
// before it loads the module's library (docs/specs/process-isolation.md#service-providers).
struct ChildProvider {
    // Its manifest, as validation read it. The module host reads the file again and refuses it
    // (MANIFEST_INVALID) when its SHA-256 is not this one's.
    std::shared_ptr<const ProviderManifest> manifest;
    // The SHA-256 its library must have (validation's), checked in the module host before the
    // library opens: LIBRARY_DIGEST_MISMATCH at /library/path of its manifest otherwise.
    std::string library_sha256;
    // The services the module gets from it: each name and major, with the minor it offers.
    std::vector<ServiceVersion> services;
    // Receives the provider's log records, with their attributes, as its logger provider.<name>
    // receives them in this process; the module host records the levels this logger records. None:
    // the provider records nothing.
    std::shared_ptr<spdlog::logger> logger;
};

struct ChildSettings {
    // The SHA-256 the module's library must have (validation's), checked in the module host
    // before the library is opened: LIBRARY_DIGEST_MISMATCH at /library/path otherwise. None:
    // only the manifest's own library.sha256, if it pins one, is checked.
    std::optional<std::string> library_sha256;
    // Receives the module host's events: module_host.started and module_host.exited (debug),
    // module_host.terminated (warn) and module_host.crashed (error), with the attributes module
    // and pid. None: they are dropped.
    std::shared_ptr<spdlog::logger> logger;
    // How long the module host may take to answer the handshake.
    std::chrono::nanoseconds start_timeout{std::chrono::seconds(30)};
    // A stop of whoever loads the module (a run's stop flag), looked at every few milliseconds
    // until the load has its answer, and never after it: once raised, no module host is started,
    // or the one started is killed and waited for, and the load fails with CANCELLED, located in
    // the manifest. The load has no timeout of its own, as in the runtime's process. None: only
    // the handshake's timeout ends a start.
    const std::atomic<bool>* stop_flag = nullptr;
    // The providers of the module's services (none for a module that uses none), loaded and
    // started in the module host, in this order, before the module's library: their errors keep
    // the provider loader's codes and messages, located in their manifests. get_service() is
    // served their services there, and no other: the runtime's built-in services are not offered
    // in a module host.
    std::vector<ChildProvider> providers;
    // Whether the module host shares compiled models with the other sharing module hosts of this
    // process (docs/specs/process-isolation.md#shared-compiled-models): its providers get the
    // store's callbacks, and the runtime answers its store requests from compiled_model_store(),
    // under the namespace of the provider each request names by its index in `providers`, logging
    // each one (compiled_models.hit, .miss, .saved, .refused, at debug) to `logger`. Without, its
    // providers get no store, and a store request from it breaks the channel's protocol.
    bool share_compiled_models = false;
    // The sandbox profile the module host applies to itself before it loads any provider or
    // module library (isolation/sandbox.hpp): restricted gives it a private temporary directory,
    // which its TMPDIR names and which is removed once it has ended. A restricted profile that
    // cannot be made or applied (off macOS, or in a process that runs in a sandbox already) fails
    // the load with MODULE_HOST_UNAVAILABLE, details.reason "sandbox", with the system's text.
    SandboxProfile sandbox = SandboxProfile::none;
    // How long the reader waits for a sharing module host to take the answer to one of its store
    // requests (it may be up to 128 MiB): a module host that does not, because it stopped reading
    // its channel, breaks the channel's protocol, and is killed.
    std::chrono::nanoseconds store_reply_wait{std::chrono::seconds(30)};
};

// How a module host crashed, from the details of a MODULE_CRASHED: "was ended by signal 11
// (SIGSEGV)", "exited with code 0", "broke the channel's protocol", or "ended" when how is not
// known.
std::string describe_crash(const std::string& details_json);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_ISOLATION_CHILD_SETTINGS_HPP
