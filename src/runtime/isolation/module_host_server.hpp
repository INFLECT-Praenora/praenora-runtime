// The module host's side of the channel (docs/specs/process-isolation.md): what the executable
// praenora-module-host (src/module_host/main.cpp) runs. POSIX only; compiled into that executable,
// not into the runtime library.
//
// The runtime starts it with its channel as descriptor 3 and the runtime's process id as its only
// argument. It first ignores the terminal's stop signals SIGTTOU and SIGTTIN (it is a background
// process group of the runtime's terminal), closes every descriptor above 3 (where the system did
// not: macOS closes them at the spawn), on Linux asks for SIGKILL when its parent dies and checks
// that the parent is still the runtime, makes descriptor 3 close-on-exec (no program the module
// runs inherits the channel), answers the handshake, and then serves one module instance:
//   - a reader thread takes the runtime's frames: a cancel frame raises the cancellation flag of
//     its call (no round trip per poll), an exit frame flushes the standard streams and ends the
//     process, and the end of the channel ends the process at once (the runtime is gone), whatever
//     the module is doing; loads and calls go to a call thread, made as the runtime makes an
//     instance's worker thread (the same stack), which makes them one at a time, a load on a
//     thread of its own with a main thread's stack (the soft limit of RLIMIT_STACK, or 8 MiB), as
//     a load in the runtime's process runs on its caller's thread;
//   - the providers of the module's services that the load names are loaded and started first,
//     with the runtime's provider loader (load_provider()) and the SHA-256s validation computed;
//     each provider's log records go to the runtime, with their level and attributes, at the
//     levels the runtime records; a provider's load error names the provider, whose manifest the
//     runtime locates it in;
//   - the module is loaded with LoadedModule::load() and the SHA-256 the runtime expects, with the
//     loader's checks, codes and messages;
//   - the module's host API forwards log records (when the runtime's logger records their level,
//     which comes with each call), serves the runtime services of those providers that the load
//     names (built-in services are not offered in a module host) and reports each lookup, and
//     reads the cancellation flag of the call in flight;
//   - each call's sinks are OutputSinks with the limits the call carries, which pass what they
//     accept on as sink frames, so a module meets the same refusals at the same byte as in the
//     runtime's process;
//   - one writer serializes the frames of every thread of the module host, so everything a call
//     produced reaches the runtime before that call's result;
//   - for a step that shares compiled models (the load says so), each provider's host offers
//     load_compiled and save_compiled, which reach the runtime's store through store requests: a
//     load waits for its answer, which the reader thread hands over; an entry or a key over the
//     store's limits is refused here and only a notice of it is sent. Without, both are NULL.
#ifndef PRAENORA_RUNTIME_ISOLATION_MODULE_HOST_SERVER_HPP
#define PRAENORA_RUNTIME_ISOLATION_MODULE_HOST_SERVER_HPP

namespace praenora::detail {

// Runs the module host; returns the process's exit code when it does not end the process itself
// (2 for a wrong start: arguments, or no channel on descriptor 3).
int run_module_host(int argc, char** argv);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_ISOLATION_MODULE_HOST_SERVER_HPP
