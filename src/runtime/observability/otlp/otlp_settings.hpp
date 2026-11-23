// The settings of the OTLP export (docs/specs/observability.md#otlp-export): TelemetryOptions with
// the standard OTEL_* variables put over them, each value checked. Pure functions; nothing here
// logs or connects. A value that is invalid or unsupported is never an error: it is ignored, and
// listed (IgnoredSetting) for the Runtime to log as event telemetry.setting_ignored.
#ifndef PRAENORA_RUNTIME_OBSERVABILITY_OTLP_OTLP_SETTINGS_HPP
#define PRAENORA_RUNTIME_OBSERVABILITY_OTLP_OTLP_SETTINGS_HPP

#include "observability/logging.hpp"

#include <praenora/runtime/options.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace praenora::detail {

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

// An endpoint of the export: http://<host>[:<port>][/<path>].
struct HttpEndpoint {
    std::string host;        // a name, an IPv4 address, or an IPv6 address without its brackets
    std::uint16_t port = 80; // 80 when the URL names none
    std::string path = "/";  // starts with '/'
    std::string url;         // http://<host>[:<port>]<path>, IPv6 addresses in brackets
};

// The endpoint that `url` names: the scheme `http` (in any case); a host that is a name of
// letters, digits, '-' and '.', an IPv4 address or an IPv6 address in brackets; an optional port
// from 1 to 65535; and an optional path of printable ASCII characters without spaces. Refused,
// with nullopt and a reason in `reason` that does not repeat the value (it may hold a secret): any
// other scheme (https included), credentials (`user@`), a query or a fragment, an empty host, and
// anything else.
std::optional<HttpEndpoint> parse_http_endpoint(std::string_view url, std::string& reason);

// `endpoint` with `suffix` ("v1/metrics", "v1/logs") appended to its path, one '/' between: the
// endpoint of a signal under the base endpoint OTEL_EXPORTER_OTLP_ENDPOINT.
HttpEndpoint append_signal_path(const HttpEndpoint& endpoint, std::string_view suffix);

// A `k=v,k=v` list (OTEL_EXPORTER_OTLP_HEADERS, OTEL_RESOURCE_ATTRIBUTES): entries separated by
// ',', empty ones skipped; around each key and value, spaces and tabs are trimmed; values are
// percent-decoded. A key must be a token of HTTP (letters, digits and !#$%&'*+-.^_`|~) when
// `header_names`, and otherwise any text without '=' and ',' that is not empty; a decoded value
// must hold no control character. With `header_names`, the names Content-Type, Content-Length,
// Host, Transfer-Encoding and Connection (in any case), which the export sets itself, are refused.
// Returns nullopt, with a reason that names no value, when any entry is invalid.
std::optional<HttpHeaders> parse_key_value_list(std::string_view text, bool header_names,
                                                std::string& reason);

// The export of one signal (metrics or logs).
struct OtlpSignalSettings {
    std::optional<HttpEndpoint> endpoint; // none: the signal exports nothing
    HttpHeaders headers;                  // sent with every export of the signal
    std::chrono::milliseconds timeout{10000};
};

struct OtlpSettings {
    OtlpSignalSettings metrics, logs;
    std::chrono::milliseconds metric_export_interval{60000};
    std::string service_name = "praenora-runtime";
    // The configured resource attributes: TelemetryOptions::resource_attributes, then those of
    // OTEL_RESOURCE_ATTRIBUTES, which replace the values of keys already there.
    HttpHeaders resource_attributes;

    bool enabled() const noexcept { return metrics.endpoint.has_value() || logs.endpoint; }
};

// A value that was ignored: the variable (OTEL_EXPORTER_OTLP_ENDPOINT, ...) or option
// (TelemetryOptions::metrics_endpoint, ...) that held it, and why. The reason never repeats the
// value.
struct IgnoredSetting {
    std::string variable;
    std::string reason;
};

struct OtlpResolution {
    OtlpSettings settings;
    std::vector<IgnoredSetting> ignored; // in the order the values were read
};

// The value of an environment variable, nullopt when it is not set; an empty value counts as not
// set.
using EnvironmentLookup = std::function<std::optional<std::string>(const char* name)>;

// The export's settings from `options`, with the variables that `environment` gives put over them
// (none when `environment` is empty), each setting on its own, from the first source that sets it
// and is valid:
// - OTEL_SDK_DISABLED "true" (in any case): nothing is exported, and nothing else is read; a value
//   other than "true" or "false" is ignored (as "false").
// - Per signal (METRICS, LOGS): OTEL_<SIGNAL>_EXPORTER "none" exports nothing, "otlp" (the
//   default) exports, anything else is ignored. The protocol, from
//   OTEL_EXPORTER_OTLP_<SIGNAL>_PROTOCOL or else OTEL_EXPORTER_OTLP_PROTOCOL, must be "http/json";
//   another one is ignored, and the signal it applies to exports nothing. The endpoint comes from
//   OTEL_EXPORTER_OTLP_<SIGNAL>_ENDPOINT as it is, else from OTEL_EXPORTER_OTLP_ENDPOINT with
//   "v1/metrics" or "v1/logs" appended to its path, else from TelemetryOptions; the first that is
//   set decides, and when it is refused the signal exports nothing. Headers come from
//   OTEL_EXPORTER_OTLP_<SIGNAL>_HEADERS, else OTEL_EXPORTER_OTLP_HEADERS, else
//   TelemetryOptions::headers, whole lists (an invalid one is ignored and the next source used);
//   the timeout (milliseconds, 1 to 2147483647) from OTEL_EXPORTER_OTLP_<SIGNAL>_TIMEOUT, else
//   OTEL_EXPORTER_OTLP_TIMEOUT, else TelemetryOptions::export_timeout_ms, else 10000.
// - OTEL_METRIC_EXPORT_INTERVAL, else TelemetryOptions::metric_export_interval_ms (milliseconds,
//   100 to 2147483647), else 60000.
// - OTEL_SERVICE_NAME, else a service.name of OTEL_RESOURCE_ATTRIBUTES, else one of
//   TelemetryOptions::resource_attributes, else TelemetryOptions::service_name (not empty), else
//   "praenora-runtime".
// Every value that is set and invalid is listed in `ignored`, also one that a higher source
// shadows. Enumerated values ("true", "otlp", "none", "http/json") are compared in any case.
OtlpResolution resolve_otlp_settings(const TelemetryOptions& options,
                                     const EnvironmentLookup& environment);

// The same, reading the process environment when `use_environment` is set (the Runtime's
// RuntimeOptions::use_environment), and no environment otherwise.
OtlpResolution resolve_otlp_settings(const TelemetryOptions& options, bool use_environment);

// The resource of the export, in this order: service.name (settings.service_name),
// service.version (`runtime_version`), service.instance.id (`instance_id()`, called only when no
// configured attribute sets it), process.pid (`pid`, an integer), then the other configured
// attributes of `settings`, in their order; a configured attribute with one of the first four keys
// replaces that value in its place. Values are strings but for process.pid.
std::vector<LogAttribute> otlp_resource(const OtlpSettings& settings,
                                        const std::string& runtime_version, std::int64_t pid,
                                        const std::function<std::string()>& instance_id);

// A random UUID of version 4 (RFC 9562), 36 characters in lower case, drawn from
// std::random_device.
std::string random_uuid_v4();

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_OBSERVABILITY_OTLP_OTLP_SETTINGS_HPP
