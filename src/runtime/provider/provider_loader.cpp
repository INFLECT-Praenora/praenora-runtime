#include "provider/provider_loader.hpp"

#include "core/paths.hpp"
#include "core/semver.hpp"
#include "document/json_util.hpp"
#include "loader/library_file.hpp"
#include "loader/output_sink.hpp"
#include "loader/shared_library.hpp"
#include "observability/logging.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/sdk/status.h>

#include <spdlog/logger.h>
#include <spdlog/sinks/null_sink.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <system_error>

namespace praenora::detail {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// describe() documents and start() error documents are small; these bound what a broken provider
// can make the runtime keep.
constexpr std::size_t kMaxDescribeBytes = std::size_t{1} << 20;
constexpr std::size_t kMaxErrorBytes = std::size_t{64} << 10;

// The size of the provider interface v0.0 table: the header and its three functions.
constexpr std::size_t kTableV0Size =
    offsetof(praenora_provider_api_v0, get_service) + sizeof(praenora_provider_api_v0::get_service);
static_assert(kTableV0Size == sizeof(praenora_provider_api_v0),
              "the v0.0 table ends with get_service");

std::string identity(const ProviderManifest& manifest) {
    return manifest.name + "@" + to_string(manifest.version);
}

std::string dump(const json& value) {
    // Text may come from providers: bytes that are not UTF-8 are replaced, not refused.
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string joined(const std::vector<std::string>& items) {
    std::string text;
    for (const auto& item : items) {
        text += (text.empty() ? "" : ", ") + item;
    }
    return text;
}

// Raises errors about the provider of one manifest, located in the manifest.
LocatedFailure provider_failure(const ProviderManifest& manifest) {
    return LocatedFailure("provider " + identity(manifest), manifest.manifest_path);
}

// LIBRARY_LOAD_FAILED with details.reason `reason` and the other `details`.
[[noreturn]] void load_failed(const LocatedFailure& fail, const char* reason,
                              const std::string& message, const std::string& pointer = "",
                              json details = json::object()) {
    details["reason"] = reason;
    fail(ErrorCode::LIBRARY_LOAD_FAILED, message, pointer, dump(details));
}

// The library's path with symbolic links resolved, as far as they can be: two manifests name the
// same library when these are equal.
fs::path canonical_library(const fs::path& library) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(library, ec);
    return ec ? library.lexically_normal() : canonical;
}

// The error document a failed start() wrote: the JSON object it is (code and message filled in
// from the status when missing), or {"code", "message"} made of its text.
json cause_of(praenora_status status, const std::string& text) {
    json cause;
    try {
        cause = parse_json_text(text, "error document");
    } catch (const Error&) {
        cause = nullptr;
    }
    if (!cause.is_object()) {
        cause = json{{"code", status}, {"message", text}};
    }
    if (!cause.contains("code")) {
        cause["code"] = status;
    }
    if (!cause.contains("message") || !cause.at("message").is_string()) {
        cause["message"] = "";
    }
    return cause;
}

json services_json(std::vector<ServiceVersion> services) {
    std::sort(services.begin(), services.end());
    json list = json::array();
    for (const auto& service : services) {
        list.push_back(
            {{"name", service.name}, {"major", service.major}, {"minor", service.minor}});
    }
    return list;
}

// Checks that describe() agrees with the manifest: LIBRARY_LOAD_FAILED with reason "identity".
void check_identity(const ProviderManifest& manifest, const json& describe,
                    const LocatedFailure& fail) {
    const auto malformed = [&fail](const std::string& what) {
        load_failed(fail, "identity",
                    "describe() wrote a document that is not a provider identity: " + what);
    };
    if (!describe.is_object()) {
        malformed("it is not a JSON object");
    }
    for (const char* key : {"name", "version"}) {
        if (!describe.contains(key) || !describe.at(key).is_string()) {
            malformed(std::string("\"") + key + "\" is not a string");
        }
    }
    if (!describe.contains("services") || !describe.at("services").is_array()) {
        malformed("\"services\" is not an array");
    }
    if (!describe.contains("engine") || !describe.at("engine").is_object()) {
        malformed("\"engine\" is not an object");
    }
    std::vector<ServiceVersion> described;
    for (const auto& service : describe.at("services")) {
        if (!service.is_object() || !service.contains("name") || !service.at("name").is_string() ||
            !service.contains("major") || !service.contains("minor") ||
            !uint32_of(service.at("major")) || !uint32_of(service.at("minor"))) {
            malformed(
                "a service is not {\"name\": string, \"major\": integer, \"minor\": integer}");
        }
        described.push_back({service.at("name").get<std::string>(), *uint32_of(service.at("major")),
                             *uint32_of(service.at("minor"))});
    }
    const std::string version_text = describe.at("version").get<std::string>();
    SemVer version;
    try {
        version = parse_semver(version_text);
    } catch (const Error& error) {
        load_failed(fail, "identity",
                    "describe() reports version '" + version_text +
                        "', which is not a usable semantic version: " + error.what());
    }

    std::vector<std::string> differences;
    json details = json::array();
    const auto differ = [&](const char* field, const json& in_manifest, const json& in_describe) {
        const auto text = [](const json& value) {
            return value.is_string() ? value.get<std::string>() : dump(value);
        };
        differences.push_back(std::string(field) + " (manifest " + text(in_manifest) +
                              ", describe() " + text(in_describe) + ")");
        details.push_back({{"field", field}, {"manifest", in_manifest}, {"describe", in_describe}});
    };
    if (describe.at("name") != manifest.name) {
        differ("name", manifest.name, describe.at("name"));
    }
    if (version != manifest.version) {
        differ("version", to_string(manifest.version), version_text);
    }
    const json expected_services = services_json(manifest.services);
    const json described_services = services_json(described);
    if (expected_services != described_services) {
        differ("services", expected_services, described_services);
    }
    if (describe.at("engine") != manifest.engine) {
        differ("engine", manifest.engine, describe.at("engine"));
    }
    if (!differences.empty()) {
        load_failed(fail, "identity",
                    "describe() disagrees with the manifest: " + joined(differences), "",
                    json{{"differences", std::move(details)}});
    }
}

// Runs describe() and parses what it wrote: LIBRARY_LOAD_FAILED with reason "identity" when it
// fails or writes no JSON.
json describe_document(const praenora_provider_api_v0& api, const LocatedFailure& fail) {
    OutputSink out(kMaxDescribeBytes);
    const praenora_status status = api.describe(out.sink());
    if (status != PRAENORA_STATUS_OK) {
        load_failed(fail, "identity",
                    "describe() failed with status " + status_text(status) +
                        (out.refused() ? "; its output exceeded " +
                                             std::to_string(kMaxDescribeBytes) + " bytes"
                                       : std::string()));
    }
    try {
        return parse_json_text(out.text(), "describe()");
    } catch (const Error& error) {
        load_failed(fail, "identity",
                    std::string("describe() did not write JSON: ") + error.what());
    }
}

} // namespace

// What the runtime gives one provider: its praenora_provider_host, whose log callbacks write to
// the provider's logger, and whose store callbacks reach `compiled`, when there is one (NULL
// otherwise). The logger can be replaced at any time; the host stays where it is for the rest of
// the process.
class ProviderHost {
public:
    ProviderHost(std::string name, std::string provider, CompiledModelAccess* compiled)
        : name_(std::move(name)), provider_(std::move(provider)), compiled_(compiled) {
        host_.struct_size = static_cast<std::uint32_t>(sizeof(praenora_provider_host));
        host_.reserved = 0;
        host_.context = this;
        host_.log = &ProviderHost::log;
        host_.log_enabled = &ProviderHost::log_enabled;
        host_.load_compiled = compiled_ != nullptr ? &ProviderHost::load_compiled : nullptr;
        host_.save_compiled = compiled_ != nullptr ? &ProviderHost::save_compiled : nullptr;
    }
    ProviderHost(const ProviderHost&) = delete;
    ProviderHost& operator=(const ProviderHost&) = delete;

    const praenora_provider_host* host() const noexcept { return &host_; }

    std::shared_ptr<spdlog::logger> logger() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return logger_;
    }

    // From now on the provider's records go to its logger "provider.<name>" of `logs`, until
    // another call names other logs or `logs` is destroyed: then they go to a logger on a null
    // sink, so the provider keeps none of the destroyed logs' sinks or files.
    void set_logs(LogHub& logs) {
        auto logger = logs.logger("provider." + name_);
        bool new_logs = false;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            logger_ = std::move(logger);
            new_logs = logs_ != &logs;
            logs_ = &logs;
        }
        if (new_logs) {
            // The host is never destroyed, so the callback may run whenever the logs go.
            logs.on_destroy([this, destroyed = &logs] { release(destroyed); });
        }
    }

    // The attributes of the provider's records: praenora.provider (name@version).
    std::vector<LogAttribute> attributes() const { return {{"praenora.provider", provider_}}; }

private:
    // Called when `destroyed` goes: lets go of its logger if the provider still writes to it.
    void release(const LogHub* destroyed) noexcept {
        try {
            auto released = released_logger();
            const std::lock_guard<std::mutex> lock(mutex_);
            if (logs_ == destroyed) {
                logger_ = std::move(released);
                logs_ = nullptr;
            }
        } catch (...) {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (logs_ == destroyed) {
                logger_.reset();
                logs_ = nullptr;
            }
        }
    }

    // A logger "provider.<name>" that records nothing, on the process's null sink. The sink is
    // allocated once and never freed, as the hosts are: a log hub destroyed during the process's
    // exit may still call release().
    std::shared_ptr<spdlog::logger> released_logger() const {
        static const auto* const sink = new std::shared_ptr<spdlog::sinks::null_sink_mt>(
            std::make_shared<spdlog::sinks::null_sink_mt>());
        auto logger = std::make_shared<spdlog::logger>("provider." + name_, *sink);
        logger->set_level(spdlog::level::off);
        return logger;
    }

    // Thread-safe; they never throw into provider code.
    static void PRAENORA_CALL log(void* context, std::int32_t level, const char* message,
                                  std::size_t size) noexcept {
        const auto* self = static_cast<const ProviderHost*>(context);
        if (self == nullptr || (message == nullptr && size != 0)) {
            return;
        }
        try {
            const auto logger = self->logger();
            const LogLevel log_level = log_level_from_abi(level);
            if (logger == nullptr || !detail::log_enabled(*logger, log_level)) {
                return;
            }
            const std::string_view text =
                message == nullptr ? std::string_view() : std::string_view(message, size);
            log_event(*logger, log_level, text, self->attributes());
        } catch (...) {
            // The message is lost.
        }
    }

    static std::int32_t PRAENORA_CALL log_enabled(void* context, std::int32_t level) noexcept {
        const auto* self = static_cast<const ProviderHost*>(context);
        if (self == nullptr) {
            return 0;
        }
        try {
            const auto logger = self->logger();
            return logger != nullptr && detail::log_enabled(*logger, log_level_from_abi(level))
                       ? 1
                       : 0;
        } catch (...) {
            return 0;
        }
    }

    static std::int32_t PRAENORA_CALL load_compiled(void* context, const char* key,
                                                    std::size_t key_size,
                                                    praenora_sink* out) noexcept {
        const auto* self = static_cast<const ProviderHost*>(context);
        if (self == nullptr || self->compiled_ == nullptr || (key == nullptr && key_size != 0) ||
            out == nullptr || out->write == nullptr) {
            return 0;
        }
        try {
            return self->compiled_->load(std::string_view(key == nullptr ? "" : key, key_size),
                                         *out)
                       ? 1
                       : 0;
        } catch (...) {
            return 0;
        }
    }

    static void PRAENORA_CALL save_compiled(void* context, const char* key, std::size_t key_size,
                                            const char* data, std::size_t size) noexcept {
        const auto* self = static_cast<const ProviderHost*>(context);
        if (self == nullptr || self->compiled_ == nullptr || (key == nullptr && key_size != 0) ||
            (data == nullptr && size != 0)) {
            return;
        }
        try {
            self->compiled_->save(std::string_view(key == nullptr ? "" : key, key_size),
                                  std::string_view(data == nullptr ? "" : data, size));
        } catch (...) {
            // Not kept.
        }
    }

    std::string name_;     // the provider's name
    std::string provider_; // name@version, the attribute of its records
    CompiledModelAccess* compiled_; // the shared compiled models, or nullptr
    praenora_provider_host host_{};
    mutable std::mutex mutex_;
    std::shared_ptr<spdlog::logger> logger_; // guarded by mutex_
    const LogHub* logs_ = nullptr;           // the logs logger_ belongs to; guarded by mutex_
};

// The providers of the process, by name: an entry is made when a provider's library opens and is
// never removed. Never destroyed either: providers live until the process ends, and their threads
// may log through their hosts at any time, even while the process exits.
class ProviderRegistry {
public:
    static ProviderRegistry& instance() {
        static ProviderRegistry* const registry = new ProviderRegistry;
        return *registry;
    }

    const LoadedProvider& load(const ProviderManifest& manifest,
                               const std::vector<ServiceVersion>& services,
                               const std::optional<std::string>& validated_sha256, LogHub* logs,
                               CompiledModelAccess* compiled);
    const LoadedProvider* find(std::string_view name);
    std::vector<std::string> names();

private:
    ProviderRegistry() = default;

    LoadedProvider& open(const ProviderManifest& manifest, const std::string& digest,
                         fs::path canonical, const LocatedFailure& fail);
    static void start(LoadedProvider& provider, const ProviderManifest& manifest, LogHub* logs,
                      CompiledModelAccess* compiled, const LocatedFailure& fail);

    std::mutex mutex_;
    std::map<std::string, std::unique_ptr<LoadedProvider>, std::less<>> providers_;
};

LoadedProvider::~LoadedProvider() = default;

const ProviderManifest& LoadedProvider::manifest() const noexcept { return manifest_; }

const std::string& LoadedProvider::library_sha256() const noexcept { return library_sha256_; }

const json& LoadedProvider::describe() const noexcept { return describe_; }

const void* LoadedProvider::service(std::string_view name, std::uint32_t major) const {
    const std::lock_guard<std::mutex> lock(services_mutex_);
    const auto table = services_.find(std::make_pair(std::string(name), major));
    return table == services_.end() ? nullptr : table->second;
}

const LoadedProvider& ProviderRegistry::load(const ProviderManifest& manifest,
                                             const std::vector<ServiceVersion>& services,
                                             const std::optional<std::string>& validated_sha256,
                                             LogHub* logs, CompiledModelAccess* compiled) {
    const LocatedFailure fail = provider_failure(manifest);

    // 0. The services, each offered by the manifest: their indexes in its services.
    std::vector<std::size_t> offered;
    for (const auto& service : services) {
        const auto match = std::find_if(
            manifest.services.begin(), manifest.services.end(), [&](const ServiceVersion& entry) {
                return entry.name == service.name && entry.major == service.major &&
                       entry.minor >= service.minor;
            });
        if (match == manifest.services.end()) {
            fail(ErrorCode::INVALID_ARGUMENT, "the manifest does not offer service " +
                                                  to_string(service) + " (" + service.name + "@" +
                                                  std::to_string(service.major) + ", minor " +
                                                  std::to_string(service.minor) + " or later)");
        }
        offered.push_back(static_cast<std::size_t>(match - manifest.services.begin()));
    }

    // 1. The file and its digests, before any of its code runs.
    const std::string digest = check_provider_library_file(manifest);
    if (validated_sha256 && *validated_sha256 != digest) {
        fail(ErrorCode::LIBRARY_DIGEST_MISMATCH,
             "the provider library '" + path_to_utf8(manifest.library_path) +
                 "' changed after validation: its SHA-256 is now " + digest + ", it was " +
                 *validated_sha256,
             "/library/path");
    }
    fs::path canonical = canonical_library(manifest.library_path);

    const std::lock_guard<std::mutex> lock(mutex_);
    // 2. One library of a provider name in the process: the same file, or a new one opened.
    LoadedProvider* provider = nullptr;
    const auto existing = providers_.find(manifest.name);
    if (existing == providers_.end()) {
        provider = &open(manifest, digest, std::move(canonical), fail);
    } else {
        provider = existing->second.get();
        if (provider->canonical_library_ != canonical || provider->library_sha256_ != digest) {
            const ProviderManifest& loaded = provider->manifest_;
            load_failed(
                fail, "conflict",
                "provider " + manifest.name + " is loaded in this process from '" +
                    path_to_utf8(loaded.library_path) + "' (SHA-256 " + provider->library_sha256_ +
                    "); this manifest names '" + path_to_utf8(manifest.library_path) +
                    "' (SHA-256 " + digest + "), and a process holds one library of a provider",
                "/library/path",
                json{{"loaded",
                      {{"manifest", path_to_utf8(loaded.manifest_path)},
                       {"library", path_to_utf8(loaded.library_path)},
                       {"library_sha256", provider->library_sha256_}}}});
        }
    }

    switch (provider->state_) {
    case LoadedProvider::State::start_failed: {
        // Without a kept failure (memory ran out while it was being described), a plain one.
        const auto kept = provider->start_failure_.value_or(
            std::make_pair(std::string("start() failed"), std::string(R"({"reason":"start"})")));
        fail(ErrorCode::LIBRARY_LOAD_FAILED,
             "start() failed when the provider was first loaded in this process, and it is "
             "not called again: " +
                 kept.first,
             "", kept.second);
    }
    case LoadedProvider::State::started:
        check_identity(manifest, provider->describe_, fail);
        if (logs != nullptr) {
            provider->host_->set_logs(*logs);
        }
        break;
    case LoadedProvider::State::opened:
        start(*provider, manifest, logs, compiled, fail);
        break;
    }

    // The tables of the services, from get_service().
    std::vector<std::string> missing;
    std::string first_missing_pointer;
    for (std::size_t i = 0; i < services.size(); ++i) {
        const ServiceVersion& service = services[i];
        const void* table = provider->api_.get_service(service.name.data(), service.name.size(),
                                                       service.major);
        if (table == nullptr) {
            if (missing.empty()) {
                first_missing_pointer = "/services/" + std::to_string(offered[i]);
            }
            missing.push_back(service.name + "@" + std::to_string(service.major));
            continue;
        }
        const std::lock_guard<std::mutex> services_lock(provider->services_mutex_);
        provider->services_[std::make_pair(service.name, service.major)] = table;
    }
    if (!missing.empty()) {
        load_failed(fail, "service",
                    "get_service() returned NULL for " + joined(missing) +
                        ", which the manifest offers",
                    first_missing_pointer, json{{"services", missing}});
    }
    return *provider;
}

LoadedProvider& ProviderRegistry::open(const ProviderManifest& manifest, const std::string& digest,
                                       fs::path canonical, const LocatedFailure& fail) {
    std::optional<SharedLibrary> library;
    try {
        library.emplace(SharedLibrary::open(manifest.library_path, {}, "provider library"));
    } catch (const Error& error) {
        load_failed(fail, "library", error.what(), "/library/path");
    }
    // In the process from here on, whatever its checks and its start() do: pinned, and recorded,
    // so that no other library of this provider's name joins it.
    library->pin();
    std::unique_ptr<LoadedProvider> created(new LoadedProvider);
    created->manifest_ = manifest;
    created->canonical_library_ = std::move(canonical);
    created->library_sha256_ = digest;
    created->library_ = std::make_unique<SharedLibrary>(std::move(*library));
    LoadedProvider& provider = *created;
    providers_.emplace(manifest.name, std::move(created));
    return provider;
}

void ProviderRegistry::start(LoadedProvider& provider, const ProviderManifest& manifest,
                             LogHub* logs, CompiledModelAccess* compiled,
                             const LocatedFailure& fail) {
    const std::string library_file = path_to_utf8(manifest.library_path);

    // 3.-4. The entry point and its table. They and describe() are free of side effects, so a
    // load after one of them failed runs them again.
    void* const entry = provider.library_->symbol(PRAENORA_PROVIDER_ENTRY_POINT);
    if (entry == nullptr) {
        fail(ErrorCode::ENTRY_POINT_MISSING, "the library '" + library_file +
                                                 "' does not export " +
                                                 PRAENORA_PROVIDER_ENTRY_POINT);
    }
    praenora_provider_get_api_fn get_api = nullptr;
    static_assert(sizeof(get_api) == sizeof(entry), "code and data pointers have one size");
    std::memcpy(&get_api, &entry, sizeof get_api);
    const praenora_provider_api_v0* table = get_api();
    if (table == nullptr) {
        fail(ErrorCode::API_TABLE_INVALID,
             std::string(PRAENORA_PROVIDER_ENTRY_POINT) + "() returned NULL: no function table");
    }
    const praenora_api_header& header = table->header;
    if (header.abi_major != PRAENORA_PROVIDER_ABI_MAJOR) {
        fail(ErrorCode::API_TABLE_INVALID,
             "the function table reports provider interface major " +
                 std::to_string(header.abi_major) +
                 "; this runtime implements provider interface major " +
                 std::to_string(PRAENORA_PROVIDER_ABI_MAJOR));
    }
    if (header.reserved != 0) {
        fail(ErrorCode::API_TABLE_INVALID, "the function table's header.reserved is " +
                                               std::to_string(header.reserved) +
                                               "; it must be 0");
    }
    if (header.struct_size < kTableV0Size) {
        fail(ErrorCode::API_TABLE_INVALID,
             "the function table's struct_size " + std::to_string(header.struct_size) +
                 " is below " + std::to_string(kTableV0Size) +
                 ", the size of the provider interface v0 table");
    }
    praenora_provider_api_v0 api{};
    std::memcpy(&api, table, kTableV0Size);
    std::vector<std::string> null_functions;
    if (api.describe == nullptr) {
        null_functions.emplace_back("describe");
    }
    if (api.start == nullptr) {
        null_functions.emplace_back("start");
    }
    if (api.get_service == nullptr) {
        null_functions.emplace_back("get_service");
    }
    if (!null_functions.empty()) {
        fail(ErrorCode::API_TABLE_INVALID,
             std::string("the function table's function") +
                 (null_functions.size() > 1 ? "s " : " ") + joined(null_functions) +
                 (null_functions.size() > 1 ? " are" : " is") + " NULL");
    }

    // 5. The identity.
    json describe = describe_document(api, fail);
    check_identity(manifest, describe, fail);

    // 6. start(), once in the process, with a host that stays for good. The host is made once:
    // a log hub may hold a callback into it from here on, so it is never replaced, even when
    // something below throws and a later load comes back here.
    provider.manifest_ = manifest;
    provider.describe_ = std::move(describe);
    provider.api_ = api;
    if (!provider.host_) {
        provider.host_ =
            std::make_unique<ProviderHost>(manifest.name, identity(manifest), compiled);
    }
    if (logs != nullptr) {
        provider.host_->set_logs(*logs);
    }
    OutputSink error(kMaxErrorBytes);
    praenora_status status = PRAENORA_STATUS_INTERNAL;
    try {
        status = api.start(provider.host_->host(), error.sink());
    } catch (...) {
        // No exception may leave a provider; one that does fails the start.
        status = PRAENORA_STATUS_INTERNAL;
    }
    // The state first, before anything that can throw: start() has been called, and is never
    // called again.
    if (status == PRAENORA_STATUS_OK) {
        provider.state_ = LoadedProvider::State::started;
    } else {
        provider.state_ = LoadedProvider::State::start_failed;
        provider.start_failure_.reset(); // a later load reports a plain failure until it is set
        const json cause = cause_of(status, error.text());
        const std::string message = "start() failed with status " + status_text(status) + ": " +
                                    cause.at("message").get<std::string>();
        const std::string details = dump(json{{"reason", "start"}, {"cause", cause}});
        provider.start_failure_ = std::make_pair(message, details);
        fail(ErrorCode::LIBRARY_LOAD_FAILED, message, "", details);
    }

    // 7. The event.
    if (const auto logger = provider.host_->logger()) {
        auto attributes = provider.host_->attributes();
        attributes.push_back({"praenora.event", "provider.loaded"});
        attributes.push_back({"praenora.library_sha256", provider.library_sha256_});
        log_event(*logger, LogLevel::info, "provider " + identity(manifest) + " loaded and started",
                  attributes);
    }
}

const LoadedProvider* ProviderRegistry::find(std::string_view name) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto provider = providers_.find(name);
    return provider == providers_.end() ||
                   provider->second->state_ != LoadedProvider::State::started
               ? nullptr
               : provider->second.get();
}

std::vector<std::string> ProviderRegistry::names() {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    for (const auto& entry : providers_) {
        names.push_back(entry.first);
    }
    return names;
}

std::string check_provider_library_file(const ProviderManifest& manifest) {
    LibraryFile file;
    file.owner = provider_failure(manifest).owner();
    file.kind = "provider";
    file.manifest_path = manifest.manifest_path;
    file.library_path = manifest.library_path;
    file.pinned_sha256 = manifest.library_sha256;
    return check_library_file(file);
}

const LoadedProvider& load_provider(const ProviderManifest& manifest,
                                    const std::vector<ServiceVersion>& services,
                                    const std::optional<std::string>& validated_sha256,
                                    LogHub* logs, CompiledModelAccess* compiled) {
    return ProviderRegistry::instance().load(manifest, services, validated_sha256, logs, compiled);
}

const LoadedProvider* loaded_provider(std::string_view name) {
    return ProviderRegistry::instance().find(name);
}

std::vector<std::string> opened_providers_for_testing() {
    return ProviderRegistry::instance().names();
}

} // namespace praenora::detail
