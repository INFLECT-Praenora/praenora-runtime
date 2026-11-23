# ADR-0043: OTLP export of metrics and logs

- Status: Accepted
- Date: 2026-11-22
- Supersedes: none
- Superseded by: none

## Context

The runtime's logs use the field names of the OpenTelemetry log data model and its metrics are
points named in OpenTelemetry style ([ADR-0015](ADR-0015-observability.md)), but nothing sends
them anywhere: an operator reads log files and polls `metrics_json()`. OQ-9 asks whether there
should be OTLP export. Operators collect telemetry with an OpenTelemetry collector, over OTLP, or
scrape Prometheus text.

The opentelemetry-cpp SDK is a large dependency for the few signals the runtime has. An export
must never change a run or hold up a tick, and a collector that is down or slow must not make a
run fail or look unhealthy. A collector merges records from many sources, where attribute keys
such as `step`, `module` or `event` would collide with other software's.

## Decision

Accepted by the maintainer on 2026-11-23; with
[ADR-0042](ADR-0042-control-and-status-api-over-http.md), it resolves OQ-9. Three choices were the
maintainer's, on 2026-11-22: the attribute prefix; that `logger` keeps its name and becomes the
OTLP scope name; and that a failed export never changes health, but is counted and logged at warn
level, at a limited rate (**Failures and health**). The builder notes are Proposed.

- **The attribute prefix.** Every attribute key that the runtime writes into a log event or a
  metric point carries the prefix `praenora.`, the rest of the key unchanged: in the metrics
  export (`Workflow::metrics_json()`, Python `Workflow.metrics()`) `praenora.step`,
  `praenora.module`, `praenora.outcome` and `praenora.workflow`; in the logs
  `praenora.run.fingerprint`, `praenora.event`, `praenora.tick`, `praenora.step`,
  `praenora.module`, `praenora.duration_ns`, `praenora.pid`, `praenora.provider`, `praenora.key`,
  `praenora.size`, `praenora.evicted`, `praenora.key_size`, `praenora.library_sha256` and every
  other key an event has. `logger` stays the reserved first attribute of the text and JSON formats
  and has no prefix; in OTLP it is the scope name instead of an attribute. Keys are written with
  the prefix where the runtime logs; `log_event()` adds nothing, so the records that a module host
  passes on keep their keys. Event names (`run.started`, `invocation.completed`, ...) and metric
  names (already `praenora.*`) do not change.
- **Placement and lifetime.** The exporter is part of the runtime library
  (`src/runtime/observability/otlp/`, namespace `praenora::detail`): the settings and the JSON
  encoders (pure functions), an HTTP client (the only runtime source that includes `httplib.h`),
  the exporter (one thread, `praenora:otlp`) and an spdlog sink. There is one exporter per
  `Runtime`, in its shared state, so copies of the `Runtime` share it; the `Runtime` constructor
  creates it when a signal has an endpoint. Workflows hold the state, so the exporter lives until
  the last copy and the last workflow are gone; its destructor flushes, one attempt per signal,
  bounded by the export timeout. A module host never creates one (it constructs no `Runtime`):
  the runtime's process exports the records a module host passes on, so every record of a run
  carries the runtime's `process.pid`.
- **Settings.** `struct TelemetryOptions` in `options.hpp`, `RuntimeOptions::telemetry`:
  `metrics_endpoint` and `logs_endpoint` (full URLs, such as `http://127.0.0.1:4318/v1/metrics`;
  empty: none), `headers` (sent with every export), `service_name` (default `praenora-runtime`),
  `resource_attributes`, `metric_export_interval_ms` (default 60000) and `export_timeout_ms`
  (default 10000). With `use_environment`, the `Runtime` constructor reads these once and puts
  them over the options:

  | Variable | Effect |
  |---|---|
  | `OTEL_SDK_DISABLED` | `true` (case-insensitive): no export at all. |
  | `OTEL_EXPORTER_OTLP_ENDPOINT` | A base URL: metrics to `<base>/v1/metrics`, logs to `<base>/v1/logs` (one `/` between). |
  | `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT`, `OTEL_EXPORTER_OTLP_LOGS_ENDPOINT` | That signal's URL, as it is. |
  | `OTEL_EXPORTER_OTLP_PROTOCOL`, `OTEL_EXPORTER_OTLP_METRICS_PROTOCOL`, `OTEL_EXPORTER_OTLP_LOGS_PROTOCOL` | `http/json` only. |
  | `OTEL_EXPORTER_OTLP_HEADERS`, `OTEL_EXPORTER_OTLP_METRICS_HEADERS`, `OTEL_EXPORTER_OTLP_LOGS_HEADERS` | `k=v,k=v`, values percent-decoded. |
  | `OTEL_EXPORTER_OTLP_TIMEOUT`, `OTEL_EXPORTER_OTLP_METRICS_TIMEOUT`, `OTEL_EXPORTER_OTLP_LOGS_TIMEOUT` | Milliseconds. |
  | `OTEL_METRICS_EXPORTER`, `OTEL_LOGS_EXPORTER` | `otlp` (the default) or `none`. |
  | `OTEL_METRIC_EXPORT_INTERVAL` | Milliseconds, at least 100. |
  | `OTEL_SERVICE_NAME`, `OTEL_RESOURCE_ATTRIBUTES` | The resource; `OTEL_SERVICE_NAME` wins over `service.name` in the attributes. |

  An endpoint must be `http://<host>[:<port>][/<path>]`. Unlike a `PRAENORA_*` variable, an
  `OTEL_*` value that is invalid or unsupported (another protocol, `https`, a malformed URL, an
  unknown exporter, a bad number) never throws: as OpenTelemetry's specification of its
  environment variables asks, it is ignored with a warning (event `telemetry.setting_ignored` on
  `praenora.runtime`, attribute `praenora.variable`), and a signal whose endpoint or protocol was
  refused exports nothing. The same holds for the values of `TelemetryOptions`. Without an
  endpoint nothing is exported; there is no default endpoint. Python and the CLI configure the
  export through the environment only; C++ also through the options.
- **Resource and identity.** The resource attributes are `service.name`, `service.version` (the
  runtime's version), `service.instance.id` (a random UUID v4 drawn once per `Runtime` from
  `std::random_device`, unless `OTEL_RESOURCE_ATTRIBUTES` or the options set one: the only random
  value the runtime draws outside the run seed, never in outputs, run records, snapshots or
  fingerprints), `process.pid`, then the configured attributes; no host name and no user name by
  default. The instrumentation scope of a log record is its logger's name (`praenora.engine`,
  `module.<step>`, ...), that of the metrics `praenora.runtime`, with the runtime's version. Every
  metric point and log record of a run carries `praenora.run.number` (the workflow's
  `run_number()`) and `praenora.run.fingerprint` besides its own attributes.
- **Metrics.** Every `metric_export_interval_ms`, the snapshot of each open workflow is encoded; a
  workflow is exported once more when it closes, and the logs are flushed then too.

  | Point of `metrics_json()` | OTLP |
  |---|---|
  | `praenora.step.invocations` counter | `sum`, `isMonotonic: true`, cumulative, `asInt` |
  | `praenora.step.invocation.duration` summary | `summary`: `count`, `sum`, `quantileValues` 0 (the minimum) and 1 (the maximum), left out when the count is 0 |
  | its `last` | `gauge` `praenora.step.invocation.last_duration`, unit `ns`, `asInt` |
  | `praenora.workflow.ticks` counter | `sum`, monotonic, cumulative |
  | `praenora.workflow.tick.duration`, `praenora.workflow.uptime` gauges | `gauge`, `asInt` |

  Cumulative points start at the run's `started_at` (its open time before it starts); every point
  is stamped with the snapshot's `generated_at`; units are those of `metrics_json()`. The
  exporter's own metrics (scope `praenora.runtime`, without run attributes) are
  `praenora.telemetry.exports.failed` (a counter, unit `{export}`, attribute `praenora.signal`,
  `metrics` or `logs`) and `praenora.telemetry.log_records.dropped` (a counter, unit `{record}`).
- **Logs.** An spdlog sink is added to the `Runtime`'s log hub and to every workflow's, so it
  receives whatever the hub's level lets through. It queues records (at most 2048; a record
  beyond that is dropped and counted), and the exporter sends them in batches of at most 512, at
  the latest 1 s after the first one was queued. A record has `timeUnixNano` and
  `observedTimeUnixNano` (the event's time), `severityNumber` and `severityText` (those of the
  JSON lines), `body.stringValue` (the message) and `attributes` (the event's, in their order,
  then the run's); JSON values map to `stringValue`, `intValue`, `doubleValue`, `boolValue`,
  `arrayValue` and `kvlistValue`, null to an empty value.
- **Encoding and transport.** OTLP/HTTP with the JSON encoding: a `POST` with
  `Content-Type: application/json`, lowerCamelCase field names, 64-bit integers (`timeUnixNano`,
  `asInt`, `count`, `intValue`, `bucketCounts`) as decimal strings, enumerations as integers
  (`aggregationTemporality` 2, `severityNumber`), canonical JSON (`dump()` without indentation).
  200 is success (a partial-success body is ignored). A connection failure, 429, 502, 503 and 504
  are retried: up to 3 attempts, 1 s and then 2 s apart, or after the `Retry-After` seconds when
  given (at most 30). Anything else, or the last failed attempt, drops the batch and counts a
  failure. While flushing at the end, one attempt and no wait. Export never holds up a tick: the
  sink only appends to the queue under a short lock, and snapshots are the existing short copies.
- **Failures and health** (chosen by the maintainer). A failed export never changes health or any
  snapshot. It is counted, and logged as event `telemetry.export_failed` (warn, on
  `praenora.runtime`, with `praenora.signal`, `praenora.failures` since the last such warning and
  `praenora.reason`): the first failure, then at most once a minute per signal.
- **Prometheus text.** `GET /metrics` of the control API
  ([ADR-0042](ADR-0042-control-and-status-api-over-http.md)) answers every run the server holds
  in the text exposition format 0.0.4 (`text/plain; version=0.0.4; charset=utf-8`):

  | Point | Prometheus |
  |---|---|
  | `praenora.step.invocations` | `praenora_step_invocations_total` counter |
  | `praenora.step.invocation.duration` | `praenora_step_invocation_duration_nanoseconds` summary: `{quantile="0"}` and `{quantile="1"}` (left out at count 0), `_sum`, `_count` |
  | its `last` | `praenora_step_invocation_last_duration_nanoseconds` gauge |
  | `praenora.workflow.ticks` | `praenora_workflow_ticks_total` counter |
  | `praenora.workflow.tick.duration` | `praenora_workflow_tick_duration_nanoseconds` gauge |
  | `praenora.workflow.uptime` | `praenora_workflow_uptime_nanoseconds` gauge |

  Labels are the point's attributes with `.` replaced by `_` (`praenora_step`, `praenora_module`,
  `praenora_outcome`, `praenora_workflow`), plus `praenora_run_number` and
  `praenora_run_fingerprint`; one `# HELP` and one `# TYPE` line per metric; values escaped (`\\`,
  `\"`, `\n`).
- **No SDK, no spans.** The exporter is the runtime's own; the opentelemetry-cpp SDK is not used.
  Tracing spans and trace context into modules are not part of this decision: they are OQ-33
  (and the ABI's structured logging OQ-24).
- **Determinism.** Outputs, the run record's reproducibility section, fingerprints and the message
  trace are byte-identical with and without an exporter.

### Builder notes (Proposed)

- The keys the runtime writes, all with the prefix: in the logs `praenora.run.fingerprint`,
  `praenora.event`, `praenora.tick`, `praenora.step`, `praenora.module`, `praenora.duration_ns`,
  `praenora.logical_time` (`tick.completed`), `praenora.workflow` (`run.started`),
  `praenora.code` (`run.failed`), `praenora.state` (`teardown.skipped`), `praenora.pid` (a module
  host's events), `praenora.provider`, `praenora.key`, `praenora.size`, `praenora.evicted`,
  `praenora.key_size` (the events of shared compiled models), `praenora.library_sha256`
  (`provider.loaded`), and `praenora.module_search_paths`, `praenora.provider_search_paths`,
  `praenora.modules`, `praenora.providers` and `praenora.catalog_entries` (`runtime.ready`); in the
  metrics export `praenora.step`, `praenora.module`, `praenora.outcome` and `praenora.workflow`.
  [docs/specs/observability.md](../specs/observability.md#logs) names each with its events.
- A provider's records that a module host passes on carry `praenora.provider` because the module
  host's own provider host writes it there, with the same code as the runtime's process; the
  runtime logs them with their keys as they come.
- The earlier records that name attribute keys without the prefix keep their text, and each has
  a Later note on this decision: [ADR-0004](ADR-0004-lifecycle-functions-and-documents.md),
  [ADR-0011](ADR-0011-tick-based-synchronous-execution.md),
  [ADR-0015](ADR-0015-observability.md), [ADR-0026](ADR-0026-runtime-service-providers.md),
  [ADR-0037](ADR-0037-process-isolation-of-module-instances.md) and
  [ADR-0038](ADR-0038-shared-compiled-models-between-module-hosts.md). The specifications name
  the keys with the prefix.
- The export is `src/runtime/observability/otlp/`: `otlp_settings` (`resolve_otlp_settings()`,
  pure over `TelemetryOptions` and an environment lookup), `otlp_json` (the encoders, pure),
  `http_client` (the interface `OtlpTransport`, which tests fake, and its HTTP implementation, the
  only runtime source that includes `httplib.h`), `exporter` (`OtlpExporter`) and `log_sink`. The
  exporter sits in `RuntimeContext::exporter`, the state that copies of a `Runtime` and its
  workflows share; white-box tests give it a fake transport through
  `set_otlp_transport_factory_for_testing()`.
- Settings: each setting comes from the first source that sets it. A refused endpoint or protocol
  leaves its signal without an export, without falling back to a lower source (a mistyped value
  must not send elsewhere); any other refused value falls back to the next source or the default.
  Header lists are not merged: a signal's variable replaces `OTEL_EXPORTER_OTLP_HEADERS`, which
  replaces `TelemetryOptions::headers`. The service name: `OTEL_SERVICE_NAME`, a `service.name` of
  `OTEL_RESOURCE_ATTRIBUTES`, one of `TelemetryOptions::resource_attributes` (which the
  environment's replace by key), then `TelemetryOptions::service_name`. `OTEL_SDK_DISABLED=true`
  reads no other variable; a value that is neither `true` nor `false` is ignored, as `false`.
  Enumerated values compare in any case; an empty variable is unset; a set and invalid value is
  reported also where a higher source shadows it. Timeouts are 1 to 2147483647 ms and the interval
  100 to 2147483647 ms. Endpoints refuse credentials, a query and a fragment as well as other
  schemes; header names must be HTTP tokens, and `Content-Type`, `Content-Length`, `Host`,
  `Transfer-Encoding` and `Connection` are refused, since the export sets them; decoded values
  hold no control character. Reasons never repeat a value (a header can hold a secret).
  `telemetry.setting_ignored` carries `praenora.reason` too, and is logged after the exporter's
  sink is on the `Runtime`'s hub, so it is exported as well.
- A configured resource attribute named `service.version`, `service.instance.id` or `process.pid`
  replaces that value in its place; the UUID is drawn only when none is configured.
- Transport: one connection per request, without keep-alive; the export timeout is the connect,
  read and write timeout and cpp-httplib's whole-request limit (`set_max_timeout()`), so a request
  ends after about the timeout. A host name is resolved by the system's resolver, which no timeout
  bounds (`HTTPLIB_USE_NON_BLOCKING_GETADDRINFO` is off). `Retry-After` is read as delay-seconds
  only (an HTTP date is ignored); only 200 succeeds, so a 500 is not retried. No write raises
  SIGPIPE, so that a failed export never changes a run in a process that keeps SIGPIPE at its
  default disposition (plain `praenora run`, `validate`, an application that embeds the runtime;
  only a process with cpp-httplib's server, or Python, ignores it): the client sets `SO_NOSIGPIPE`
  on its socket on macOS, and on other POSIX systems every `send()` passes `MSG_NOSIGNAL`
  (`CPPHTTPLIB_SEND_FLAGS`, a definition of `praenora_httplib`;
  [ADR-0042](ADR-0042-control-and-status-api-over-http.md) has the details). A collector that
  resets the connection while a request is written is then a connection failure, retried as one.
  `OtlpHttp.AResetConnectionFailsTheRequestNotTheProcess` (label `server`, POSIX) checks it in a
  child process with SIGPIPE at its default disposition.
- Exporter: `Workflow::close()` hands the final snapshot to the exporter, which sends it, with the
  queued records, on its own thread; `close()` does not wait. A stop during a wait between attempts
  ends the wait at once and puts the request back for the flush; a batch of records put back keeps
  the queue's limit, and the newest of it that do not fit are dropped and counted. The flush sends
  every queued record in one request (2048 at most), and the metrics of the finished runs and of
  those still added in another. The whole end is bounded by one export timeout, the largest of the
  signals', taken when the destructor starts: a request of the thread that fails while the exporter
  stops was its signal's attempt at the end and is not sent again (so a collector that never answers
  holds up the end by about one timeout, not one per signal or retry); each flush attempt gets the
  smaller of its signal's timeout and what is left, and none is made once nothing is left. A request
  not sent counts as a failed export. The request in flight is not aborted (cpp-httplib's
  `Client::stop()` could do it); it ends within its own timeout, which started before the end did.
  Nothing that fails on the exporter's thread ends it: such an export is dropped and counted. A
  metrics request goes only with at least one run's points; the exporter's own counters go with it,
  `praenora.telemetry.exports.failed` with a point per exported signal. A run is exported only once
  its number is set, which `Runtime::open()` does after the workflow exists; no record of a run is
  logged before that. The warning `telemetry.export_failed` names the failure in `praenora.reason`:
  `HTTP status <n>`; `no answer:` and the client's error; `not sent: the request in flight at the
  end failed` or `not sent: the end's export timeout is spent` (a request the end does not make); or
  `the request could not be made` (an error on the exporter's thread). It also names the failures
  since the last one of that signal; its interval starts at the warning. Points of `metrics_json()`
  map by name, as for the Prometheus text; others are left out.
- Log records: the run's attributes come after the event's and are not repeated when the event has
  the key already (the engine's events carry `praenora.run.fingerprint`); an unsigned value beyond
  a signed 64-bit integer is a `stringValue`; NaN and the infinities are the strings proto3's JSON
  mapping uses. The sink queues spdlog's payload as it is; the exporter's thread takes it apart
  with `decode_logged_event()`.
- The Prometheus text is `praenora::server::prometheus_text()` (`src/server/prometheus.cpp`),
  over each run's `metrics_json()` document with its run number and fingerprint. A metric family
  without samples is left out, so a server without runs answers an empty body; points of other
  names and points without the fields they map are left out. Label keys have every character but
  `A-Z a-z 0-9 _` replaced by `_` (the `.` of the prefixed keys and anything else a key could
  hold), in the order of `metrics_json()` (sorted), then `praenora_run_number`,
  `praenora_run_fingerprint` and `quantile`. The last duration's gauge is written at count 0 too;
  only the quantiles are left out. `# HELP` texts escape `\` and the line feed. The test
  `Prometheus.TextOfAFixedSnapshot` (label `unit`) holds the whole text of two runs.
- No test exports to a collector that the caller's environment names: every test runs with
  `OTEL_SDK_DISABLED=true` (the test presets, `praenora_add_gtest()` and `python.unittest` set
  it), and the tests of the settings set every variable they read.
- Tests: label `unit` (`OtlpSettings`, `OtlpEndpoint`, `OtlpResource`, `OtlpJson` against golden
  documents, `OtlpExporter` over a fake transport with short delays and the defaults checked as
  constants, `OtlpHttp.RetryAfterIsDelaySeconds`, `OtlpRuntime` with fixture runs over a fake
  transport: failures change no health or snapshot, and outputs, trace, fingerprint and
  reproducibility section are those of a run without the export); label `server` (`OtlpHttp`,
  `OtlpExport` and `TelemetryRun`, against a fake collector over real connections, and a
  listener that resets the connection); the Python
  `TelemetryTest.test_export_to_fake_collector`.

## Alternatives considered

- **The opentelemetry-cpp SDK.** It brings protobuf or gRPC, its own threads and a large build for
  two signals the runtime already shapes; a small exporter over OTLP/HTTP with JSON covers them.
- **OTLP over gRPC or with the protobuf encoding.** An OpenTelemetry collector accepts OTLP/HTTP
  with JSON, which needs neither protobuf nor gRPC; the others can be added later.
- **A failed export degrading health.** Health says how the run goes; a collector that is down
  says nothing about it, and a run's health would then depend on another service.
- **Keys without a prefix**, relying on the scope to tell the runtime's attributes apart. Many
  backends put the attributes of every source in one key space, where `step` or `module` would
  collide with other software's; OpenTelemetry's naming guidance recommends a namespace for
  attributes of one's own.
- **The prefix added by `log_event()`.** The records a module host passes on already carry their
  keys and would be prefixed twice, and keys would differ between what the code says and what the
  logs show.
- **Prometheus through a push gateway.** Scraping `GET /metrics` of a server needs no further
  service; a run without a server exports over OTLP.

## Consequences

- A collector receives the runtime's metrics and logs from C++, the CLI and Python, configured by
  the standard `OTEL_*` variables; Prometheus scrapes a server.
- Every consumer of the attribute keys reads the prefixed names; nothing outside this repository
  uses them yet.
- No TLS for the export in this version (`https` endpoints are refused with a warning); a local
  collector or a proxy forwards.
- `service.instance.id` is random per `Runtime`; it never reaches outputs, run records, snapshots
  or fingerprints.
- Open: tracing spans and trace context into modules (OQ-33); structured logging and metrics of
  modules through the ABI (OQ-24).

## References

- [ADR-0015](ADR-0015-observability.md): observability; it has a Later note on the attribute
  prefix, as have [ADR-0004](ADR-0004-lifecycle-functions-and-documents.md),
  [ADR-0011](ADR-0011-tick-based-synchronous-execution.md),
  [ADR-0026](ADR-0026-runtime-service-providers.md),
  [ADR-0037](ADR-0037-process-isolation-of-module-instances.md) and
  [ADR-0038](ADR-0038-shared-compiled-models-between-module-hosts.md)
- [ADR-0042](ADR-0042-control-and-status-api-over-http.md): the control and status API
- [docs/specs/observability.md](../specs/observability.md)
- [docs/open-questions.md](../open-questions.md) (OQ-9, OQ-24, OQ-33)
- [OTLP specification](https://opentelemetry.io/docs/specs/otlp/)
- [OpenTelemetry environment variable specification](https://opentelemetry.io/docs/specs/otel/configuration/sdk-environment-variables/)
- [OpenTelemetry attribute naming](https://opentelemetry.io/docs/specs/semconv/general/naming/)
- [OpenTelemetry logs data model](https://opentelemetry.io/docs/specs/otel/logs/data-model/)
- [Prometheus text exposition format](https://prometheus.io/docs/instrumenting/exposition_formats/)
