#include "isolation/module_host_server.hpp"

#include "core/paths.hpp"
#include "discovery/manifest.hpp"
#include "instance/services.hpp"
#include "isolation/channel.hpp"
#include "isolation/compiled_model_store.hpp"
#include "isolation/sandbox.hpp"
#include "loader/loaded_module.hpp"
#include "loader/output_sink.hpp"
#include "observability/logging.hpp"
#include "platform/platform.hpp"
#include "provider/provider_loader.hpp"
#include "provider/provider_manifest.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/version.hpp>
#include <praenora/sdk/abi.h>
#include <praenora/sdk/status.h>

#include <nlohmann/json.hpp>
#include <spdlog/sinks/base_sink.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <dirent.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace praenora::detail {

namespace {

using nlohmann::json;

constexpr int kChannelDescriptor = 3;

// Exit codes of the module host when it ends itself: the runtime asked it to or is gone (0), or
// the runtime broke the channel's protocol (3).
constexpr int kExitDone = 0;
constexpr int kExitBroken = 3;

// The answer to one store request of a provider, which its thread waits for.
struct StoreAnswer {
    bool done = false;
    bool found = false;
    std::string entry;
};

class StoreClient;

// The module host's state. The module's threads may use the host API until the process ends, so
// it is never destroyed.
struct Server {
    Channel channel{kChannelDescriptor};
    praenora_host_api api{};
    // The services offered here: those of the providers the load started, which the module's
    // manifest declares. Filled by the load, before the module's code runs; read-only after it.
    ServiceRegistry services;
    // The logs of each provider that records anything, whose events go to the runtime.
    std::vector<std::unique_ptr<LogHub>> provider_logs;
    std::atomic<std::uint64_t> call{0};      // the call in flight, 0 between calls
    std::atomic<std::uint64_t> cancelled{0}; // the latest call a cancel frame named
    std::atomic<std::int32_t> threshold{PRAENORA_LOG_CRITICAL + 1};
    std::mutex mutex;
    std::condition_variable arrived;
    std::deque<Frame> requests;            // guarded by mutex
    std::shared_ptr<LoadedModule> module;  // the call thread's
    praenora_instance* instance = nullptr; // the call thread's
    // The shared compiled models: whether the load shares them (set before any provider starts),
    // and the providers' access to them, by index (never destroyed: providers keep their hosts).
    bool share = false;
    std::vector<std::unique_ptr<StoreClient>> store_clients;
    // The store requests waiting for their answers, by id; guarded by store_mutex.
    std::atomic<std::uint64_t> next_store_request{1};
    std::mutex store_mutex;
    std::condition_variable store_answered;
    std::map<std::uint64_t, StoreAnswer*> store_waiting;
};

Server& server() {
    static Server* const state = new Server;
    return *state;
}

bool send(FrameKind kind, std::uint64_t id, std::string payload) {
    Frame frame;
    frame.kind = kind;
    frame.id = id;
    frame.payload = std::move(payload);
    return server().channel.write(frame);
}

std::int32_t clamped(std::int32_t level) noexcept {
    return std::clamp<std::int32_t>(level, PRAENORA_LOG_TRACE, PRAENORA_LOG_CRITICAL);
}

void mark_cancelled(std::uint64_t call) noexcept {
    std::uint64_t seen = server().cancelled.load();
    while (seen < call && !server().cancelled.compare_exchange_weak(seen, call)) {
    }
}

// The host API the module is given. Thread-safe; never throws into module code.

void PRAENORA_CALL host_log(void*, std::int32_t level, const char* message,
                            std::size_t size) noexcept {
    if ((message == nullptr && size != 0) || clamped(level) < server().threshold.load()) {
        return;
    }
    try {
        const std::string_view text =
            message == nullptr ? std::string_view()
                               : std::string_view(message, std::min(size, kMaxLogMessageBytes));
        static_cast<void>(send(FrameKind::log, server().call.load(),
                               PayloadWriter().i32(level).bytes(text).take()));
    } catch (...) {
        // The message is lost.
    }
}

std::int32_t PRAENORA_CALL host_log_enabled(void*, std::int32_t level) noexcept {
    return clamped(level) >= server().threshold.load() ? 1 : 0;
}

std::int32_t PRAENORA_CALL host_cancel_requested(void*) noexcept {
    const std::uint64_t call = server().call.load();
    return call != 0 && server().cancelled.load() == call ? 1 : 0;
}

const void* PRAENORA_CALL host_get_service(void*, const char* name, std::size_t name_size,
                                           std::uint32_t major) noexcept {
    if (name == nullptr) {
        return nullptr;
    }
    const std::string_view service(name, name_size);
    const void* table = server().services.find(service, major);
    try {
        static_cast<void>(
            send(FrameKind::lookup, server().call.load(),
                 PayloadWriter()
                     .u32(major)
                     .u8(table != nullptr ? 1 : 0)
                     .bytes(service.substr(0, std::min(service.size(), kMaxServiceNameBytes)))
                     .take()));
    } catch (...) {
        // The lookup goes unreported.
    }
    return table;
}

// What the load_compiled and save_compiled callbacks of the provider `index` of the load reach:
// the store in the runtime's process, through the channel. A load waits for its answer, which the
// reader thread hands over (answer_store()); a save is sent and not answered. An entry or a key
// over the store's limits is refused here: only a notice of it is sent, never its bytes, and a
// load under a key over the limit finds nothing without asking.
class StoreClient final : public CompiledModelAccess {
public:
    explicit StoreClient(std::uint32_t index) : index_(index) {}

    bool load(std::string_view key, praenora_sink& out) override {
        if (key.size() > kMaxCompiledModelKeyBytes) {
            return false; // nothing is ever kept under it
        }
        Server& state = server();
        StoreAnswer answer;
        const std::uint64_t id = state.next_store_request.fetch_add(1);
        {
            const std::lock_guard<std::mutex> lock(state.store_mutex);
            state.store_waiting.emplace(id, &answer);
        }
        const std::string fields = PayloadWriter().u32(index_).take();
        const bool sent = state.channel.write_parts(FrameKind::store_load, id, {fields, key});
        {
            std::unique_lock<std::mutex> lock(state.store_mutex);
            // Without the request sent the runtime is gone, and the reader ends the process.
            state.store_answered.wait(lock, [&answer, sent] { return answer.done || !sent; });
            state.store_waiting.erase(id);
        }
        return answer.found && out.write(out.context, answer.entry.data(), answer.entry.size()) ==
                                   PRAENORA_STATUS_OK;
    }

    void save(std::string_view key, std::string_view data) override {
        Server& state = server();
        const std::uint64_t id = state.next_store_request.fetch_add(1);
        if (key.size() > kMaxCompiledModelKeyBytes || data.size() > kMaxCompiledModelBytes) {
            const std::string fields =
                PayloadWriter().u32(index_).u64(key.size()).u64(data.size()).take();
            static_cast<void>(state.channel.write_parts(
                FrameKind::store_refused, id,
                {fields, key.substr(0, std::min(key.size(), kMaxCompiledModelKeyBytes))}));
            return;
        }
        const std::string fields =
            PayloadWriter().u32(index_).u32(static_cast<std::uint32_t>(key.size())).take();
        static_cast<void>(
            state.channel.write_parts(FrameKind::store_save, id, {fields, key, data}));
    }

private:
    std::uint32_t index_;
};

// Hands the runtime's answer to a store request (a store_reply frame) to the provider's thread
// that waits for it; false for an answer that is malformed or that no request waits for.
bool answer_store(Frame& frame) {
    Server& state = server();
    if (frame.payload.empty() || static_cast<unsigned char>(frame.payload[0]) > 1) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(state.store_mutex);
    const auto waiting = state.store_waiting.find(frame.id);
    if (waiting == state.store_waiting.end() || waiting->second->done) {
        return false;
    }
    StoreAnswer& answer = *waiting->second;
    answer.found = frame.payload[0] == 1;
    answer.entry = std::move(frame.payload);
    answer.entry.erase(0, 1);
    answer.done = true;
    state.store_answered.notify_all();
    return true;
}

// Passes what a sink accepts on to the runtime, in chunks.
OutputSink::Forward forward_to(std::uint8_t sink, std::uint64_t call) {
    return [sink, call](const char* data, std::size_t size) {
        try {
            while (size > 0) {
                const std::size_t chunk = std::min(size, kMaxSinkChunkBytes);
                if (!send(FrameKind::sink, call,
                          PayloadWriter().u8(sink).bytes(std::string_view(data, chunk)).take())) {
                    return false;
                }
                data += chunk;
                size -= chunk;
            }
            return true;
        } catch (...) {
            return false;
        }
    };
}

// The PRAENORA_LOG_* level of a level of spdlog's.
std::int32_t abi_level(spdlog::level::level_enum level) noexcept {
    switch (level) {
    case spdlog::level::trace:
        return PRAENORA_LOG_TRACE;
    case spdlog::level::debug:
        return PRAENORA_LOG_DEBUG;
    case spdlog::level::info:
        return PRAENORA_LOG_INFO;
    case spdlog::level::warn:
        return PRAENORA_LOG_WARN;
    case spdlog::level::err:
        return PRAENORA_LOG_ERROR;
    default:
        return PRAENORA_LOG_CRITICAL;
    }
}

// Passes each event of one provider's logs on to the runtime, which logs it on its logger
// provider.<name> of the run: its level, its attributes and its message, as the provider's host
// logged them (provider_loader.cpp).
class ProviderLogSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit ProviderLogSink(std::uint32_t provider) : provider_(provider) {}

protected:
    void sink_it_(const spdlog::details::log_msg& message) override {
        try {
            const LoggedEvent event = decode_logged_event(
                std::string_view(message.payload.data(), message.payload.size()));
            json pairs = json::array();
            for (const LogAttribute& attribute : event.attributes) {
                pairs.push_back(json::array({attribute.key, attribute.value}));
            }
            std::string attributes = pairs.dump(-1, ' ', false, json::error_handler_t::replace);
            if (attributes.size() > kMaxLogAttributesBytes) {
                attributes = "[]"; // no attributes rather than some
            }
            const std::string_view text(event.message.data(),
                                        std::min(event.message.size(), kMaxLogMessageBytes));
            static_cast<void>(send(FrameKind::provider_log, server().call.load(),
                                   PayloadWriter()
                                       .u32(provider_)
                                       .i32(abi_level(message.level))
                                       .u32(static_cast<std::uint32_t>(attributes.size()))
                                       .bytes(attributes)
                                       .bytes(text)
                                       .take()));
        } catch (...) {
            // The record is lost.
        }
    }
    void flush_() override {}

private:
    std::uint32_t provider_;
};

// Loads and starts the provider `index` of a load request, as the runtime's process loads one
// (load_provider()), and adds the tables of its services to the module's: errors are those of the
// provider loader, located in the provider's manifest.
void load_provider_of(const json& request, std::uint32_t index) {
    Server& state = server();
    const ProviderManifest manifest =
        load_provider_manifest(path_from_utf8(request.at("manifest").get<std::string>()));
    const std::string read = request.at("manifest_sha256").get<std::string>();
    const std::string library = request.at("library").get<std::string>();
    const auto changed = [&manifest](const std::string& what) {
        SourceLocation location;
        location.file = path_to_utf8(manifest.manifest_path);
        return Error(ErrorCode::MANIFEST_INVALID,
                     "provider " + manifest.name + "@" + to_string(manifest.version) +
                         ": the manifest changed after the runtime read it: " + what,
                     std::move(location));
    };
    if (manifest.manifest_sha256 != read) {
        throw changed("its SHA-256 is now " + manifest.manifest_sha256 + ", it was " + read);
    }
    if (path_to_utf8(manifest.library_path) != library) {
        throw changed("it names the library '" + path_to_utf8(manifest.library_path) +
                      "', it named '" + library + "'");
    }
    std::vector<ServiceVersion> services;
    for (const json& service : request.at("services")) {
        services.push_back({service.at("name").get<std::string>(),
                            service.at("major").get<std::uint32_t>(),
                            service.at("minor").get<std::uint32_t>()});
    }
    // The provider's records, at the levels the runtime records them, go to the runtime; with
    // none, it is given no logs and records nothing, as a provider without logs in any process.
    LogHub* logs = nullptr;
    const std::int32_t threshold = request.at("log_level").get<std::int32_t>();
    if (threshold <= PRAENORA_LOG_CRITICAL) {
        LogOptions options;
        options.level = log_level_from_abi(threshold);
        options.to_stderr = false;
        state.provider_logs.push_back(std::make_unique<LogHub>(options));
        logs = state.provider_logs.back().get();
        logs->add_sink(std::make_shared<ProviderLogSink>(index));
    }
    // The shared compiled models, only for a step that shares them.
    CompiledModelAccess* compiled = nullptr;
    if (state.share) {
        state.store_clients.push_back(std::make_unique<StoreClient>(index));
        compiled = state.store_clients.back().get();
    }
    const LoadedProvider& provider = load_provider(
        manifest, services, request.at("library_sha256").get<std::string>(), logs, compiled);
    for (const ServiceVersion& service : services) {
        state.services.add(service.name, service.major,
                           provider.service(service.name, service.major));
    }
}

// An error document of the module host, written as a module would write its own.
void write_error(OutputSink& error, praenora_status code, const std::string& message) {
    const std::string text =
        json{{"code", code}, {"message", message}, {"details", json::object()}}.dump();
    praenora_sink* sink = error.sink();
    static_cast<void>(sink->write(sink->context, text.data(), text.size()));
}

std::string dumped(const json& value) {
    // Messages may hold bytes that are not UTF-8: replaced, not refused.
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Applies the restricted profile, with `directory` (the module host's temporary directory, which
// the runtime made) as the only place it may write to, besides /dev/null; TMPDIR names it from now
// on, so std::filesystem::temp_directory_path() does too. Throws MODULE_HOST_UNAVAILABLE, details
// {"reason": "sandbox", "system"}, located in the module's manifest, when it cannot be applied.
// While it lives, this process's standard error goes to /dev/null: macOS's sandbox library writes
// a line of its own there when sandbox_init() fails ("sandbox initialization failed: ..."), which
// would reach the runtime's standard error, while the load's error says the same. It lives only
// around that call, when nothing else of this process writes there: the reader thread never does,
// and no library of a provider or of the module is loaded yet. Descriptor 2 is restored as it was.
class QuietStandardError {
public:
    QuietStandardError() noexcept {
        static_cast<void>(std::fflush(stderr));
        saved_ = ::fcntl(2, F_DUPFD_CLOEXEC, 0);
        const int null = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (saved_ >= 0 && null >= 0) {
            quiet_ = ::dup2(null, 2) == 2;
        }
        if (null >= 0) {
            static_cast<void>(::close(null));
        }
    }
    ~QuietStandardError() {
        if (quiet_) {
            static_cast<void>(::dup2(saved_, 2));
        }
        if (saved_ >= 0) {
            static_cast<void>(::close(saved_));
        }
    }
    QuietStandardError(const QuietStandardError&) = delete;
    QuietStandardError& operator=(const QuietStandardError&) = delete;

private:
    int saved_ = -1;
    bool quiet_ = false;
};

void apply_sandbox(const ModuleManifest& manifest, const std::string& directory) {
    const auto unavailable = [&manifest](const std::string& what, const std::string& system) {
        SourceLocation location;
        location.file = path_to_utf8(manifest.manifest_path);
        return Error(ErrorCode::MODULE_HOST_UNAVAILABLE,
                     "module " + manifest.name + "@" + to_string(manifest.version) +
                         ": the module host cannot " + what + ": " + system,
                     std::move(location), json{{"reason", "sandbox"}, {"system", system}}.dump());
    };
    // Before any library's code runs, only this thread, the reader thread, which reads no
    // environment variable, and the threads that wait for this one exist.
    if (::setenv("TMPDIR", directory.c_str(), 1) != 0) {
        const int error = errno;
        throw unavailable("set its temporary directory", std::strerror(error));
    }
    std::optional<std::string> problem;
    {
        const QuietStandardError quiet;
        problem = apply_restricted_profile(directory);
    }
    if (problem) {
        throw unavailable("apply the restricted sandbox profile", *problem);
    }
}

void serve_load(const Frame& frame) {
    Server& state = server();
    FrameKind kind = FrameKind::load_failed;
    json answer;
    // The provider being loaded, whose manifest an error is located in.
    std::optional<std::uint32_t> provider;
    try {
        const json request = json::parse(frame.payload);
        if (state.module != nullptr) {
            throw Error(ErrorCode::INVALID_STATE, "the module host has loaded a module already");
        }
        const ModuleManifest manifest =
            load_manifest(path_from_utf8(request.at("manifest").get<std::string>()));
        const std::string read = request.at("manifest_sha256").get<std::string>();
        if (manifest.manifest_sha256 != read) {
            SourceLocation location;
            location.file = path_to_utf8(manifest.manifest_path);
            throw Error(ErrorCode::MANIFEST_INVALID,
                        "module " + manifest.name + "@" + to_string(manifest.version) +
                            ": the manifest changed after the runtime read it: its SHA-256 is "
                            "now " +
                            manifest.manifest_sha256 + ", it was " + read,
                        std::move(location));
        }
        // The sandbox profile first, before any provider or module library is loaded, so that
        // their static initializers run under it too.
        if (request.at("sandbox").get<std::string>() == "restricted") {
            apply_sandbox(manifest, request.at("temporary_directory").get<std::string>());
        }
        std::optional<std::string> expected;
        if (request.at("library_sha256").is_string()) {
            expected = request.at("library_sha256").get<std::string>();
        }
        state.share = request.at("share_compiled_models").get<bool>();
        // The providers of the module's services first, as a run loads them before any module.
        const json& providers = request.at("providers");
        for (std::uint32_t i = 0; i < providers.size(); ++i) {
            provider = i;
            load_provider_of(providers.at(i), i);
        }
        provider.reset();
        std::shared_ptr<LoadedModule> module = LoadedModule::load(manifest, nullptr, expected);
        answer = {{"describe", module->describe()},
                  {"library_sha256", module->library_sha256()},
                  {"diagnostics", module->api().diagnostics != nullptr},
                  {"self_test", module->api().self_test != nullptr}};
        kind = FrameKind::loaded;
        state.module = std::move(module);
    } catch (const Error& error) {
        const SourceLocation& location = error.location();
        answer = {{"code", static_cast<std::int32_t>(error.code())},
                  {"message", error.what()},
                  {"pointer", location.pointer},
                  {"line", location.line},
                  {"column", location.column},
                  {"details", error.details_json()},
                  {"provider", provider ? json(*provider) : json(nullptr)}};
    } catch (const std::exception& error) {
        answer = {
            {"code", static_cast<std::int32_t>(ErrorCode::INTERNAL)},
            {"message", std::string("the module host could not load the module: ") + error.what()},
            {"pointer", ""},
            {"line", 0},
            {"column", 0},
            {"details", "{}"},
            {"provider", nullptr}};
    }
    std::string payload = dumped(answer);
    if (payload.size() > max_payload(kind)) {
        // Too large to pass on whole: an error, shortened, without its details.
        const bool failed = kind == FrameKind::load_failed;
        const json shortened = {
            {"code",
             failed ? answer.at("code") : json(static_cast<std::int32_t>(ErrorCode::INTERNAL))},
            {"message", failed ? answer.at("message").get<std::string>().substr(0, 65536)
                               : std::string("the module's describe() document is too large to "
                                             "pass to the runtime")},
            {"pointer", failed ? answer.at("pointer") : json("")},
            {"line", failed ? answer.at("line") : json(0)},
            {"column", failed ? answer.at("column") : json(0)},
            {"details", "{}"},
            {"provider", failed ? answer.at("provider") : json(nullptr)}};
        kind = FrameKind::load_failed;
        payload = dumped(shortened);
    }
    if (!send(kind, frame.id, std::move(payload))) {
        std::_Exit(kExitDone); // the runtime is gone
    }
}

void serve_call(const Frame& frame) {
    Server& state = server();
    PayloadReader reader(frame.payload);
    std::uint64_t output_limit = 0, error_limit = 0;
    std::int32_t threshold = 0;
    std::uint8_t cancelled = 0;
    if (!reader.u64(output_limit) || !reader.u64(error_limit) || !reader.i32(threshold) ||
        !reader.u8(cancelled)) {
        std::_Exit(kExitBroken);
    }
    const std::string_view document = reader.rest();
    state.threshold.store(threshold);
    state.call.store(frame.id);
    if (cancelled != 0) {
        mark_cancelled(frame.id);
    }
    OutputSink output(static_cast<std::size_t>(output_limit), forward_to(0, frame.id));
    OutputSink error(static_cast<std::size_t>(error_limit), forward_to(1, frame.id));
    const praenora_bytes input{document.data(), document.size()};
    const praenora_module_api_v0* api = state.module != nullptr ? &state.module->api() : nullptr;
    praenora_status status = PRAENORA_STATUS_OK;
    bool created = false;
    const auto refuse = [&status, &error](const std::string& message) {
        status = PRAENORA_STATUS_INVALID_STATE;
        write_error(error, status, message);
    };
    if (api == nullptr) {
        refuse("the module host has no module loaded");
    } else if (frame.kind == FrameKind::create) {
        if (state.instance != nullptr) {
            refuse("the module host serves one instance, and it exists already");
        } else {
            praenora_instance* instance = nullptr;
            status = api->create(&state.api, input, &instance, error.sink());
            created = instance != nullptr;
            if (status == PRAENORA_STATUS_OK) {
                state.instance = instance;
            }
        }
    } else if (state.instance == nullptr) {
        if (frame.kind != FrameKind::destroy) {
            refuse("the module host has no instance");
        }
    } else {
        switch (frame.kind) {
        case FrameKind::initialize:
            status = api->initialize(state.instance, input, error.sink());
            break;
        case FrameKind::invoke:
            status = api->invoke(state.instance, input, output.sink(), error.sink());
            break;
        case FrameKind::finalize:
            status = api->finalize(state.instance, error.sink());
            break;
        case FrameKind::destroy:
            api->destroy(state.instance);
            state.instance = nullptr;
            break;
        case FrameKind::diagnostics:
            status = api->diagnostics != nullptr ? api->diagnostics(state.instance, output.sink())
                                                 : PRAENORA_STATUS_UNSUPPORTED;
            break;
        default:
            std::_Exit(kExitBroken);
        }
    }
    state.call.store(0);
    if (!send(FrameKind::result, frame.id,
              PayloadWriter().i32(status).u8(created ? 1 : 0).take())) {
        std::_Exit(kExitDone); // the runtime is gone
    }
}

// The reader thread: cancel and exit at once, loads and calls for the call thread.
void read_requests() {
    Server& state = server();
    for (;;) {
        Channel::ReadResult read = state.channel.read();
        if (read.status != Channel::ReadStatus::frame) {
            // The runtime is gone, or its end of the channel broke: nothing is left to serve.
            std::_Exit(read.status == Channel::ReadStatus::end ? kExitDone : kExitBroken);
        }
        switch (read.frame.kind) {
        case FrameKind::exit:
            std::fflush(nullptr);
            std::_Exit(kExitDone);
        case FrameKind::cancel:
            mark_cancelled(read.frame.id);
            break;
        case FrameKind::store_reply:
            if (!answer_store(read.frame)) {
                std::_Exit(kExitBroken);
            }
            break;
        case FrameKind::load:
        case FrameKind::create:
        case FrameKind::initialize:
        case FrameKind::invoke:
        case FrameKind::finalize:
        case FrameKind::destroy:
        case FrameKind::diagnostics: {
            {
                const std::lock_guard<std::mutex> lock(state.mutex);
                state.requests.push_back(std::move(read.frame));
            }
            state.arrived.notify_one();
            break;
        }
        default:
            std::_Exit(kExitBroken);
        }
    }
}

// The thread that serves a load (`frame`, a const Frame*). An exception that escapes ends the
// process, as one that escapes the call thread does.
void* serve_load_thread(void* frame) noexcept {
    set_current_thread_name("praenora:load");
    serve_load(*static_cast<const Frame*>(frame));
    return nullptr;
}

// Serves a load (static initializers, describe(), the providers' start()) on a thread with a
// main thread's stack (module_thread_stack_size(), platform/platform.hpp), as a load in the
// runtime's process runs on its caller's thread (the main thread of the tool and of Python), and
// waits for it; on this thread when no such thread can be made.
void serve_load_on_a_main_stack(const Frame& frame) {
    pthread_attr_t attributes;
    if (::pthread_attr_init(&attributes) == 0) {
        pthread_t thread{};
        const bool made =
            ::pthread_attr_setstacksize(&attributes, module_thread_stack_size()) == 0 &&
            ::pthread_create(&thread, &attributes, &serve_load_thread,
                             const_cast<void*>(static_cast<const void*>(&frame))) == 0;
        static_cast<void>(::pthread_attr_destroy(&attributes));
        if (made) {
            static_cast<void>(::pthread_join(thread, nullptr));
            return;
        }
    }
    serve_load(frame);
}

// The call thread: loads and calls, one at a time, in the order they came; a load on a thread of
// its own (serve_load_on_a_main_stack()). Both have a main thread's stack.
void serve_requests() {
    set_current_thread_name("praenora:module");
    Server& state = server();
    for (;;) {
        Frame request;
        {
            std::unique_lock<std::mutex> lock(state.mutex);
            state.arrived.wait(lock, [&state] { return !state.requests.empty(); });
            request = std::move(state.requests.front());
            state.requests.pop_front();
        }
        if (request.kind == FrameKind::load) {
            serve_load_on_a_main_stack(request);
        } else {
            serve_call(request);
        }
    }
}

// A module host is a background process group of the runtime's terminal, when it has one: a write
// to the terminal under `stty tostop`, a change of its settings (tcsetattr()) or a read from it
// would stop this process (SIGTTOU, SIGTTIN), and with it the call, which the same module makes
// unhindered in the runtime's process. Both are ignored, in this process only (the runtime's own
// dispositions are not touched): such a write or change then goes on, and a read fails with EIO
// (standard input is /dev/null anyway). Programs the module starts inherit the ignored signals.
void ignore_terminal_stops() noexcept {
    struct sigaction ignored{};
    ignored.sa_handler = SIG_IGN;
    static_cast<void>(sigemptyset(&ignored.sa_mask));
    static_cast<void>(::sigaction(SIGTTOU, &ignored, nullptr));
    static_cast<void>(::sigaction(SIGTTIN, &ignored, nullptr));
}

#if !defined(__APPLE__)
// Closes every descriptor above the channel's, which the runtime did not pass on on purpose.
void close_descriptors_above_channel() {
#if defined(__linux__) && defined(SYS_close_range)
    if (::syscall(SYS_close_range, static_cast<unsigned>(kChannelDescriptor + 1), ~0U, 0U) == 0) {
        return;
    }
#endif
#if defined(__linux__)
    if (DIR* directory = ::opendir("/proc/self/fd")) {
        std::vector<int> open;
        while (const dirent* entry = ::readdir(directory)) {
            const std::string name = entry->d_name;
            if (!name.empty() && std::all_of(name.begin(), name.end(),
                                             [](char c) { return c >= '0' && c <= '9'; })) {
                const int descriptor = std::atoi(name.c_str());
                if (descriptor > kChannelDescriptor && descriptor != ::dirfd(directory)) {
                    open.push_back(descriptor);
                }
            }
        }
        ::closedir(directory);
        for (const int descriptor : open) {
            static_cast<void>(::close(descriptor));
        }
        return;
    }
#endif
    long limit = ::sysconf(_SC_OPEN_MAX);
    if (limit < 0 || limit > 65536) {
        limit = 65536;
    }
    for (int descriptor = kChannelDescriptor + 1; descriptor < limit; ++descriptor) {
        static_cast<void>(::close(descriptor));
    }
}
#endif

int usage() {
    std::fputs("praenora-module-host: the Praenora runtime starts this program to run a module in "
               "a process of its own; it is not meant to be started by hand\n",
               stderr);
    return 2;
}

} // namespace

int run_module_host(int argc, char** argv) {
    ignore_terminal_stops();
    if (argc != 2 || argv[1][0] == '\0' ||
        !std::all_of(argv[1], argv[1] + std::char_traits<char>::length(argv[1]),
                     [](char c) { return c >= '0' && c <= '9'; })) {
        return usage();
    }
#if defined(__linux__)
    // Killed with the runtime; and if the runtime is gone already, nothing is left to serve.
    const long long parent = std::atoll(argv[1]);
    static_cast<void>(::prctl(PR_SET_PDEATHSIG, SIGKILL));
    if (static_cast<long long>(::getppid()) != parent) {
        std::_Exit(kExitDone);
    }
#endif
#if !defined(__APPLE__)
    close_descriptors_above_channel();
#endif
    struct stat channel_status{};
    if (::fstat(kChannelDescriptor, &channel_status) != 0 || !S_ISSOCK(channel_status.st_mode)) {
        return usage();
    }
    // The spawn passed the channel on without close-on-exec; no program the module runs may
    // inherit it (it could write into it, or hold it open after this process ends).
    if (::fcntl(kChannelDescriptor, F_SETFD, FD_CLOEXEC) != 0) {
        return usage();
    }
    protect_from_sigpipe(kChannelDescriptor);

    Server& state = server();
    state.api.struct_size = static_cast<std::uint32_t>(sizeof(praenora_host_api));
    state.api.abi_major = PRAENORA_ABI_MAJOR;
    state.api.abi_minor = PRAENORA_ABI_MINOR;
    state.api.context = &state;
    state.api.log = &host_log;
    state.api.log_enabled = &host_log_enabled;
    state.api.cancel_requested = &host_cancel_requested;
    state.api.get_service = &host_get_service;

    // The handshake: the runtime's protocol and version, then this build's.
    const Channel::ReadResult hello = state.channel.read();
    if (hello.status != Channel::ReadStatus::frame || hello.frame.kind != FrameKind::hello) {
        std::_Exit(kExitBroken);
    }
    PayloadReader reader(hello.frame.payload);
    std::uint32_t protocol = 0;
    static_cast<void>(reader.u32(protocol));
    const std::string runtime_version(reader.rest());
    const std::string own_version = version_info().runtime;
    if (!send(FrameKind::ready, 0,
              PayloadWriter().u32(kChannelProtocol).bytes(own_version).take()) ||
        protocol != kChannelProtocol || runtime_version != own_version) {
        // A runtime of another version or protocol refuses this module host; nothing to serve.
        std::_Exit(kExitDone);
    }

    std::thread(&read_requests).detach();
    // The module's calls run on a thread of their own, made as the runtime makes an instance's
    // worker (instance/worker.cpp: a ModuleThread, with a main thread's stack), so that a module
    // has the thread and the stack it has in the runtime's process; its load runs on another
    // thread with a main thread's stack, as it runs on its caller's thread there.
    ModuleThread calls(&serve_requests);
    calls.join(); // never returns: the process ends from the reader thread
    return kExitDone;
}

} // namespace praenora::detail
