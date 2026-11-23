#include "isolation/child_module.hpp"

#include "core/deadline.hpp"
#include "core/paths.hpp"
#include "discovery/search_paths.hpp"
#include "document/json_util.hpp"
#include "isolation/compiled_model_store.hpp"
#include "loader/loaded_module.hpp"
#include "observability/logging.hpp"
#include "platform/platform.hpp"
#include "provider/provider_manifest.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/version.hpp>
#include <praenora/sdk/status.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace praenora::detail {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// The executable's name, in <prefix>/libexec/praenora.
constexpr const char* kModuleHostName = "praenora-module-host";

// How long a module host whose channel ended gets to end its process before it is killed. A
// crashing process closes its channel only as it is torn down, so this is normally far shorter;
// the margin keeps a crash on a loaded machine from being taken for a broken protocol.
constexpr std::chrono::seconds kEndGrace{5};

// What create() hands out for the instance a module host holds: never dereferenced.
char g_child_instance_marker = 0;
praenora_instance* child_instance() noexcept {
    return reinterpret_cast<praenora_instance*>(&g_child_instance_marker);
}

std::mutex& test_directory_mutex() {
    static auto* const mutex = new std::mutex;
    return *mutex;
}

std::optional<fs::path>& test_directory() {
    static auto* const directory = new std::optional<fs::path>;
    return *directory;
}

bool is_lower_hex_sha256(const std::string& text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

// Why a module host executable cannot be run: details.reason, the message, and the system's text.
struct Unusable {
    std::string reason, message, system;
};

std::optional<Unusable> unusable_executable(const fs::path& path) {
    const std::string file = "the module host executable '" + path_to_utf8(path) + "'";
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return Unusable{"missing", file + " does not exist", std::string()};
    }
#ifndef _WIN32
    if (::access(path.c_str(), X_OK) != 0) {
        const std::string system = std::strerror(errno);
        return Unusable{"spawn", file + " is not executable: " + system, system};
    }
#endif
    return std::nullopt;
}

// A request's document and the limits of its sinks, as a call frame carries them.
std::string call_payload(const OutputSink* output, const OutputSink* error, std::int32_t threshold,
                         bool cancelled, praenora_bytes document) {
    return PayloadWriter()
        .u64(output != nullptr ? output->limit() : 0)
        .u64(error != nullptr ? error->limit() : 0)
        .i32(threshold)
        .u8(cancelled ? 1 : 0)
        .bytes(document.data == nullptr ? std::string_view()
                                        : std::string_view(document.data, document.size))
        .take();
}

// The size of a call frame's fields before its document.
constexpr std::size_t kCallFields = 8 + 8 + 4 + 1;

// CANCELLED for a start or load of `module` (name@version) that a stop ended, located in its
// manifest `manifest_file`.
Error stopped(const std::string& module, const std::string& manifest_file, const char* when) {
    SourceLocation location;
    location.file = manifest_file;
    return Error(ErrorCode::CANCELLED,
                 "module " + module + ": the load was stopped " + when +
                     ": a stop was requested, and the module host was ended",
                 std::move(location));
}

// The lowest PRAENORA_LOG_* level that `logger` records; PRAENORA_LOG_CRITICAL + 1 for none.
std::int32_t threshold_of(const std::shared_ptr<spdlog::logger>& logger) noexcept {
    if (logger != nullptr) {
        for (std::int32_t level = PRAENORA_LOG_TRACE; level <= PRAENORA_LOG_CRITICAL; ++level) {
            if (log_enabled(*logger, log_level_from_abi(level))) {
                return level;
            }
        }
    }
    return PRAENORA_LOG_CRITICAL + 1;
}

// The providers of a load request: what the module host needs to load and start each one, as
// the runtime's provider loader does in this process.
json providers_of(const std::vector<ChildProvider>& providers) {
    json list = json::array();
    for (const ChildProvider& provider : providers) {
        json services = json::array();
        for (const ServiceVersion& service : provider.services) {
            services.push_back(
                {{"name", service.name}, {"major", service.major}, {"minor", service.minor}});
        }
        list.push_back({{"manifest", path_to_utf8(provider.manifest->manifest_path)},
                        {"manifest_sha256", provider.manifest->manifest_sha256},
                        {"library", path_to_utf8(provider.manifest->library_path)},
                        {"library_sha256", provider.library_sha256},
                        {"services", std::move(services)},
                        {"log_level", threshold_of(provider.logger)}});
    }
    return list;
}

// Writes an error document of the host to `error`, as a module would write its own.
void write_host_error(OutputSink* error, praenora_status code, const std::string& message) {
    if (error == nullptr) {
        return;
    }
    const std::string text =
        json{{"code", code}, {"message", message}, {"details", json::object()}}.dump();
    praenora_sink* sink = error->sink();
    static_cast<void>(sink->write(sink->context, text.data(), text.size()));
}

} // namespace

std::optional<fs::path> module_host_executable() {
    {
        const std::lock_guard<std::mutex> lock(test_directory_mutex());
        if (test_directory()) {
            return *test_directory() / kModuleHostName;
        }
    }
    const auto praenora_dir = installation_praenora_dir();
    if (!praenora_dir) {
        return std::nullopt;
    }
    return (*praenora_dir / ".." / ".." / "libexec" / "praenora" / kModuleHostName)
        .lexically_normal();
}

void set_module_host_directory_for_testing(std::optional<fs::path> directory) {
    const std::lock_guard<std::mutex> lock(test_directory_mutex());
    test_directory() = std::move(directory);
}

std::string describe_crash(const std::string& details_json) {
    const json details = json::parse(details_json, nullptr, false);
    if (!details.is_object()) {
        return "ended";
    }
    if (details.value("reason", std::string()) == "protocol") {
        return "broke the channel's protocol";
    }
    if (details.contains("signal") && details.at("signal").is_number_integer()) {
        return "was ended by signal " + std::to_string(details.at("signal").get<int>()) + " (" +
               details.value("signal_name", std::string()) + ")";
    }
    if (details.contains("exit_code") && details.at("exit_code").is_number_integer()) {
        return "exited with code " + std::to_string(details.at("exit_code").get<int>());
    }
    return "ended";
}

ChildModule::ChildModule(std::string module, std::string manifest_file, ChildSettings settings,
                         std::optional<PrivateTemporaryDirectory> temporary_directory,
                         ChildProcess::Started started)
    : module_(std::move(module)), manifest_file_(std::move(manifest_file)),
      settings_(std::move(settings)), temporary_directory_(std::move(temporary_directory)),
      process_(std::move(started.process)), channel_(std::make_unique<Channel>(started.channel)) {
    // A module host whose step does not share compiled models never sends a store frame: one is
    // refused at its header, before the runtime reads any of what it announces (a save may announce
    // up to 128 MiB), and breaks the protocol.
    if (!settings_.share_compiled_models) {
        channel_->refuse_kinds(
            {FrameKind::store_load, FrameKind::store_save, FrameKind::store_refused},
            "came from the module host of a step that does not share compiled models");
    }
}

std::unique_ptr<ChildModule> ChildModule::start(const ModuleManifest& manifest,
                                                const ChildSettings& settings, Loaded& loaded) {
    const std::string module = manifest.name + "@" + to_string(manifest.version);
    const std::string manifest_file = path_to_utf8(manifest.manifest_path);
    const auto unavailable = [&](const std::string& reason, const std::string& message,
                                 json details) -> Error {
        details["reason"] = reason;
        SourceLocation location;
        location.file = manifest_file;
        return Error(ErrorCode::MODULE_HOST_UNAVAILABLE, "module " + module + ": " + message,
                     std::move(location), details.dump());
    };
    if (!process_isolation_available()) {
        throw unavailable("spawn", "process isolation is not available on Windows in this version",
                          json::object());
    }
    if (settings.stop_flag != nullptr && settings.stop_flag->load()) {
        throw stopped(module, manifest_file, "before its module host started");
    }
    const auto executable = module_host_executable();
    if (!executable) {
        throw unavailable("missing",
                          "the module host executable cannot be located: the runtime's own file "
                          "is unknown",
                          json::object());
    }
    if (const auto unusable = unusable_executable(*executable)) {
        json details = {{"executable", path_to_utf8(*executable)}};
        if (!unusable->system.empty()) {
            details["system"] = unusable->system;
        }
        throw unavailable(unusable->reason, unusable->message, std::move(details));
    }
    // The restricted profile's temporary directory, the module host's own.
    std::optional<PrivateTemporaryDirectory> temporary_directory;
    if (settings.sandbox == SandboxProfile::restricted) {
        if (!restricted_sandbox_available()) {
            throw unavailable("sandbox",
                              "the restricted sandbox profile is available only on macOS in this "
                              "version",
                              json::object());
        }
        std::string problem;
        temporary_directory = PrivateTemporaryDirectory::make(problem);
        if (!temporary_directory) {
            throw unavailable("sandbox",
                              "cannot make the module host's temporary directory: " + problem,
                              {{"system", problem}});
        }
    }
    ChildProcess::Started started;
    try {
        started = ChildProcess::start(*executable, {std::to_string(current_process_id())});
    } catch (const Error& error) {
        json details = json::parse(error.details_json(), nullptr, false);
        throw unavailable("spawn", error.what(), details.is_object() ? details : json::object());
    }
    std::unique_ptr<ChildModule> child(new ChildModule(
        module, manifest_file, settings, std::move(temporary_directory), std::move(started)));
    try {
        child->handshake();
    } catch (...) {
        child->stop_process(false); // not a module host that can be used: nothing to ask it
        throw;
    }
    child->log(PRAENORA_LOG_DEBUG,
               "module host started for module " + module + " (pid " +
                   std::to_string(child->process_->pid()) + ")",
               "module_host.started");
    child->reader_ = std::thread([raw = child.get()] { raw->read_frames(); });
    loaded = child->load(manifest);
    // The flag is the caller's, and only the start looks at it: the module host may outlive it.
    child->settings_.stop_flag = nullptr;
    child->diagnostics_ = loaded.diagnostics;
    child->self_test_ = loaded.self_test;
    return child;
}

ChildModule::~ChildModule() {
    end(kExitWait);
    // The module host has been waited for: its temporary directory goes now, and what cannot go is
    // said, so that nothing is left in the system's temporary directory unnoticed.
    if (temporary_directory_) {
        if (const auto left = temporary_directory_->remove()) {
            try {
                log(PRAENORA_LOG_WARN,
                    "the temporary directory of the module host of module " + module_ + " (pid " +
                        std::to_string(process_->pid()) +
                        ") could not be removed, and is left: " + *left,
                    "module_host.directory_left");
            } catch (...) {
                // Out of memory while building the warning; the directory is left all the same.
            }
        }
    }
}

[[noreturn]] void ChildModule::fail_start(const std::string& reason, const std::string& message,
                                          const std::string& system) const {
    json details = {{"reason", reason}};
    if (!system.empty()) {
        details["system"] = system;
    }
    SourceLocation location;
    location.file = manifest_file_;
    throw Error(ErrorCode::MODULE_HOST_UNAVAILABLE, "module " + module_ + ": " + message,
                std::move(location), details.dump());
}

void ChildModule::handshake() {
    const VersionInfo version = version_info();
    Frame hello;
    hello.kind = FrameKind::hello;
    hello.payload = PayloadWriter().u32(kChannelProtocol).bytes(version.runtime).take();
    const auto ended = [this](const std::string& what) {
        const auto exit = process_->wait(kEndGrace);
        return what + (exit ? " (it " + describe(*exit) + ")" : std::string());
    };
    const auto deadline = deadline_after(std::chrono::steady_clock::now(), settings_.start_timeout);
    std::string problem;
    if (channel_->write_parts_until(FrameKind::hello, 0, {hello.payload}, deadline, &problem) !=
        Channel::WriteStatus::written) {
        fail_start("handshake", ended("the module host did not take the handshake: " + problem));
    }
    // Until the answer begins, a stop is looked for every kCancelPoll; the answer is then read
    // whole (within the same deadline). A stop kills the module host, as during the load, and
    // says so (module_host.terminated), although it was never announced as started.
    for (;;) {
        if (stop_requested()) {
            stop_process(true);
            throw stopped(module_, manifest_file_, "while its module host started");
        }
        const auto now = std::chrono::steady_clock::now();
        if (channel_->wait_readable(std::min(deadline, deadline_after(now, kCancelPoll))) ||
            now >= deadline) {
            break;
        }
    }
    const Channel::ReadResult read = channel_->read(deadline);
    switch (read.status) {
    case Channel::ReadStatus::frame:
        break;
    case Channel::ReadStatus::timed_out:
        fail_start("handshake", "the module host did not answer the handshake within " +
                                    std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                                       settings_.start_timeout)
                                                       .count()) +
                                    " s");
    case Channel::ReadStatus::end:
        fail_start("handshake", ended("the module host ended before it answered the handshake"));
    case Channel::ReadStatus::broken:
        fail_start("handshake", "the module host answered the handshake with something that is "
                                "not a frame of this runtime: " +
                                    read.problem);
    }
    if (read.frame.kind != FrameKind::ready) {
        fail_start("handshake", std::string("the module host answered the handshake with a ") +
                                    frame_kind_name(static_cast<std::uint32_t>(read.frame.kind)) +
                                    " frame");
    }
    PayloadReader reader(read.frame.payload);
    std::uint32_t protocol = 0;
    if (!reader.u32(protocol)) {
        fail_start("handshake", "the module host's answer to the handshake is too short");
    }
    const std::string their_version(reader.rest());
    if (protocol != kChannelProtocol || their_version != version.runtime) {
        fail_start("build", "the module host is of another runtime version or channel protocol "
                            "(channel protocol " +
                                std::to_string(protocol) + ", version " + their_version +
                                "; this runtime: channel protocol " +
                                std::to_string(kChannelProtocol) + ", version " + version.runtime +
                                ")");
    }
}

bool ChildModule::stop_requested() const noexcept {
    return settings_.stop_flag != nullptr && settings_.stop_flag->load();
}

ChildModule::Loaded ChildModule::load(const ModuleManifest& manifest) {
    json request = {{"manifest", manifest_file_},
                    {"manifest_sha256", manifest.manifest_sha256},
                    {"library_sha256",
                     settings_.library_sha256 ? json(*settings_.library_sha256) : json(nullptr)},
                    {"providers", providers_of(settings_.providers)},
                    {"share_compiled_models", settings_.share_compiled_models},
                    {"sandbox", std::string(sandbox_profile_name(settings_.sandbox))},
                    {"temporary_directory",
                     temporary_directory_ ? json(temporary_directory_->path()) : json(nullptr)}};
    std::unique_lock<std::mutex> lock(mutex_);
    pending_ = Pending{};
    pending_.active = true;
    pending_.id = next_id_++;
    pending_.name = "load";
    Frame frame;
    frame.kind = FrameKind::load;
    frame.id = pending_.id;
    frame.payload = request.dump();
    ++sent_;
    lock.unlock();
    // A module host takes its load at once; it has the handshake's time to, and a stop ends it.
    const auto taken_by = deadline_after(std::chrono::steady_clock::now(), settings_.start_timeout);
    if (send_request(
            frame, [this] { return !stop_requested(); }, taken_by, "its load") ==
        Channel::WriteStatus::stopped) {
        stop_process(true); // the stop is reported below, as during the load
    }
    lock.lock();
    wait_for_answer(lock, nullptr, false, settings_.stop_flag);
    const bool answered = pending_.done;
    Frame reply = std::move(pending_.reply);
    pending_ = Pending{};
    SourceLocation location;
    location.file = manifest_file_;
    if (!answered && state_ == State::terminated) {
        lock.unlock();
        throw stopped(module_, manifest_file_, "while its module host loaded it");
    }
    if (!answered) {
        const std::string details = crash_ ? crash_details("load") : json{{"phase", "load"}}.dump();
        lock.unlock();
        throw Error(ErrorCode::MODULE_CRASHED,
                    "module " + module_ +
                        ": the module host crashed while it loaded the module: it " +
                        describe_crash(details),
                    std::move(location), details);
    }
    lock.unlock();

    // The answer is untrusted: anything malformed is a protocol violation.
    const auto violation = [this, &location](const std::string& problem) -> Error {
        connection_ended(problem, "load");
        std::unique_lock<std::mutex> guard(mutex_);
        // When another thread was first to see the end, it says how the module host ended.
        changed_.wait(guard, [this] { return state_ != State::running; });
        const std::string details = crash_details("load");
        return Error(ErrorCode::MODULE_CRASHED,
                     "module " + module_ +
                         ": the module host crashed while it loaded the module: " + problem,
                     location, details);
    };
    // Bounded nesting (parse_json_text()): a frame within its length limit can still hold JSON
    // nested too deep to handle safely.
    json answer;
    try {
        answer = parse_json_text(reply.payload, "the module host's answer to the load");
    } catch (const Error&) {
        throw violation("its answer to the load is not JSON");
    }
    if (!answer.is_object()) {
        throw violation("its answer to the load is not a JSON object");
    }
    if (reply.kind == FrameKind::load_failed) {
        const auto code = answer.find("code");
        const auto message = answer.find("message");
        const auto pointer = answer.find("pointer");
        const auto line = answer.find("line");
        const auto column = answer.find("column");
        const auto details = answer.find("details");
        const auto provider = answer.find("provider");
        if (code == answer.end() || !code->is_number_integer() || message == answer.end() ||
            !message->is_string() || pointer == answer.end() || !pointer->is_string() ||
            line == answer.end() || !line->is_number_integer() || column == answer.end() ||
            !column->is_number_integer() || details == answer.end() || !details->is_string()) {
            throw violation("its load error lacks a field or has one of the wrong type");
        }
        // An error of a provider's load is located in that provider's manifest, as the runtime's
        // provider loader locates it: the runtime names the file, from what it sent.
        if (provider != answer.end() && !provider->is_null()) {
            if (!provider->is_number_unsigned() ||
                provider->get<std::uint64_t>() >= settings_.providers.size()) {
                throw violation("its load error names no provider of the load");
            }
            location.file = path_to_utf8(
                settings_.providers[provider->get<std::size_t>()].manifest->manifest_path);
        }
        const auto value = static_cast<ErrorCode>(code->get<std::int64_t>());
        if (code->get<std::int64_t>() <= 0 ||
            code->get<std::int64_t>() > std::numeric_limits<std::int32_t>::max() ||
            error_code_info(value).code != value) {
            throw violation("its load error has the unknown code " + code->dump());
        }
        location.pointer = pointer->get<std::string>();
        location.line = std::max<std::int64_t>(line->get<std::int64_t>(), 0);
        location.column = std::max<std::int64_t>(column->get<std::int64_t>(), 0);
        bool details_object = false;
        try {
            details_object = parse_json_text(details->get<std::string>(), "details").is_object();
        } catch (const Error&) {
            // Not JSON: the error goes without its details.
        }
        throw Error(value, message->get<std::string>(), std::move(location),
                    details_object ? details->get<std::string>() : "{}");
    }
    Loaded loaded;
    const auto describe = answer.find("describe");
    const auto sha256 = answer.find("library_sha256");
    const auto diagnostics = answer.find("diagnostics");
    const auto self_test = answer.find("self_test");
    if (describe == answer.end() || !describe->is_object() || sha256 == answer.end() ||
        !sha256->is_string() || !is_lower_hex_sha256(sha256->get<std::string>()) ||
        diagnostics == answer.end() || !diagnostics->is_boolean() || self_test == answer.end() ||
        !self_test->is_boolean()) {
        throw violation("its load result lacks a field or has one of the wrong type");
    }
    loaded.describe = *describe;
    loaded.library_sha256 = sha256->get<std::string>();
    loaded.diagnostics = diagnostics->get<bool>();
    loaded.self_test = self_test->get<bool>();
    if (settings_.library_sha256 && *settings_.library_sha256 != loaded.library_sha256) {
        throw violation("it loaded a library whose SHA-256 is not the one expected");
    }
    // The module host made these checks; a module host that did not is broken.
    try {
        check_describe_document(manifest, loaded.describe);
    } catch (const Error& error) {
        throw violation(std::string("its describe() document fails the checks: ") + error.what());
    }
    return loaded;
}

bool ChildModule::has_diagnostics() const noexcept { return diagnostics_; }

bool ChildModule::has_self_test() const noexcept { return self_test_; }

CodeStatus ChildModule::call(FrameKind kind, const char* name, praenora_bytes document,
                             OutputSink* output, OutputSink* error, bool* created) {
    const std::lock_guard<std::mutex> serial(call_mutex_);
    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ == State::crashed) {
        return {PRAENORA_STATUS_INTERNAL, crash_details(name)};
    }
    if (state_ != State::running) {
        lock.unlock();
        write_host_error(error, PRAENORA_STATUS_INVALID_STATE,
                         std::string(name) + " was not called: the module host of module " +
                             module_ + " has ended");
        return {PRAENORA_STATUS_INVALID_STATE, {}};
    }
    if (document.size > max_payload(kind) - kCallFields) {
        lock.unlock();
        write_host_error(error, PRAENORA_STATUS_RESOURCE_EXHAUSTED,
                         std::string(name) + " was not called: its document of " +
                             std::to_string(document.size) +
                             " bytes is larger than a module host can take");
        return {PRAENORA_STATUS_RESOURCE_EXHAUSTED, {}};
    }
    const std::shared_ptr<InstanceHost> host = host_;
    pending_ = Pending{};
    pending_.active = true;
    pending_.id = next_id_++;
    pending_.name = name;
    pending_.output = output;
    pending_.error = error;
    const std::uint64_t id = pending_.id;
    ++sent_;
    lock.unlock();

    const bool cancelled = host != nullptr && host->cancel_requested();
    Frame frame;
    frame.kind = kind;
    frame.id = id;
    frame.payload = call_payload(
        output, error, host != nullptr ? host->log_threshold() : PRAENORA_LOG_CRITICAL + 1,
        cancelled, document);
    // Written in slices, as long as the module host takes it: a call's deadline cannot reach a
    // module whose call has not arrived (its grace's end kills the module host, which ends the
    // write), but a stop can: the module host, which has not begun the call, is ended at once.
    const Channel::WriteStatus written = send_request(
        frame, [&host] { return host == nullptr || !host->call_stopped(); }, std::nullopt, name);
    frame.payload = std::string();
    if (written == Channel::WriteStatus::stopped) {
        stop_process(true);
        lock.lock();
        pending_ = Pending{};
        lock.unlock();
        write_host_error(error, PRAENORA_STATUS_CANCELLED,
                         std::string(name) + " was stopped before the module host of module " +
                             module_ +
                             " had taken it: a stop was requested, and the module "
                             "host was ended");
        return {PRAENORA_STATUS_CANCELLED, {}};
    }

    lock.lock();
    wait_for_answer(lock, host.get(), cancelled);
    CodeStatus status;
    if (pending_.done) {
        status.status = pending_.status;
        if (created != nullptr) {
            *created = pending_.created;
        }
    } else if (state_ == State::crashed) {
        status = {PRAENORA_STATUS_INTERNAL, crash_details(name)};
    } else {
        status.status = PRAENORA_STATUS_TIMEOUT; // terminated (or ended) while it ran
    }
    pending_ = Pending{}; // from now on the reader writes to none of this call's sinks
    return status;
}

Channel::WriteStatus
ChildModule::send_request(const Frame& frame, const std::function<bool()>& go_on,
                          std::optional<std::chrono::steady_clock::time_point> deadline,
                          const char* what) {
    std::string problem;
    const Channel::WriteStatus status = channel_->write_parts_while(
        frame.kind, frame.id, {frame.payload}, go_on, deadline, &problem);
    if (status == Channel::WriteStatus::timed_out) {
        // It stopped reading its channel: nothing more can be asked of it.
        connection_ended(
            "the module host did not take " + std::string(what) + " within " +
            std::to_string(
                std::chrono::duration_cast<std::chrono::seconds>(settings_.start_timeout).count()) +
            " s");
    } else if (status != Channel::WriteStatus::written && status != Channel::WriteStatus::stopped) {
        // Usually the module host is gone and the reader sees the end of the stream as well;
        // whatever the reason, nothing more can be asked of it.
        connection_ended(std::string(), nullptr,
                         "the runtime could not write to the module host's channel: " + problem);
    }
    return status;
}

void ChildModule::wait_for_answer(std::unique_lock<std::mutex>& lock, const InstanceHost* host,
                                  bool cancel_sent, const std::atomic<bool>* stop) {
    const std::uint64_t id = pending_.id;
    std::optional<std::chrono::steady_clock::time_point> gone_since;
    while (!pending_.done && state_ == State::running) {
        if (stop != nullptr && stop->load()) {
            // Nothing in the module host passes the stop on to a load: it is ended.
            lock.unlock();
            stop_process(true);
            lock.lock();
            continue;
        }
        if (!cancel_sent && host != nullptr && host->cancel_requested()) {
            lock.unlock();
            // Within a poll: another write may hold the channel (the reader answering the
            // store), or the module host may take nothing. A cancel frame that could not begin
            // is tried again at the next poll; one cut short leaves nothing to write, and the
            // call's grace ends it.
            const Channel::WriteStatus written = channel_->write_parts_until(
                FrameKind::cancel, id, {},
                deadline_after(std::chrono::steady_clock::now(), kCancelPoll));
            cancel_sent = written != Channel::WriteStatus::timed_out || channel_->broken();
            lock.lock();
            if (cancel_sent) {
                continue;
            }
        }
        if (gone_since) {
            if (std::chrono::steady_clock::now() - *gone_since >= kDrainWait) {
                // Its process has ended and the reader has not seen the end of the channel, which
                // a process it started may keep open: a crash all the same.
                lock.unlock();
                connection_ended(std::string());
                lock.lock();
                if (state_ != State::running) {
                    continue;
                }
                // Another thread is finding out how it ended, and wakes this one when it knows.
            }
        } else {
            lock.unlock();
            const bool gone = process_->wait(std::chrono::nanoseconds::zero()).has_value();
            lock.lock();
            if (gone) {
                gone_since = std::chrono::steady_clock::now();
                continue;
            }
        }
        changed_.wait_for(lock, kCancelPoll);
    }
}

CodeStatus ChildModule::create(const std::shared_ptr<InstanceHost>& host,
                               praenora_bytes create_json, praenora_instance** out_instance,
                               OutputSink& error) {
    *out_instance = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (host_ != nullptr) {
            lock.unlock();
            write_host_error(&error, PRAENORA_STATUS_INVALID_STATE,
                             "create was not called: the module host of module " + module_ +
                                 " serves one instance, and has served one already");
            return {PRAENORA_STATUS_INVALID_STATE, {}};
        }
        host_ = host;
    }
    bool created = false;
    CodeStatus status = call(FrameKind::create, "create", create_json, nullptr, &error, &created);
    *out_instance = created ? child_instance() : nullptr;
    return status;
}

CodeStatus ChildModule::initialize(praenora_instance*, praenora_bytes initialize_json,
                                   OutputSink& error) {
    return call(FrameKind::initialize, "initialize", initialize_json, nullptr, &error, nullptr);
}

CodeStatus ChildModule::invoke(praenora_instance*, praenora_bytes invocation_json,
                               OutputSink& result, OutputSink& error) {
    return call(FrameKind::invoke, "invoke", invocation_json, &result, &error, nullptr);
}

CodeStatus ChildModule::finalize(praenora_instance*, OutputSink& error) {
    return call(FrameKind::finalize, "finalize", praenora_bytes{nullptr, 0}, nullptr, &error,
                nullptr);
}

CodeStatus ChildModule::destroy(praenora_instance*) {
    return call(FrameKind::destroy, "destroy", praenora_bytes{nullptr, 0}, nullptr, nullptr,
                nullptr);
}

CodeStatus ChildModule::diagnostics(praenora_instance*, OutputSink& out) {
    if (!diagnostics_) {
        return {PRAENORA_STATUS_UNSUPPORTED, {}};
    }
    return call(FrameKind::diagnostics, "diagnostics", praenora_bytes{nullptr, 0}, &out, nullptr,
                nullptr);
}

CodeStatus ChildModule::self_test(const praenora_host_api*, OutputSink&) {
    return {PRAENORA_STATUS_UNSUPPORTED, {}};
}

std::optional<std::int64_t> ChildModule::process_id() const noexcept { return process_->pid(); }

std::uint64_t ChildModule::requests_sent() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return sent_;
}

void ChildModule::read_frames() noexcept {
    std::string problem;
    for (;;) {
        Channel::ReadResult read = channel_->read();
        if (read.status != Channel::ReadStatus::frame) {
            problem = read.status == Channel::ReadStatus::broken ? read.problem : std::string();
            break;
        }
        try {
            if (!dispatch(read.frame, problem)) {
                break;
            }
        } catch (...) {
            problem = "a frame from the module host could not be handled";
            break;
        }
    }
    connection_ended(std::move(problem));
}

bool ChildModule::dispatch(Frame& frame, std::string& problem) {
    PayloadReader reader(frame.payload);
    switch (frame.kind) {
    case FrameKind::log: {
        std::int32_t level = 0;
        if (!reader.i32(level)) {
            problem = "a log frame is too short";
            return false;
        }
        const std::string_view message = reader.rest();
        std::shared_ptr<InstanceHost> host;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            host = host_;
        }
        if (host != nullptr) {
            host->log(level, message);
        }
        return true;
    }
    case FrameKind::lookup: {
        std::uint32_t major = 0;
        std::uint8_t provided = 0;
        if (!reader.u32(major) || !reader.u8(provided) || provided > 1) {
            problem = "a lookup frame is malformed";
            return false;
        }
        const std::string_view service = reader.rest();
        std::shared_ptr<InstanceHost> host;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            host = host_;
        }
        if (host != nullptr) {
            host->lookup(service, major, provided == 1);
        }
        return true;
    }
    case FrameKind::provider_log: {
        std::uint32_t provider = 0, size = 0;
        std::int32_t level = 0;
        std::string_view text;
        if (!reader.u32(provider) || !reader.i32(level) || !reader.u32(size) ||
            !reader.bytes(size, text)) {
            problem = "a provider log frame is malformed";
            return false;
        }
        if (provider >= settings_.providers.size()) {
            problem = "a provider log frame names no provider of the module host";
            return false;
        }
        // The record's attributes, in order: [[key, value], ...].
        std::vector<LogAttribute> attributes;
        json pairs;
        try {
            pairs = parse_json_text(text, "the attributes of a provider log record");
        } catch (const Error&) {
            pairs = nullptr;
        }
        if (!pairs.is_array()) {
            problem = "the attributes of a provider log record are not a JSON array";
            return false;
        }
        for (json& pair : pairs) {
            if (!pair.is_array() || pair.size() != 2 || !pair[0].is_string()) {
                problem = "an attribute of a provider log record is not [key, value]";
                return false;
            }
            attributes.push_back(LogAttribute{pair[0].get<std::string>(), std::move(pair[1])});
        }
        const std::shared_ptr<spdlog::logger>& logger = settings_.providers[provider].logger;
        if (logger != nullptr) {
            log_event(*logger, log_level_from_abi(level), reader.rest(), attributes);
        }
        return true;
    }
    case FrameKind::sink: {
        std::uint8_t which = 0;
        if (!reader.u8(which) || which > 1) {
            problem = "a sink frame names no sink";
            return false;
        }
        const std::string_view bytes = reader.rest();
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_.active || pending_.id != frame.id || pending_.done) {
            problem = "a sink frame belongs to no call in flight";
            return false;
        }
        OutputSink* sink = which == 0 ? pending_.output : pending_.error;
        if (sink == nullptr) {
            problem = std::string("a sink frame writes to a sink that ") + pending_.name +
                      " does not have";
            return false;
        }
        praenora_sink* abi = sink->sink();
        if (abi->write(abi->context, bytes.data(), bytes.size()) != PRAENORA_STATUS_OK) {
            problem = "a sink frame writes beyond the sink's limit";
            return false;
        }
        return true;
    }
    case FrameKind::result: {
        std::int32_t status = 0;
        std::uint8_t created = 0;
        if (!reader.i32(status) || !reader.u8(created) || created > 1 || !reader.rest().empty()) {
            problem = "a result frame is malformed";
            return false;
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_.active || pending_.id != frame.id || pending_.done ||
            std::string(pending_.name) == "load") {
            problem = "a result frame belongs to no call in flight";
            return false;
        }
        if (created == 1 && std::string(pending_.name) != "create") {
            problem = "a result frame of " + std::string(pending_.name) + " says it created one";
            return false;
        }
        pending_.done = true;
        pending_.status = status;
        pending_.created = created == 1;
        changed_.notify_all();
        return true;
    }
    case FrameKind::loaded:
    case FrameKind::load_failed: {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_.active || pending_.id != frame.id || pending_.done ||
            std::string(pending_.name) != "load") {
            problem = "a load answer belongs to no load in flight";
            return false;
        }
        pending_.done = true;
        pending_.reply = std::move(frame);
        changed_.notify_all();
        return true;
    }
    case FrameKind::store_load:
    case FrameKind::store_save:
    case FrameKind::store_refused:
        return dispatch_store(frame, problem);
    case FrameKind::hello:
    case FrameKind::load:
    case FrameKind::create:
    case FrameKind::initialize:
    case FrameKind::invoke:
    case FrameKind::finalize:
    case FrameKind::destroy:
    case FrameKind::diagnostics:
    case FrameKind::cancel:
    case FrameKind::exit:
    case FrameKind::store_reply:
    case FrameKind::ready:
        break;
    }
    problem = std::string("the module host sent an unexpected ") +
              frame_kind_name(static_cast<std::uint32_t>(frame.kind)) + " frame";
    return false;
}

const ChildProvider* ChildModule::store_provider(std::uint32_t index, std::uint64_t key_size,
                                                 std::string& problem) const {
    // The channel refuses the store's frames of a module host that does not share at their
    // header already; this check stays for any that came another way.
    if (!settings_.share_compiled_models) {
        problem = "a store frame came from the module host of a step that does not share compiled "
                  "models";
        return nullptr;
    }
    if (index >= settings_.providers.size()) {
        problem = "a store frame names no provider of the module host";
        return nullptr;
    }
    if (key_size > kMaxCompiledModelKeyBytes) {
        problem = "a store frame's key of " + std::to_string(key_size) +
                  " bytes is over the limit of " + std::to_string(kMaxCompiledModelKeyBytes);
        return nullptr;
    }
    return &settings_.providers[index];
}

bool ChildModule::dispatch_store(Frame& frame, std::string& problem) {
    PayloadReader reader(frame.payload);
    std::uint32_t index = 0;
    if (!reader.u32(index)) {
        problem = std::string("a ") + frame_kind_name(static_cast<std::uint32_t>(frame.kind)) +
                  " frame is too short";
        return false;
    }
    const auto describe_key = [](std::string_view key) {
        return "compiled model '" + std::string(key) + "'";
    };
    switch (frame.kind) {
    case FrameKind::store_load: {
        const std::string_view key = reader.rest();
        const ChildProvider* provider = store_provider(index, key.size(), problem);
        if (provider == nullptr) {
            return false;
        }
        const std::string name = provider->manifest->name;
        const std::shared_ptr<const std::string> entry = compiled_model_store().find(
            CompiledModelNamespace{name, provider->library_sha256}, key);
        if (entry != nullptr) {
            log_store("compiled_models.hit", *provider, key,
                      describe_key(key) + " of provider " + name +
                          " was found in the shared store (" + std::to_string(entry->size()) +
                          " bytes)",
                      {{"praenora.size", entry->size()}});
        } else {
            log_store("compiled_models.miss", *provider, key,
                      describe_key(key) + " of provider " + name + " is not in the shared store",
                      {});
        }
        const char found = entry != nullptr ? 1 : 0;
        // Within the wait: this thread must never stop reading for good, and while it writes it
        // holds the channel's writer. A module host that is gone is noticed by the next read; one
        // that does not take the answer breaks the protocol.
        if (channel_->write_parts_until(
                FrameKind::store_reply, frame.id,
                {std::string_view(&found, 1),
                 entry != nullptr ? std::string_view(*entry) : std::string_view()},
                deadline_after(std::chrono::steady_clock::now(), settings_.store_reply_wait)) ==
            Channel::WriteStatus::timed_out) {
            problem = "the module host did not take the answer to its store request within " +
                      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         settings_.store_reply_wait)
                                         .count()) +
                      " ms";
            return false;
        }
        return true;
    }
    case FrameKind::store_save: {
        std::uint32_t key_size = 0;
        std::string_view key_view;
        if (!reader.u32(key_size) || !reader.bytes(key_size, key_view)) {
            problem = "a store_save frame is malformed";
            return false;
        }
        const ChildProvider* provider = store_provider(index, key_size, problem);
        if (provider == nullptr) {
            return false;
        }
        const std::string key(key_view);
        const std::size_t size = reader.rest().size();
        if (size > kMaxCompiledModelBytes) {
            problem = "a store_save frame's entry of " + std::to_string(size) +
                      " bytes is over the limit of " + std::to_string(kMaxCompiledModelBytes);
            return false;
        }
        // The entry is the payload's end: kept without a copy.
        std::string entry = std::move(frame.payload);
        entry.erase(0, entry.size() - size);
        const std::string name = provider->manifest->name;
        const CompiledModelStore::SaveResult saved = compiled_model_store().save(
            CompiledModelNamespace{name, provider->library_sha256}, key, std::move(entry));
        if (saved.status == CompiledModelStore::Saved::saved) {
            log_store("compiled_models.saved", *provider, key,
                      describe_key(key) + " of provider " + name +
                          " was saved in the shared store (" + std::to_string(size) + " bytes" +
                          (saved.evicted > 0 ? "; " + std::to_string(saved.evicted) +
                                                   " least recently used evicted"
                                             : std::string()) +
                          ")",
                      {{"praenora.size", size}, {"praenora.evicted", saved.evicted}});
        } else {
            log_store("compiled_models.refused", *provider, key,
                      describe_key(key) + " of provider " + name +
                          " was not saved in the shared store: its " + std::to_string(size) +
                          " bytes do not fit",
                      {{"praenora.key_size", key.size()}, {"praenora.size", size}});
        }
        return true;
    }
    case FrameKind::store_refused: {
        std::uint64_t key_size = 0, size = 0;
        if (!reader.u64(key_size) || !reader.u64(size)) {
            problem = "a store_refused frame is malformed";
            return false;
        }
        const std::string_view key = reader.rest();
        const ChildProvider* provider = store_provider(index, key.size(), problem);
        if (provider == nullptr) {
            return false;
        }
        const bool long_key = key_size > kMaxCompiledModelKeyBytes;
        const bool large = size > kMaxCompiledModelBytes;
        if (!long_key && !large) {
            problem = "a store_refused frame refuses an entry within the limits";
            return false;
        }
        if (key.size() != std::min<std::uint64_t>(key_size, kMaxCompiledModelKeyBytes)) {
            problem = "a store_refused frame carries a key of another size than it says";
            return false;
        }
        const std::string name = provider->manifest->name;
        log_store("compiled_models.refused", *provider, key,
                  describe_key(key) + " of provider " + name +
                      " was not saved in the shared store: " +
                      (long_key ? "its key of " + std::to_string(key_size) +
                                      " bytes is over the limit of " +
                                      std::to_string(kMaxCompiledModelKeyBytes)
                                : "its " + std::to_string(size) + " bytes are over the limit of " +
                                      std::to_string(kMaxCompiledModelBytes)),
                  {{"praenora.key_size", key_size}, {"praenora.size", size}});
        return true;
    }
    default:
        break;
    }
    problem = "a frame is not a store frame";
    return false;
}

void ChildModule::log_store(
    const char* event, const ChildProvider& provider, std::string_view key,
    const std::string& message,
    std::vector<std::pair<const char*, std::uint64_t>> extra) const noexcept {
    if (settings_.logger == nullptr) {
        return;
    }
    try {
        std::vector<LogAttribute> attributes = {{"praenora.event", event},
                                                {"praenora.module", module_},
                                                {"praenora.pid", process_->pid()},
                                                {"praenora.provider", provider.manifest->name},
                                                {"praenora.key", std::string(key)}};
        for (const auto& [name, value] : extra) {
            attributes.push_back({name, value});
        }
        log_event(*settings_.logger, LogLevel::debug, message, attributes);
    } catch (...) {
        // The record is lost.
    }
}

void ChildModule::connection_ended(std::string problem, const char* phase_hint,
                                   const std::string& still_running) noexcept {
    bool kill = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::running || ending_) {
            changed_.notify_all();
            // Asked to exit, killed, known to have crashed, or being found out so. A module host
            // asked to exit that broke the protocol meanwhile (a frame cut short, an answer it
            // did not take) cannot be asked anything any more: it is ended now, so that the
            // ending does not wait on it.
            kill = state_ == State::exiting && !problem.empty();
            if (!kill) {
                return;
            }
        }
    }
    if (kill) {
        process_->terminate();
        channel_->shut_down();
        // Logged as the other kills are, with the problem: the ending itself would only find the
        // process ended by the kill's signal.
        try {
            log(PRAENORA_LOG_WARN,
                "the module host of module " + module_ + " (pid " +
                    std::to_string(process_->pid()) +
                    ") broke the channel's protocol while it was being ended: " + problem +
                    "; it was terminated",
                "module_host.terminated");
        } catch (...) {
            // Out of memory while building the message; the module host is killed all the same.
        }
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::running || ending_) {
            changed_.notify_all();
            return;
        }
        // The first to see the end says how it ended: a protocol violation found by the thread
        // that reads an answer kills the module host, whose end of the stream the reader then sees,
        // and the kill is not the crash.
        ending_ = true;
    }
    std::optional<ChildExit> exit;
    try {
        if (problem.empty()) {
            exit = process_->wait(kEndGrace);
            if (!exit) {
                problem = still_running;
            }
        }
        if (!problem.empty()) {
            process_->terminate();
            exit = process_->wait();
        }
    } catch (...) {
        // Out of memory: what ended it stays unknown.
    }
    channel_->shut_down();
    const std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::running) {
        changed_.notify_all();
        return; // terminate() or end() came first
    }
    state_ = State::crashed;
    crash_ = Crash{exit, problem};
    // Logged before the call waiting for the module host learns of the crash, so that the record
    // is there when the call returns. The logger never calls back into this object.
    const char* phase = pending_.active ? pending_.name : phase_hint;
    try {
        std::string message =
            "the module host of module " + module_ + " (pid " + std::to_string(process_->pid()) +
            ") crashed " +
            (phase != nullptr ? std::string("during ") + phase : std::string("between calls")) +
            ": it ";
        message += problem.empty() ? (exit ? describe(*exit) : std::string("ended"))
                                   : "broke the channel's protocol: " + problem;
        log(PRAENORA_LOG_ERROR, message, "module_host.crashed");
    } catch (...) {
        // Out of memory while building the message; the state says it.
    }
    changed_.notify_all();
}

std::string ChildModule::crash_details(const char* phase) const {
    json details = {{"phase", phase}};
    if (!crash_) {
        details["reason"] = "protocol";
    } else if (!crash_->problem.empty()) {
        details["reason"] = "protocol";
    } else if (crash_->exit && crash_->exit->signal) {
        details["signal"] = *crash_->exit->signal;
        details["signal_name"] = signal_name(*crash_->exit->signal);
    } else if (crash_->exit && crash_->exit->exit_code) {
        details["exit_code"] = *crash_->exit->exit_code;
    } else {
        details["reason"] = "ended";
    }
    return details.dump();
}

void ChildModule::stop_process(bool announce) noexcept {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::running) {
            return;
        }
        state_ = State::terminated;
    }
    process_->terminate();
    try {
        static_cast<void>(process_->wait());
    } catch (...) {
    }
    channel_->shut_down();
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        changed_.notify_all();
    }
    if (!announce) {
        return;
    }
    try {
        log(PRAENORA_LOG_WARN,
            "the module host of module " + module_ + " (pid " + std::to_string(process_->pid()) +
                ") was terminated",
            "module_host.terminated");
    } catch (...) {
    }
}

void ChildModule::terminate() noexcept { stop_process(true); }

bool ChildModule::terminated() const noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::terminated;
}

void ChildModule::end(std::chrono::nanoseconds wait) noexcept {
    bool ask = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::running) {
            state_ = State::exiting;
            ask = true;
        }
    }
    try {
        std::optional<ChildExit> exit;
        bool killed = false;
        bool asked = false;
        if (ask) {
            // The request to exit is written within the wait, as the wait for the exit is: a
            // module host that takes neither is killed when the wait has passed. A wait without
            // limit (the longest duration) saturates at the clock's last instant: no limit.
            const auto until = deadline_after(std::chrono::steady_clock::now(), wait);
            const Channel::WriteStatus written =
                channel_->write_parts_until(FrameKind::exit, 0, {}, until);
            asked = written == Channel::WriteStatus::written;
            if (asked || written == Channel::WriteStatus::failed) {
                // Not written because the module host is gone: its exit is there to be taken.
                const auto left = until - std::chrono::steady_clock::now();
                exit = process_->wait(
                    std::max<std::chrono::nanoseconds>(left, std::chrono::nanoseconds::zero()));
            }
            if (!exit) {
                process_->terminate();
                killed = true;
            }
        }
        exit = process_->wait();
        channel_->shut_down();
        if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id()) {
            reader_.join();
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            changed_.notify_all();
        }
        if (killed) {
            log(PRAENORA_LOG_WARN,
                "the module host of module " + module_ + " (pid " +
                    std::to_string(process_->pid()) + ") did not " +
                    (asked ? "exit" : "take the request to exit") + " within " +
                    std::to_string(
                        std::chrono::duration_cast<std::chrono::milliseconds>(wait).count()) +
                    " ms of being asked to; it was terminated",
                "module_host.terminated");
        } else if (ask) {
            log(PRAENORA_LOG_DEBUG,
                "the module host of module " + module_ + " (pid " +
                    std::to_string(process_->pid()) + ") " +
                    (exit ? describe(*exit) : std::string("ended")),
                "module_host.exited");
        }
    } catch (...) {
        // Out of memory while ending it: the process and the reader are dealt with by now, or
        // by the destructors of process_ and reader_ (std::thread would end the process).
    }
}

void ChildModule::log(int level, const std::string& message, const char* event) const noexcept {
    if (settings_.logger == nullptr) {
        return;
    }
    try {
        log_event(*settings_.logger, log_level_from_abi(level), message,
                  {{"praenora.event", event},
                   {"praenora.module", module_},
                   {"praenora.pid", process_->pid()}});
    } catch (...) {
        // The message is lost.
    }
}

} // namespace praenora::detail
