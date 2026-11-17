// The contract checks of one module (`praenora modules check`, Runtime::check_module()): stable
// check ids, each with a result (pass, fail or skip) and a message.
//
// The checks, in report order; each one runs only when the check before it in its chain passed,
// otherwise it is skipped with the reason (the failed check, or the reason of the skipped one):
//   manifest.schema         the file is a JSON document of kind module-manifest that satisfies
//                           the module-manifest schema of its format_version;
//   manifest.semantics      what the schema cannot check (load_manifest(): relative paths, unique
//                           capabilities, a compiling parameters_schema), compatibility with this
//                           runtime (platform, ABI major, PMP version) and that the catalog holds
//                           each capability and the message types of its ports;
//   library.digest ... describe.identity
//                           the checks of LoadedModule::load(), one id each: the library file and
//                           its pinned SHA-256, loading it, the entry point, the table's header,
//                           the required functions, the describe() document and its identity.
//                           Before the library loads, library.load resolves the services the
//                           manifest declares on the runtime's provider search paths and loads
//                           and starts their providers, as a run does: a required service that
//                           does not resolve fails it with SERVICE_UNAVAILABLE, a provider that
//                           does not load with the provider loader's error (the message starts
//                           with the error's name), and the module's instances are then served
//                           the provider services that resolved, as in a run;
//   lifecycle.create_destroy  an instance is created and destroyed;
//   lifecycle.full          an instance is created, initialized, finalized and destroyed;
//   invoke.result_schema    every case of the test vectors runs (create, initialize, one invoke
//                           per tick, finalize, destroy) and every result is JSON, satisfies
//                           pmp-result, names only output ports of the case's capability and each
//                           required one, and each output's data satisfies its port's message type;
//   invoke.expected_outputs the outputs of each tick that has an `expect` equal the expected
//                           outputs (the same ports; numbers within the tick's tolerance,
//                           json_near());
//   invoke.determinism      every case runs a second time and gives byte-identical results;
//   invoke.malformed_input  invoke() of an initialized instance refuses the document "{" with an
//                           error status (and the process survives);
//   self_test               the module's self_test(), when its table has one, reports passed;
//   unload                  once every instance is destroyed, the library is released.
// The lifecycle checks and invoke.malformed_input use the first case of the test vectors, or,
// without usable vectors, the manifest's first capability with the parameters_schema's defaults.
// The checks that need test vectors are skipped without them; invalid vectors fail
// invoke.result_schema. After a call that never returns (unresponsive), the checks that would
// call into the module again are skipped, and unload fails.
//
// The checks' instances are those of a step named "check", with the deadlines invoke 30 s (also
// for self_test) and lifecycle 10 s, and a cancellation grace of 5 s. A case's documents are the
// engine's: create {pmp_version, step, capability, module, parameters (the case's, with the
// parameters_schema's defaults)}, initialize {pmp_version, run {fingerprint, seed (the case's)},
// time (the case's)}, and for tick t an invocation {pmp_version, step, tick t, logical_time (the
// end of the tick's window), inputs}, each input's data wrapped in an envelope of its port's
// message type with source inputs.<port>. The run fingerprint is the SHA-256 of the case's
// canonical JSON (with the vectors' defaults).
#ifndef PRAENORA_RUNTIME_API_MODULE_CHECK_HPP
#define PRAENORA_RUNTIME_API_MODULE_CHECK_HPP

#include "api/validation.hpp"
#include "observability/logging.hpp"

#include <praenora/runtime/runtime.hpp>

#include <spdlog/logger.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>

namespace praenora::detail {

// The deadlines of the checks' calls into the module: invoke (also self_test), every other call,
// and the cancellation grace after a deadline. Tests may shorten them.
struct CheckDeadlines {
    std::chrono::nanoseconds invoke = std::chrono::seconds(30);
    std::chrono::nanoseconds lifecycle = std::chrono::seconds(10);
    std::chrono::nanoseconds cancel_grace = std::chrono::seconds(5);
};

// Runs the contract checks of the module whose manifest is `manifest`, with the test vectors in
// `vectors` (by default the ones the manifest's tests.vectors names, if any), against the catalog,
// the provider index and the runtime services of `context`. `logger` receives the module's log
// messages (none when null), `logs` the records of the providers the checks load (none when
// null). Problems of the module, its manifest or the vectors are reported as failed checks. Only a
// manifest file that cannot be read (missing, for example) throws praenora::Error(IO_ERROR), and
// a vectors file that cannot be read, which is opened only once the manifest checks have passed
// (when they fail, the vectors are never read); both before any of the module's code runs.
ModuleCheckReport check_module(const std::filesystem::path& manifest,
                               const std::optional<std::filesystem::path>& vectors,
                               const RuntimeContext& context,
                               std::shared_ptr<spdlog::logger> logger,
                               const CheckDeadlines& deadlines = {}, LogHub* logs = nullptr);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_API_MODULE_CHECK_HPP
