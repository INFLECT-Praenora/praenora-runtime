// The code of a loaded module as a module instance calls it (instance/module_instance.hpp): the
// module's function table in this process, or a module host, another process, reached over its
// channel (isolation/child_module.hpp). LoadedModule owns one; ModuleInstance keeps its state
// machine, deadlines, stop flags, logging and lookup recording whichever it is.
#ifndef PRAENORA_RUNTIME_LOADER_MODULE_CODE_HPP
#define PRAENORA_RUNTIME_LOADER_MODULE_CODE_HPP

#include "loader/output_sink.hpp"

#include <praenora/sdk/abi.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace praenora::detail {

// The instance a module host serves, as the runtime's side of the channel reaches it. What the
// module does with its host API in the module host arrives here and is handled as the same call
// on host_api() would be in this process. Implemented by the instance's host context; thread-safe;
// callable at any time, also between calls.
class InstanceHost {
public:
    virtual ~InstanceHost() = default;

    // The host API the module would be given in this process.
    virtual const praenora_host_api* host_api() const noexcept = 0;
    // A message the module logged at a PRAENORA_LOG_* level: logged as host_api()->log would.
    virtual void log(std::int32_t level, std::string_view message) noexcept = 0;
    // The lowest PRAENORA_LOG_* level that log() records; PRAENORA_LOG_CRITICAL + 1 for none.
    virtual std::int32_t log_threshold() const noexcept = 0;
    // Whether the call in flight has passed its deadline or had a stop requested, as
    // host_api()->cancel_requested reports it.
    virtual bool cancel_requested() const noexcept = 0;
    // Whether the call in flight had a stop requested (not only its deadline passed).
    virtual bool call_stopped() const noexcept = 0;
    // A runtime-service lookup the module made, and whether a table was provided: recorded and
    // logged as host_api()->get_service would.
    virtual void lookup(std::string_view service, std::uint32_t major, bool provided) noexcept = 0;
};

// How one call into a module's code ended: the status the module returned; or, for code in a
// module host whose process ended or broke its channel during the call (or before it), the details
// of that crash, a JSON object's text ({"phase", "signal", "signal_name"}, {"phase", "exit_code"},
// {"phase", "reason": "protocol"} or {"phase", "reason": "ended"}), with status
// PRAENORA_STATUS_INTERNAL.
struct CodeStatus {
    praenora_status status = PRAENORA_STATUS_OK;
    std::optional<std::string> crash;
};

class ModuleCode {
public:
    virtual ~ModuleCode() = default;

    // Whether the module has its optional functions diagnostics() and self_test().
    virtual bool has_diagnostics() const noexcept = 0;
    virtual bool has_self_test() const noexcept = 0;

    // The module's functions (<praenora/sdk/abi.h>). Each blocks until the module returns, or,
    // for code in a module host, until that process ends. What the module writes goes to the
    // sinks given; a sink's limit is the module's limit. `host`, given to create(), serves the
    // instance from then on.
    virtual CodeStatus create(const std::shared_ptr<InstanceHost>& host, praenora_bytes create_json,
                              praenora_instance** out_instance, OutputSink& error) = 0;
    virtual CodeStatus initialize(praenora_instance* instance, praenora_bytes initialize_json,
                                  OutputSink& error) = 0;
    virtual CodeStatus invoke(praenora_instance* instance, praenora_bytes invocation_json,
                              OutputSink& result, OutputSink& error) = 0;
    virtual CodeStatus finalize(praenora_instance* instance, OutputSink& error) = 0;
    virtual CodeStatus destroy(praenora_instance* instance) = 0;
    virtual CodeStatus diagnostics(praenora_instance* instance, OutputSink& out) = 0;
    // Only for code in this process (the contract checks): UNSUPPORTED otherwise.
    virtual CodeStatus self_test(const praenora_host_api* host, OutputSink& report) = 0;

    // Code in a module host. The process's id while it runs; nullopt in this process.
    virtual std::optional<std::int64_t> process_id() const noexcept { return std::nullopt; }
    // Kills the module host and waits for it: a call in flight returns at once. Nothing in this
    // process.
    virtual void terminate() noexcept {}
    // Code in a module host: whether the runtime killed its process (terminate(), or a stop that
    // came before the module host had taken a call), so that no call reaches the module any more
    // and its instance is gone with the process. A crash is not this: the calls report it. Always
    // false in this process.
    virtual bool terminated() const noexcept { return false; }
    // Asks the module host to exit, waits up to `wait`, kills it after that, and waits for it.
    // Nothing in this process.
    virtual void end(std::chrono::nanoseconds wait) noexcept { static_cast<void>(wait); }
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_LOADER_MODULE_CODE_HPP
