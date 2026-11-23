// Options of the C++ API: logging, OTLP export, the runtime, workflow runs, validation and graph
// requests. Paths are UTF-8 strings.
#ifndef PRAENORA_RUNTIME_OPTIONS_HPP
#define PRAENORA_RUNTIME_OPTIONS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace praenora {

enum class LogLevel { trace, debug, info, warn, error, critical, off };

// text: one human-readable line per event; json: one JSON object per line, with the field names
// of the OpenTelemetry log data model.
enum class LogFormat { text, json };

struct LogOptions {
    LogLevel level = LogLevel::info;
    LogFormat format = LogFormat::text;
    std::string file;      // also log to this file (appended to) when not empty
    bool to_stderr = true; // log to standard error
};

// The export of the runtime's metrics and logs to an OpenTelemetry collector over OTLP/HTTP with
// the JSON encoding (docs/specs/observability.md#otlp-export). Nothing is exported without an
// endpoint. With RuntimeOptions::use_environment, the standard OTEL_* variables are read once, when
// the Runtime is constructed, and win over these values. A value that is invalid or unsupported
// (an https endpoint, a timeout of 0, ...) never throws: it is ignored with a warning (event
// telemetry.setting_ignored), and a signal whose endpoint was refused exports nothing.
struct TelemetryOptions {
    std::string metrics_endpoint; // full URL, such as http://127.0.0.1:4318/v1/metrics; empty: none
    std::string logs_endpoint;    // full URL; empty: none
    std::vector<std::pair<std::string, std::string>> headers; // sent with every export
    std::string service_name = "praenora-runtime";
    std::vector<std::pair<std::string, std::string>> resource_attributes;
    std::int64_t metric_export_interval_ms = 60000;
    std::int64_t export_timeout_ms = 10000;
};

// Process-level settings; fixed once a Runtime is constructed.
struct RuntimeOptions {
    std::vector<std::string> module_paths, catalog_paths; // searched before configured paths
    // Searched for service providers first, before PRAENORA_PROVIDER_PATH and the installation's
    // provider directory.
    std::vector<std::string> provider_paths;
    // Honor PRAENORA_MODULE_PATH, PRAENORA_PROVIDER_PATH, PRAENORA_CATALOG_PATH,
    // PRAENORA_LOG_LEVEL, PRAENORA_LOG_FORMAT and the OTEL_* variables of the OTLP export
    // (PRAENORA_DEBUG_KEEP_LIBRARIES, a debugging aid, is read regardless).
    bool use_environment = true;
    LogOptions logging;
    // OTLP export of the metrics and logs of this Runtime and its workflows; none by default.
    TelemetryOptions telemetry;
};

// Where a step's module instance runs: in the runtime's process ("in-process", the default), or in
// a module host, a process of its own ("process"; docs/specs/process-isolation.md).
enum class IsolationMode { in_process, process };

// Per-run overrides of the workflow configuration; unset values keep the configuration's. Relative
// paths are relative to the current directory.
struct WorkflowOptions {
    std::optional<std::uint64_t> seed, ticks;
    std::optional<std::string> output_dir;
    bool overwrite = false; // allow an output directory that holds an earlier run
    std::optional<bool> trace_messages;
    // An empty status_file means no status file, in place of the configuration's.
    std::optional<std::string> status_file;
    // The run's logging, each setting on its own (see Runtime): these win over the environment,
    // the configuration and RuntimeOptions::logging. An empty log_file means no log file.
    std::optional<LogLevel> log_level;
    std::optional<LogFormat> log_format;
    std::optional<std::string> log_file;
    // Every step's isolation mode, in place of the configuration's (run.isolation and
    // steps.<id>.isolation). Like the other overrides, the run record lists it.
    std::optional<IsolationMode> isolation;
};

struct ValidateOptions {
    // Also load and start the service providers of the workflow's services (they stay loaded for
    // the rest of the process), and load the module libraries and check their identity: in this
    // process, or in a module host for a step that runs in one.
    bool load = false;
};

enum class GraphFormat { text, json, dot, asyncapi };

// Either `config_path` or `workflow` ("name@version") selects the workflow definition.
struct GraphRequest {
    std::optional<std::string> config_path, workflow;
    GraphFormat format = GraphFormat::text;
};

} // namespace praenora

#endif // PRAENORA_RUNTIME_OPTIONS_HPP
