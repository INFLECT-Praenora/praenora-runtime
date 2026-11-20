// The runtime's side of one module host (docs/specs/process-isolation.md): it starts the
// praenora-module-host executable, makes the handshake, has the module loaded there, and then
// forwards each call of the one instance it serves over the channel, as the module's code
// (loader/module_code.hpp).
//
// Threads. A reader thread takes every frame from the module host as it arrives: log records and
// service lookups go to the instance at once (InstanceHost), sink chunks to the call in flight,
// and results end their call. The thread that makes a call waits for its result; while it waits it
// passes a raised cancellation (deadline or stop) on to the module host as one cancel frame,
// within a few milliseconds: it looks every kCancelPoll, since whoever raises the flag (a signal
// handler, say) may not be able to wake it. Calls are made one at a time.
//
// Writes. No write of this side waits for good: each is bounded (Channel::write_parts_until(),
// write_parts_while()). The handshake and the load are taken within the handshake's timeout (a
// stop ends the load's); a call is written in slices that a stop ends (the module host, which has
// not begun the call, is killed: the call is cancelled), and the call's deadline and grace end it
// otherwise (ModuleInstance kills the module host); a cancel frame is tried within a poll, and
// again at the next one; the store's answers are taken within ChildSettings::store_reply_wait;
// the request to exit within the wait end() is given.
//
// Untrusted input. Every frame from the module host is checked (kind, limits, fields, the call it
// belongs to); a violation ends the connection and kills the module host, and counts as a crash
// with reason "protocol".
//
// Shared compiled models. The reader answers the store requests of a module host whose step shares
// compiled models (ChildSettings::share_compiled_models) at any time, during the load and between
// calls too, from compiled_model_store() (isolation/compiled_model_store.hpp): a request names the
// provider by its index in the load, and its namespace is that provider's name and the library
// SHA-256 this side sent, never anything the module host says. A request from any other module
// host (refused at its frame's header, before its payload is read), for a provider the load did
// not have, or with a key or an entry over the store's limits, breaks the protocol; so does a
// module host that does not take an answer within ChildSettings::store_reply_wait (the reader never
// blocks for good on a module host that stopped reading, and never holds the channel's writer for
// good). A module host whose process ends while a request is outstanding, or before one, has
// crashed too: the call reports it (CodeStatus::crash) and every later call does at once, without
// reaching the channel. The end is noticed at the end of the channel, and also while a request
// waits, by looking at the process: a process the module started may hold the channel open (a
// forked child); what is left in the module host's process group goes with it.
#ifndef PRAENORA_RUNTIME_ISOLATION_CHILD_MODULE_HPP
#define PRAENORA_RUNTIME_ISOLATION_CHILD_MODULE_HPP

#include "discovery/manifest.hpp"
#include "isolation/channel.hpp"
#include "isolation/child_process.hpp"
#include "isolation/child_settings.hpp"
#include "isolation/sandbox.hpp"
#include "loader/module_code.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace praenora::detail {

// The module host executable: <installation_praenora_dir()>/../../libexec/praenora/
// praenora-module-host, so a build tree and an installation, moved or not, each find their own;
// or in the directory that set_module_host_directory_for_testing() named. nullopt when the
// runtime's binary cannot be located.
std::optional<std::filesystem::path> module_host_executable();

// For tests only: module_host_executable() looks in `directory` from now on, or where it normally
// does again after nullopt.
void set_module_host_directory_for_testing(std::optional<std::filesystem::path> directory);

class ChildModule final : public ModuleCode {
public:
    // What the module host reported about the module it loaded.
    struct Loaded {
        nlohmann::json describe;
        std::string library_sha256;
        bool diagnostics = false, self_test = false;
    };

    // Starts a module host and loads `manifest`'s module in it (LoadedModule::load_in_child()
    // says what it throws), filling `loaded`.
    static std::unique_ptr<ChildModule> start(const ModuleManifest& manifest,
                                              const ChildSettings& settings, Loaded& loaded);

    // Ends the module host as end() does, waiting up to kExitWait for it.
    ~ChildModule() override;
    ChildModule(const ChildModule&) = delete;
    ChildModule& operator=(const ChildModule&) = delete;

    bool has_diagnostics() const noexcept override;
    bool has_self_test() const noexcept override;
    // A module host serves one instance: a second create() is refused here (INVALID_STATE, with
    // an error document), and the first instance keeps the module host.
    CodeStatus create(const std::shared_ptr<InstanceHost>& host, praenora_bytes create_json,
                      praenora_instance** out_instance, OutputSink& error) override;
    CodeStatus initialize(praenora_instance* instance, praenora_bytes initialize_json,
                          OutputSink& error) override;
    CodeStatus invoke(praenora_instance* instance, praenora_bytes invocation_json,
                      OutputSink& result, OutputSink& error) override;
    CodeStatus finalize(praenora_instance* instance, OutputSink& error) override;
    CodeStatus destroy(praenora_instance* instance) override;
    CodeStatus diagnostics(praenora_instance* instance, OutputSink& out) override;
    // UNSUPPORTED: the contract checks run modules in the runtime's process.
    CodeStatus self_test(const praenora_host_api* host, OutputSink& report) override;

    // The module host's process id, also once it has ended.
    std::optional<std::int64_t> process_id() const noexcept override;
    void terminate() noexcept override;
    bool terminated() const noexcept override;
    void end(std::chrono::nanoseconds wait) noexcept override;

    // The number of requests (the load and every call) sent to the module host so far.
    std::uint64_t requests_sent() const noexcept;

    // How long the destructor waits for a module host to exit before it kills it.
    static constexpr std::chrono::seconds kExitWait{5};
    // How often a waiting request looks for a raised cancellation and at the module host's
    // process.
    static constexpr std::chrono::milliseconds kCancelPoll{5};
    // How long a waiting request lets the reader take what a module host sent before it ended,
    // once the process is gone, before the request reports the crash.
    static constexpr std::chrono::milliseconds kDrainWait{100};

private:
    // running: serving; exiting: asked to exit (end()); terminated: killed (terminate());
    // crashed: it ended or broke the protocol on its own.
    enum class State { running, exiting, terminated, crashed };

    // The request waiting for its answer.
    struct Pending {
        bool active = false;
        std::uint64_t id = 0;
        const char* name = nullptr; // "load", "create", ...
        OutputSink* output = nullptr;
        OutputSink* error = nullptr;
        bool done = false;
        praenora_status status = PRAENORA_STATUS_OK;
        bool created = false;
        Frame reply; // of a load
    };

    // How the module host crashed.
    struct Crash {
        std::optional<ChildExit> exit;
        std::string problem; // what broke the protocol; empty when the process ended on its own
    };

    ChildModule(std::string module, std::string manifest_file, ChildSettings settings,
                std::optional<PrivateTemporaryDirectory> temporary_directory,
                ChildProcess::Started started);

    void handshake();
    Loaded load(const ModuleManifest& manifest);
    // Sends one request and waits for its answer, passing a raised cancellation on.
    CodeStatus call(FrameKind kind, const char* name, praenora_bytes document, OutputSink* output,
                    OutputSink* error, bool* created);
    // Sends the request pending_ names (mutex_ not held), in slices while `go_on` says so, and no
    // later than `deadline`, if any (the module host did not take `what`: a protocol violation).
    // A frame that cannot be written ends the connection as an end of the stream would; a stop
    // (go_on false) is left to the caller.
    Channel::WriteStatus send_request(const Frame& frame, const std::function<bool()>& go_on,
                                      std::optional<std::chrono::steady_clock::time_point> deadline,
                                      const char* what);
    // Waits (with `lock` on mutex_) until the pending request is answered or the module host has
    // ended: passes a raised cancellation of `host` on once, and looks at the process every
    // kCancelPoll; a process that has ended is a crash, after kDrainWait. A raised `stop` (the
    // load's ChildSettings::stop_flag) kills the module host instead.
    void wait_for_answer(std::unique_lock<std::mutex>& lock, const InstanceHost* host,
                         bool cancel_sent, const std::atomic<bool>* stop = nullptr);
    // The reader thread's loop.
    void read_frames() noexcept;
    // Handles one frame; false, with the reason, for a protocol violation.
    bool dispatch(Frame& frame, std::string& problem);
    // Handles a store frame (store_load, store_save, store_refused) as dispatch() does.
    bool dispatch_store(Frame& frame, std::string& problem);
    // The provider of a store request that names `index`, with a key of `key_size` bytes, or
    // nullptr with the reason in `problem`: a module host of a step that does not share, an index
    // out of range and a key over the limit break the protocol.
    const ChildProvider* store_provider(std::uint32_t index, std::uint64_t key_size,
                                        std::string& problem) const;
    // Logs one store request of `provider` at debug on the step's logger: `event`, `message` and,
    // after the provider and `key`, the attributes `extra`.
    void log_store(const char* event, const ChildProvider& provider, std::string_view key,
                   const std::string& message,
                   std::vector<std::pair<const char*, std::uint64_t>> extra) const noexcept;
    // The connection is over for this end: the module host crashed, unless it was asked to exit
    // or killed. `problem` is a protocol violation (the module host is killed); when it is empty
    // (an end of the stream), the process is given kEndGrace to end, and is killed after that for
    // the reason `still_running`. `phase_hint` names the request it belongs to when none is
    // outstanding any more. Only the first call decides how the module host ended; the calls that
    // come while it does so return at once.
    void connection_ended(std::string problem, const char* phase_hint = nullptr,
                          const std::string& still_running =
                              "the module host closed its channel but kept running") noexcept;
    // Kills the module host and its process group and waits for them; logs
    // module_host.terminated when `announce` (not for a failed handshake; for one a stop ended).
    void stop_process(bool announce) noexcept;
    // The details of the crash for a request named `phase` (mutex_ held).
    std::string crash_details(const char* phase) const;
    [[noreturn]] void fail_start(const std::string& reason, const std::string& message,
                                 const std::string& system = std::string()) const;
    // Whether the start's stop flag (ChildSettings::stop_flag) is raised.
    bool stop_requested() const noexcept;
    void log(int level, const std::string& message, const char* event) const noexcept;

    std::string module_;        // name@version
    std::string manifest_file_; // UTF-8, for errors located in the manifest
    ChildSettings settings_;
    // The module host's own temporary directory, for the restricted profile: removed once the
    // module host has ended (declared before process_, so destroyed after it).
    std::optional<PrivateTemporaryDirectory> temporary_directory_;
    std::unique_ptr<ChildProcess> process_;
    std::unique_ptr<Channel> channel_;
    std::thread reader_;
    bool diagnostics_ = false, self_test_ = false;

    std::mutex call_mutex_; // one call at a time
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    // Guarded by mutex_.
    State state_ = State::running;
    bool ending_ = false; // a connection_ended() is finding out how the module host ended
    std::shared_ptr<InstanceHost> host_;
    Pending pending_;
    std::uint64_t next_id_ = 1;
    std::uint64_t sent_ = 0;
    std::optional<Crash> crash_; // set with state_ crashed
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_ISOLATION_CHILD_MODULE_HPP
