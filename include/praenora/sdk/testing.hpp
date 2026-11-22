// Praenora module SDK: an in-process test kit for modules.
//
// FakeHost implements the host API: it records log messages, lets a test raise and clear the
// cancel flag (a real host scopes it to one call; here the test decides), serves registered
// service tables, can pretend to be an older host (a smaller struct_size) and can refuse sink
// writes. DirectModule drives one instance of a module through its function
// table, with no dynamic loading: pass it a getter defined with PRAENORA_SDK_DEFINE_MODULE_GETTER,
// or any praenora_module_get_api_fn.
//
// Header-only; uses only the C++17 standard library, so it works with GoogleTest or any other
// test framework. FakeHost's callbacks are thread-safe, so modules may log from their own threads.
#ifndef PRAENORA_SDK_TESTING_HPP
#define PRAENORA_SDK_TESTING_HPP

#include <praenora/sdk/abi.h>
#include <praenora/sdk/status.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace praenora::sdk::testing {

// One message passed to the host's log callback; `level` is a PRAENORA_LOG_* value.
struct LogRecord {
    std::int32_t level;
    std::string message;
};

// A host API for tests. It must outlive every instance created with it.
class FakeHost {
public:
    FakeHost() {
        api_.struct_size = static_cast<std::uint32_t>(sizeof(praenora_host_api));
        api_.abi_major = PRAENORA_ABI_MAJOR;
        api_.abi_minor = PRAENORA_ABI_MINOR;
        api_.context = this;
        api_.log = &FakeHost::log_callback;
        api_.log_enabled = &FakeHost::log_enabled_callback;
        api_.cancel_requested = &FakeHost::cancel_requested_callback;
        api_.get_service = &FakeHost::get_service_callback;
    }

    FakeHost(const FakeHost&) = delete;
    FakeHost& operator=(const FakeHost&) = delete;

    const praenora_host_api* api() noexcept { return &api_; }

    // Every message logged so far, in order. Every level is enabled.
    std::vector<LogRecord> logs() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return logs_;
    }

    // From now on, cancel_requested reports nonzero, until reset_cancel(). A real host raises the
    // flag for one call only: every call starts with it clear.
    void request_cancel() noexcept { cancel_.store(true); }

    // Clears the cancel flag, as a real host does when the next call starts.
    void reset_cancel() noexcept { cancel_.store(false); }

    // Makes get_service(name, major) return `table`.
    void register_service(std::string name, std::uint32_t major, const void* table) {
        const std::lock_guard<std::mutex> lock(mutex_);
        services_[std::make_pair(std::move(name), major)] = table;
    }

    // Reports a different struct_size, for example to act as a host that lacks the later
    // callbacks. Call it before passing api() to a module; sizes above sizeof(praenora_host_api)
    // are not meaningful.
    void set_struct_size(std::uint32_t size) noexcept { api_.struct_size = size; }

    // While on, every write to the sinks of DirectModule calls made with this host is refused
    // with PRAENORA_STATUS_RESOURCE_EXHAUSTED, the way the runtime refuses oversized outputs.
    void refuse_sink_writes(bool refuse) noexcept { refuse_writes_.store(refuse); }
    bool sink_writes_refused() const noexcept { return refuse_writes_.load(); }

private:
    static FakeHost* self(void* context) noexcept { return static_cast<FakeHost*>(context); }

    static void PRAENORA_CALL log_callback(void* context, std::int32_t level, const char* msg,
                                           std::size_t size) noexcept {
        try {
            std::string message = msg != nullptr ? std::string(msg, size) : std::string();
            FakeHost* host = self(context);
            const std::lock_guard<std::mutex> lock(host->mutex_);
            host->logs_.push_back(LogRecord{level, std::move(message)});
        } catch (...) {
            // A callback never throws into module code; the message is lost.
        }
    }

    static std::int32_t PRAENORA_CALL log_enabled_callback(void*, std::int32_t) noexcept {
        return 1;
    }

    static std::int32_t PRAENORA_CALL cancel_requested_callback(void* context) noexcept {
        return self(context)->cancel_.load() ? 1 : 0;
    }

    static const void* PRAENORA_CALL get_service_callback(void* context, const char* name,
                                                          std::size_t name_size,
                                                          std::uint32_t major) noexcept {
        if (name == nullptr) {
            return nullptr;
        }
        try {
            FakeHost* host = self(context);
            const std::lock_guard<std::mutex> lock(host->mutex_);
            const auto found =
                host->services_.find(std::make_pair(std::string(name, name_size), major));
            return found == host->services_.end() ? nullptr : found->second;
        } catch (...) {
            return nullptr;
        }
    }

    praenora_host_api api_{};
    mutable std::mutex mutex_;
    std::vector<LogRecord> logs_;
    std::map<std::pair<std::string, std::uint32_t>, const void*> services_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> refuse_writes_{false};
};

// The outcome of one call: its status, what the module wrote to the output sink (result,
// describe or diagnostics document) and what it wrote to the error sink.
struct CallResult {
    praenora_status status = PRAENORA_STATUS_OK;
    std::string output, error;
};

// Drives one instance of a module through its v0 table. The constructor applies the host's
// acceptance rules to the table and throws std::invalid_argument, naming the problem, if the
// table would be rejected. Calls that need an instance return PRAENORA_STATUS_INVALID_STATE
// before create() succeeded or after destroy(). Not thread-safe; use it from one thread.
class DirectModule {
public:
    explicit DirectModule(praenora_module_get_api_fn get_api) : api_(checked_table(get_api)) {}

    DirectModule(const DirectModule&) = delete;
    DirectModule& operator=(const DirectModule&) = delete;

    // Destroys the instance if it still exists.
    ~DirectModule() { destroy(); }

    CallResult describe() {
        Capture out(nullptr);
        const praenora_status status = api_->describe(&out.sink);
        return CallResult{status, std::move(out.data), {}};
    }

    // Creates the instance on `host`, which must outlive it.
    CallResult create(FakeHost& host, std::string_view create_json) {
        if (instance_ != nullptr) {
            return own_error(PRAENORA_STATUS_INVALID_STATE, "create: the instance already exists");
        }
        host_ = &host;
        Capture error(host_);
        praenora_instance* instance = nullptr;
        const praenora_status status =
            api_->create(host.api(), bytes(create_json), &instance, &error.sink);
        if (status == PRAENORA_STATUS_OK && instance == nullptr) {
            return own_error(PRAENORA_STATUS_INTERNAL,
                             "create: the module returned OK without an instance");
        }
        if (status == PRAENORA_STATUS_OK) {
            instance_ = instance;
        }
        return CallResult{status, {}, std::move(error.data)};
    }

    CallResult initialize(std::string_view initialize_json) {
        if (instance_ == nullptr) {
            return no_instance("initialize");
        }
        Capture error(host_);
        const praenora_status status =
            api_->initialize(instance_, bytes(initialize_json), &error.sink);
        return CallResult{status, {}, std::move(error.data)};
    }

    CallResult invoke(std::string_view invocation_json) {
        if (instance_ == nullptr) {
            return no_instance("invoke");
        }
        Capture result(host_);
        Capture error(host_);
        const praenora_status status =
            api_->invoke(instance_, bytes(invocation_json), &result.sink, &error.sink);
        return CallResult{status, std::move(result.data), std::move(error.data)};
    }

    CallResult finalize() {
        if (instance_ == nullptr) {
            return no_instance("finalize");
        }
        Capture error(host_);
        const praenora_status status = api_->finalize(instance_, &error.sink);
        return CallResult{status, {}, std::move(error.data)};
    }

    // PRAENORA_STATUS_UNSUPPORTED if the module does not implement diagnostics.
    CallResult diagnostics() {
        if (instance_ == nullptr) {
            return no_instance("diagnostics");
        }
        if (api_->header.struct_size <
                offsetof(praenora_module_api_v0, diagnostics) + sizeof(api_->diagnostics) ||
            api_->diagnostics == nullptr) {
            return own_error(PRAENORA_STATUS_UNSUPPORTED,
                             "diagnostics: the module does not implement diagnostics");
        }
        Capture out(host_);
        const praenora_status status = api_->diagnostics(instance_, &out.sink);
        return CallResult{status, std::move(out.data), {}};
    }

    // Calls the module's destroy() if the instance exists; later calls do nothing.
    void destroy() noexcept {
        if (instance_ != nullptr) {
            praenora_instance* instance = instance_;
            instance_ = nullptr;
            api_->destroy(instance);
        }
    }

private:
    // A sink that collects the bytes written to it.
    struct Capture {
        explicit Capture(const FakeHost* refusing_host) noexcept
            : sink{this, &Capture::write}, host(refusing_host) {}
        Capture(const Capture&) = delete;
        Capture& operator=(const Capture&) = delete;

        static praenora_status PRAENORA_CALL write(void* context, const char* chunk,
                                                   std::size_t size) noexcept {
            auto* self = static_cast<Capture*>(context);
            if (chunk == nullptr && size != 0) {
                return PRAENORA_STATUS_INVALID_ARGUMENT;
            }
            if (self->host != nullptr && self->host->sink_writes_refused()) {
                return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
            }
            try {
                if (size != 0) {
                    self->data.append(chunk, size);
                }
                return PRAENORA_STATUS_OK;
            } catch (...) {
                return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
            }
        }

        praenora_sink sink;
        const FakeHost* host;
        std::string data;
    };

    static const praenora_module_api_v0* checked_table(praenora_module_get_api_fn get_api) {
        if (get_api == nullptr) {
            throw std::invalid_argument("DirectModule: the entry point is NULL");
        }
        const praenora_api_header* header = get_api(0u);
        if (header == nullptr) {
            throw std::invalid_argument("DirectModule: the module returned no table for ABI 0");
        }
        if (header->abi_major != 0u) {
            throw std::invalid_argument("DirectModule: the table reports ABI major " +
                                        std::to_string(header->abi_major) + ", expected 0");
        }
        if (header->reserved != 0u) {
            throw std::invalid_argument("DirectModule: the table header's reserved field is not 0");
        }
        if (header->struct_size < PRAENORA_MODULE_API_V0_BASE_SIZE) {
            throw std::invalid_argument(
                "DirectModule: the table's struct_size " + std::to_string(header->struct_size) +
                " is below the v0 base size " + std::to_string(PRAENORA_MODULE_API_V0_BASE_SIZE));
        }
        const auto* api = reinterpret_cast<const praenora_module_api_v0*>(header);
        const char* missing = api->describe == nullptr     ? "describe"
                              : api->create == nullptr     ? "create"
                              : api->initialize == nullptr ? "initialize"
                              : api->invoke == nullptr     ? "invoke"
                              : api->finalize == nullptr   ? "finalize"
                              : api->destroy == nullptr    ? "destroy"
                                                           : nullptr;
        if (missing != nullptr) {
            throw std::invalid_argument(std::string("DirectModule: the required function ") +
                                        missing + " is NULL");
        }
        return api;
    }

    static praenora_bytes bytes(std::string_view text) noexcept {
        return praenora_bytes{text.data(), text.size()};
    }

    // An error raised by the test kit itself, as an error document.
    static CallResult own_error(praenora_status status, const std::string& message) {
        return CallResult{status,
                          {},
                          "{\"code\":" + std::to_string(status) + ",\"message\":\"" + message +
                              "\",\"details\":{}}"};
    }

    static CallResult no_instance(const char* call) {
        return own_error(PRAENORA_STATUS_INVALID_STATE,
                         std::string(call) + ": no instance (create() has not succeeded)");
    }

    const praenora_module_api_v0* api_;
    FakeHost* host_ = nullptr;
    praenora_instance* instance_ = nullptr;
};

} // namespace praenora::sdk::testing

#endif // PRAENORA_SDK_TESTING_HPP
