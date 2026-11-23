#include "observability/otlp/otlp_settings.hpp"

#include "platform/platform.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <utility>

namespace praenora::detail {

namespace {

constexpr std::int64_t kMaxMilliseconds = std::numeric_limits<std::int32_t>::max();
constexpr std::int64_t kDefaultTimeoutMs = 10000;
constexpr std::int64_t kDefaultIntervalMs = 60000;
constexpr std::int64_t kMinIntervalMs = 100;
constexpr const char* kDefaultServiceName = "praenora-runtime";

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equals_ignoring_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                                              [](char x, char y) { return lower(x) == lower(y); });
}

std::string_view trimmed(std::string_view text) {
    const auto blank = [](char c) { return c == ' ' || c == '\t'; };
    while (!text.empty() && blank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && blank(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_alnum(char c) { return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

int hex_value(char c) {
    if (is_digit(c)) {
        return c - '0';
    }
    const char l = lower(c);
    return l >= 'a' && l <= 'f' ? l - 'a' + 10 : -1;
}

bool is_token_char(char c) {
    static constexpr std::string_view kExtra = "!#$%&'*+-.^_`|~";
    return is_alnum(c) || kExtra.find(c) != std::string_view::npos;
}

bool is_control(unsigned char c) { return c < 0x20 || c == 0x7f; }

// A whole number of milliseconds from `min` to kMaxMilliseconds, written with digits only.
std::optional<std::int64_t> parse_milliseconds(std::string_view text, std::int64_t min) {
    text = trimmed(text);
    if (text.empty() || text.size() > 10) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    for (const char c : text) {
        if (!is_digit(c)) {
            return std::nullopt;
        }
        value = value * 10 + (c - '0');
    }
    if (value < min || value > kMaxMilliseconds) {
        return std::nullopt;
    }
    return value;
}

std::string milliseconds_reason(std::int64_t min) {
    return "not a whole number of milliseconds from " + std::to_string(min) + " to " +
           std::to_string(kMaxMilliseconds);
}

bool valid_ipv6(std::string_view host) {
    if (host.find(':') == std::string_view::npos) {
        return false;
    }
    return std::all_of(host.begin(), host.end(),
                       [](char c) { return hex_value(c) >= 0 || c == ':' || c == '.'; });
}

bool valid_host_name(std::string_view host) {
    return !host.empty() && std::all_of(host.begin(), host.end(),
                                        [](char c) { return is_alnum(c) || c == '-' || c == '.'; });
}

std::string endpoint_url(const HttpEndpoint& endpoint) {
    const bool ipv6 = endpoint.host.find(':') != std::string::npos;
    return "http://" + (ipv6 ? "[" + endpoint.host + "]" : endpoint.host) + ":" +
           std::to_string(endpoint.port) + endpoint.path;
}

// What the combination below needs of one environment variable or option: whether it is set, and
// its value when that is valid.
template <class T> struct Read {
    bool set = false;
    std::optional<T> value;
};

class Resolver {
public:
    Resolver(const TelemetryOptions& options, const EnvironmentLookup& environment,
             std::vector<IgnoredSetting>& ignored)
        : options_(options), environment_(environment), ignored_(ignored) {}

    std::optional<std::string> variable(const char* name) const {
        if (!environment_) {
            return std::nullopt;
        }
        std::optional<std::string> value = environment_(name);
        if (value && value->empty()) {
            return std::nullopt;
        }
        return value;
    }

    void ignore(std::string name, std::string reason) {
        ignored_.push_back({std::move(name), std::move(reason)});
    }

    Read<HttpEndpoint> endpoint_variable(const char* name) {
        Read<HttpEndpoint> read;
        if (const std::optional<std::string> value = variable(name)) {
            read.set = true;
            read.value = endpoint(name, *value);
        }
        return read;
    }

    Read<HttpEndpoint> endpoint_option(const char* name, const std::string& value) {
        Read<HttpEndpoint> read;
        if (!value.empty()) {
            read.set = true;
            read.value = endpoint(name, value);
        }
        return read;
    }

    // Whether the protocol variable `name` is set, and its value is http/json.
    Read<bool> protocol_variable(const char* name) {
        Read<bool> read;
        if (const std::optional<std::string> value = variable(name)) {
            read.set = true;
            if (equals_ignoring_case(trimmed(*value), "http/json")) {
                read.value = true;
            } else {
                ignore(name, "the protocol is not supported, only http/json is; the signals it "
                             "applies to export nothing");
            }
        }
        return read;
    }

    // OTEL_<SIGNAL>_EXPORTER: true for otlp, false for none.
    std::optional<bool> exporter_variable(const char* name) {
        const std::optional<std::string> value = variable(name);
        if (!value) {
            return std::nullopt;
        }
        const std::string_view text = trimmed(*value);
        if (equals_ignoring_case(text, "otlp")) {
            return true;
        }
        if (equals_ignoring_case(text, "none")) {
            return false;
        }
        ignore(name, "the exporter is not supported, only otlp and none are; otlp is used");
        return std::nullopt;
    }

    std::optional<HttpHeaders> list_variable(const char* name, bool header_names) {
        const std::optional<std::string> value = variable(name);
        if (!value) {
            return std::nullopt;
        }
        std::string reason;
        std::optional<HttpHeaders> list = parse_key_value_list(*value, header_names, reason);
        if (!list) {
            ignore(name, reason);
        }
        return list;
    }

    std::optional<std::chrono::milliseconds> milliseconds_variable(const char* name,
                                                                   std::int64_t min) {
        const std::optional<std::string> value = variable(name);
        if (!value) {
            return std::nullopt;
        }
        if (const std::optional<std::int64_t> ms = parse_milliseconds(*value, min)) {
            return std::chrono::milliseconds(*ms);
        }
        ignore(name, "the value is " + milliseconds_reason(min));
        return std::nullopt;
    }

    std::optional<std::chrono::milliseconds> milliseconds_option(const char* name,
                                                                 std::int64_t value,
                                                                 std::int64_t min,
                                                                 std::int64_t default_value) {
        if (value == default_value) {
            return std::nullopt;
        }
        if (value < min || value > kMaxMilliseconds) {
            ignore(name, "the value is " + milliseconds_reason(min));
            return std::nullopt;
        }
        return std::chrono::milliseconds(value);
    }

    // The headers of TelemetryOptions, checked as a header list is.
    std::optional<HttpHeaders> headers_option() {
        if (options_.headers.empty()) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < options_.headers.size(); ++i) {
            const auto& [key, value] = options_.headers[i];
            std::string reason;
            if (!valid_list_entry(key, value, true, reason)) {
                ignore("TelemetryOptions::headers",
                       "entry " + std::to_string(i + 1) + ": " + reason);
                return std::nullopt;
            }
        }
        return options_.headers;
    }

    std::optional<HttpHeaders> resource_option() {
        for (std::size_t i = 0; i < options_.resource_attributes.size(); ++i) {
            const auto& [key, value] = options_.resource_attributes[i];
            std::string reason;
            if (!valid_list_entry(key, value, false, reason)) {
                ignore("TelemetryOptions::resource_attributes",
                       "entry " + std::to_string(i + 1) + ": " + reason);
                return std::nullopt;
            }
        }
        return options_.resource_attributes;
    }

    static bool valid_list_entry(const std::string& key, const std::string& value,
                                 bool header_names, std::string& reason) {
        if (key.empty()) {
            reason = "the key is empty";
            return false;
        }
        if (header_names) {
            if (!std::all_of(key.begin(), key.end(), is_token_char)) {
                reason = "the name is not an HTTP header name";
                return false;
            }
            for (const char* reserved :
                 {"content-type", "content-length", "host", "transfer-encoding", "connection"}) {
                if (equals_ignoring_case(key, reserved)) {
                    reason = "the header " + key + " is set by the export itself";
                    return false;
                }
            }
        } else if (key.find_first_of("=,") != std::string::npos ||
                   std::any_of(key.begin(), key.end(),
                               [](char c) { return is_control(static_cast<unsigned char>(c)); })) {
            reason = "the key holds '=', ',' or a control character";
            return false;
        }
        if (std::any_of(value.begin(), value.end(),
                        [](char c) { return is_control(static_cast<unsigned char>(c)); })) {
            reason = "the value holds a control character";
            return false;
        }
        return true;
    }

private:
    std::optional<HttpEndpoint> endpoint(const char* name, const std::string& value) {
        std::string reason;
        std::optional<HttpEndpoint> parsed = parse_http_endpoint(value, reason);
        if (!parsed) {
            ignore(name, reason + "; the signals it applies to export nothing");
        }
        return parsed;
    }

    const TelemetryOptions& options_;
    const EnvironmentLookup& environment_;
    std::vector<IgnoredSetting>& ignored_;
};

// Replaces the value of `key` in `list`, or appends the pair.
void put(HttpHeaders& list, const std::string& key, const std::string& value) {
    for (auto& entry : list) {
        if (entry.first == key) {
            entry.second = value;
            return;
        }
    }
    list.emplace_back(key, value);
}

} // namespace

std::optional<HttpEndpoint> parse_http_endpoint(std::string_view url, std::string& reason) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        reason = "not a URL of the form http://<host>[:<port>][/<path>]";
        return std::nullopt;
    }
    if (!equals_ignoring_case(url.substr(0, scheme_end), "http")) {
        reason = "the scheme is not http (https and others are not supported)";
        return std::nullopt;
    }
    if (url.find_first_of("?#") != std::string_view::npos) {
        reason = "a query or a fragment is not supported";
        return std::nullopt;
    }
    std::string_view rest = url.substr(scheme_end + 3);
    const auto path_start = rest.find('/');
    const std::string_view authority = rest.substr(0, path_start);
    const std::string_view path =
        path_start == std::string_view::npos ? std::string_view() : rest.substr(path_start);
    if (authority.find('@') != std::string_view::npos) {
        reason = "credentials in the URL are not supported";
        return std::nullopt;
    }
    HttpEndpoint endpoint;
    std::string_view port;
    bool has_port = false;
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos || !valid_ipv6(authority.substr(1, close - 1))) {
            reason = "the host is not an IPv6 address in brackets";
            return std::nullopt;
        }
        endpoint.host = std::string(authority.substr(1, close - 1));
        const std::string_view after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') {
                reason = "the IPv6 address is not followed by a port or a path";
                return std::nullopt;
            }
            has_port = true;
            port = after.substr(1);
        }
    } else {
        const auto colon = authority.find(':');
        if (colon != std::string_view::npos) {
            has_port = true;
            port = authority.substr(colon + 1);
        }
        const std::string_view host = authority.substr(0, colon);
        if (!valid_host_name(host)) {
            reason = host.empty() ? "the URL names no host"
                                  : "the host is not a name of letters, digits, '-' and '.', an "
                                    "IPv4 address or an IPv6 address in brackets";
            return std::nullopt;
        }
        endpoint.host = std::string(host);
    }
    if (has_port) {
        std::uint32_t number = 0;
        const bool digits =
            !port.empty() && port.size() <= 5 && std::all_of(port.begin(), port.end(), is_digit);
        if (digits) {
            for (const char c : port) {
                number = number * 10 + static_cast<std::uint32_t>(c - '0');
            }
        }
        if (!digits || number < 1 || number > 65535) {
            reason = "the port is not a number from 1 to 65535";
            return std::nullopt;
        }
        endpoint.port = static_cast<std::uint16_t>(number);
    }
    if (!std::all_of(path.begin(), path.end(), [](char c) { return c > 0x20 && c < 0x7f; })) {
        reason = "the path holds a space, a control character or a character beyond ASCII";
        return std::nullopt;
    }
    endpoint.path = path.empty() ? std::string("/") : std::string(path);
    endpoint.url = endpoint_url(endpoint);
    return endpoint;
}

HttpEndpoint append_signal_path(const HttpEndpoint& endpoint, std::string_view suffix) {
    HttpEndpoint result = endpoint;
    if (result.path.empty() || result.path.back() != '/') {
        result.path += '/';
    }
    result.path += suffix;
    result.url = endpoint_url(result);
    return result;
}

std::optional<HttpHeaders> parse_key_value_list(std::string_view text, bool header_names,
                                                std::string& reason) {
    HttpHeaders list;
    std::size_t position = 0;
    std::size_t entry = 0;
    while (position <= text.size()) {
        const auto comma = text.find(',', position);
        const std::string_view item = trimmed(text.substr(
            position, comma == std::string_view::npos ? text.size() - position : comma - position));
        position = comma == std::string_view::npos ? text.size() + 1 : comma + 1;
        if (item.empty()) {
            continue;
        }
        ++entry;
        const std::string where = "entry " + std::to_string(entry) + ": ";
        const auto equals = item.find('=');
        if (equals == std::string_view::npos) {
            reason = where + "it is not of the form key=value";
            return std::nullopt;
        }
        const std::string key(trimmed(item.substr(0, equals)));
        const std::string_view encoded = trimmed(item.substr(equals + 1));
        std::string value;
        for (std::size_t i = 0; i < encoded.size(); ++i) {
            if (encoded[i] != '%') {
                value += encoded[i];
                continue;
            }
            const int high = i + 2 < encoded.size() ? hex_value(encoded[i + 1]) : -1;
            const int low = high >= 0 ? hex_value(encoded[i + 2]) : -1;
            if (low < 0) {
                reason = where + "a '%' is not followed by two hexadecimal digits";
                return std::nullopt;
            }
            value += static_cast<char>(high * 16 + low);
            i += 2;
        }
        std::string entry_reason;
        if (!Resolver::valid_list_entry(key, value, header_names, entry_reason)) {
            reason = where + entry_reason;
            return std::nullopt;
        }
        list.emplace_back(key, std::move(value));
    }
    return list;
}

OtlpResolution resolve_otlp_settings(const TelemetryOptions& options,
                                     const EnvironmentLookup& environment) {
    OtlpResolution result;
    Resolver read(options, environment, result.ignored);
    OtlpSettings& settings = result.settings;

    if (const std::optional<std::string> disabled = read.variable("OTEL_SDK_DISABLED")) {
        if (equals_ignoring_case(trimmed(*disabled), "true")) {
            return result; // no endpoint: nothing is exported
        }
        if (!equals_ignoring_case(trimmed(*disabled), "false")) {
            read.ignore("OTEL_SDK_DISABLED",
                        "the value is neither true nor false; it counts as false");
        }
    }

    // Every source, each checked once, in a fixed order.
    const Read<HttpEndpoint> base = read.endpoint_variable("OTEL_EXPORTER_OTLP_ENDPOINT");
    const Read<bool> protocol = read.protocol_variable("OTEL_EXPORTER_OTLP_PROTOCOL");
    const std::optional<HttpHeaders> headers =
        read.list_variable("OTEL_EXPORTER_OTLP_HEADERS", true);
    const std::optional<std::chrono::milliseconds> timeout =
        read.milliseconds_variable("OTEL_EXPORTER_OTLP_TIMEOUT", 1);

    struct Signal {
        const char* exporter;
        const char* endpoint;
        const char* protocol;
        const char* headers;
        const char* timeout;
        const char* suffix;
        const char* option;
        const std::string* option_value;
        OtlpSignalSettings* settings;
    };
    const Signal signals[] = {
        {"OTEL_METRICS_EXPORTER", "OTEL_EXPORTER_OTLP_METRICS_ENDPOINT",
         "OTEL_EXPORTER_OTLP_METRICS_PROTOCOL", "OTEL_EXPORTER_OTLP_METRICS_HEADERS",
         "OTEL_EXPORTER_OTLP_METRICS_TIMEOUT", "v1/metrics", "TelemetryOptions::metrics_endpoint",
         &options.metrics_endpoint, &settings.metrics},
        {"OTEL_LOGS_EXPORTER", "OTEL_EXPORTER_OTLP_LOGS_ENDPOINT",
         "OTEL_EXPORTER_OTLP_LOGS_PROTOCOL", "OTEL_EXPORTER_OTLP_LOGS_HEADERS",
         "OTEL_EXPORTER_OTLP_LOGS_TIMEOUT", "v1/logs", "TelemetryOptions::logs_endpoint",
         &options.logs_endpoint, &settings.logs},
    };
    struct SignalRead {
        std::optional<bool> exporter;
        Read<HttpEndpoint> endpoint;
        Read<bool> protocol;
        std::optional<HttpHeaders> headers;
        std::optional<std::chrono::milliseconds> timeout;
    };
    std::array<SignalRead, 2> reads;
    for (std::size_t i = 0; i < reads.size(); ++i) {
        reads[i].exporter = read.exporter_variable(signals[i].exporter);
        reads[i].endpoint = read.endpoint_variable(signals[i].endpoint);
        reads[i].protocol = read.protocol_variable(signals[i].protocol);
        reads[i].headers = read.list_variable(signals[i].headers, true);
        reads[i].timeout = read.milliseconds_variable(signals[i].timeout, 1);
    }
    const std::optional<std::chrono::milliseconds> interval =
        read.milliseconds_variable("OTEL_METRIC_EXPORT_INTERVAL", kMinIntervalMs);
    const std::optional<std::string> service_name = read.variable("OTEL_SERVICE_NAME");
    const std::optional<HttpHeaders> resource =
        read.list_variable("OTEL_RESOURCE_ATTRIBUTES", false);

    std::array<Read<HttpEndpoint>, 2> option_endpoints;
    for (std::size_t i = 0; i < option_endpoints.size(); ++i) {
        option_endpoints[i] = read.endpoint_option(signals[i].option, *signals[i].option_value);
    }
    const std::optional<HttpHeaders> option_headers = read.headers_option();
    if (options.service_name.empty()) {
        read.ignore("TelemetryOptions::service_name", "the service name is empty");
    }
    const std::optional<HttpHeaders> option_resource = read.resource_option();
    const std::optional<std::chrono::milliseconds> option_interval = read.milliseconds_option(
        "TelemetryOptions::metric_export_interval_ms", options.metric_export_interval_ms,
        kMinIntervalMs, kDefaultIntervalMs);
    const std::optional<std::chrono::milliseconds> option_timeout = read.milliseconds_option(
        "TelemetryOptions::export_timeout_ms", options.export_timeout_ms, 1, kDefaultTimeoutMs);

    // Each signal's settings, from the first source that sets each.
    for (std::size_t i = 0; i < reads.size(); ++i) {
        const SignalRead& signal = reads[i];
        OtlpSignalSettings& out = *signals[i].settings;
        bool enabled = signal.exporter.value_or(true);
        const Read<bool>& used_protocol = signal.protocol.set ? signal.protocol : protocol;
        if (used_protocol.set && !used_protocol.value) {
            enabled = false;
        }
        std::optional<HttpEndpoint> endpoint;
        if (signal.endpoint.set) {
            endpoint = signal.endpoint.value;
        } else if (base.set) {
            if (base.value) {
                endpoint = append_signal_path(*base.value, signals[i].suffix);
            }
        } else {
            endpoint = option_endpoints[i].value;
        }
        if (enabled) {
            out.endpoint = std::move(endpoint);
        }
        if (signal.headers) {
            out.headers = *signal.headers;
        } else if (headers) {
            out.headers = *headers;
        } else if (option_headers) {
            out.headers = *option_headers;
        }
        out.timeout = signal.timeout   ? *signal.timeout
                      : timeout        ? *timeout
                      : option_timeout ? *option_timeout
                                       : std::chrono::milliseconds(kDefaultTimeoutMs);
    }
    settings.metric_export_interval = interval ? *interval
                                      : option_interval
                                          ? *option_interval
                                          : std::chrono::milliseconds(kDefaultIntervalMs);

    // The resource: the options' attributes, then the environment's; the service name folded out.
    if (option_resource) {
        for (const auto& [key, value] : *option_resource) {
            put(settings.resource_attributes, key, value);
        }
    }
    if (resource) {
        for (const auto& [key, value] : *resource) {
            put(settings.resource_attributes, key, value);
        }
    }
    settings.service_name =
        options.service_name.empty() ? kDefaultServiceName : options.service_name;
    const auto named =
        std::find_if(settings.resource_attributes.begin(), settings.resource_attributes.end(),
                     [](const auto& entry) { return entry.first == "service.name"; });
    if (named != settings.resource_attributes.end()) {
        if (!named->second.empty()) {
            settings.service_name = named->second;
        }
        settings.resource_attributes.erase(named);
    }
    if (service_name) {
        settings.service_name = *service_name;
    }
    return result;
}

OtlpResolution resolve_otlp_settings(const TelemetryOptions& options, bool use_environment) {
    if (!use_environment) {
        return resolve_otlp_settings(options, EnvironmentLookup());
    }
    const EnvironmentLookup environment = [](const char* name) {
        return environment_variable(name);
    };
    return resolve_otlp_settings(options, environment);
}

std::vector<LogAttribute> otlp_resource(const OtlpSettings& settings,
                                        const std::string& runtime_version, std::int64_t pid,
                                        const std::function<std::string()>& instance_id) {
    const auto configured = [&](const char* key) -> const std::string* {
        for (const auto& entry : settings.resource_attributes) {
            if (entry.first == key) {
                return &entry.second;
            }
        }
        return nullptr;
    };
    std::vector<LogAttribute> resource;
    resource.push_back({"service.name", settings.service_name});
    const std::string* version = configured("service.version");
    resource.push_back({"service.version", version != nullptr ? *version : runtime_version});
    const std::string* instance = configured("service.instance.id");
    resource.push_back({"service.instance.id", instance != nullptr ? *instance : instance_id()});
    const std::string* configured_pid = configured("process.pid");
    resource.push_back({"process.pid", configured_pid != nullptr ? nlohmann::json(*configured_pid)
                                                                 : nlohmann::json(pid)});
    for (const auto& [key, value] : settings.resource_attributes) {
        if (key == "service.name") {
            resource[0].value = value;
        } else if (key != "service.version" && key != "service.instance.id" &&
                   key != "process.pid") {
            resource.push_back({key, value});
        }
    }
    return resource;
}

std::string random_uuid_v4() {
    std::random_device device;
    std::array<unsigned char, 16> bytes{};
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        const std::uint32_t word = device();
        for (std::size_t j = 0; j < 4; ++j) {
            bytes[i + j] = static_cast<unsigned char>((word >> (8 * j)) & 0xffu);
        }
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fu) | 0x40u); // version 4
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fu) | 0x80u); // variant 10
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            text += '-';
        }
        text += kHex[bytes[i] >> 4];
        text += kHex[bytes[i] & 0x0fu];
    }
    return text;
}

} // namespace praenora::detail
