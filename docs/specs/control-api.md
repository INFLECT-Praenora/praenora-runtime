# Control and status API

- **Version:** API 0.1.0 (paths under `/v0/`); runtime 0.2.0
- **Status:** Normative, and Accepted, all of it
  ([ADR-0042](../adr/ADR-0042-control-and-status-api-over-http.md); the Prometheus text with
  [ADR-0043](../adr/ADR-0043-otlp-export-of-metrics-and-logs.md)), which resolve OQ-9.
- **Requirement keywords:** MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as described in
  [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119).

The control API lets another process watch and stop a run over HTTP: its status snapshot, its
metrics, its run record and its latest outputs; in serve mode it also opens runs on request, only
from configurations an operator placed under a configuration directory, with run directories it
chooses itself. It is a front end over the public C++ API, beside the `praenora` tool and the
Python package: the runtime library holds no server, and an application that embeds it opens no
listener it did not ask for.

The front end is the static library `praenora_server` (`src/server/`, namespace
`praenora::server`; internal, never installed). Its logic is plain functions over plain request
and response types: `ControlApi::handle()` answers a request over a `RunSource`, the runs of one
mode. The `praenora` tool serves it (`praenora run --listen`, `praenora serve`) and reads it
(`praenora status --server`); this document specifies that interface. The API is not added to
`praenora version`.

## Terms

- **Control API**: this HTTP interface, version 0.1.0.
- **Listener**: an address the tool accepts connections on, a Unix domain socket or a TCP address.
- **Run mode**: `praenora run --listen`, which serves the one run of the process.
- **Serve mode**: `praenora serve`, a long-running process that opens runs on request.
- **Scope**: what a request may do: `read` (the queries) or `control` (opening, stopping and
  deleting runs).
- **Token**: a bearer token, read from a token file.
- **Configuration root** and **output root**: serve mode's directories, where the configurations
  that runs may be opened from lie, and where the runs write.
- **Run id**: the control API's name of a run: decimal, without leading zeros.
- **Run number**: a workflow's number in its `Runtime` (`Workflow::run_number()`), reported beside
  the run id.

## Security rules

A listener is reachable by the other users of the machine and, through a browser, by web pages.
These rules are what keeps both out.

### Listeners and credentials

- A Unix socket listener MUST be created with mode 0600. Every request through it has both scopes:
  the socket's mode is the credential. `--token-file` and `--read-token-file` with a Unix socket
  listener are a usage error.
- A TCP listener without tokens MUST listen on a loopback address (127.0.0.0/8 or `::1`); any
  other address is a usage error, and an IPv4-mapped address is refused ([Addresses](#addresses)).
  Every request through it has the `read` scope, and none has the `control` scope: a control route
  answers 403 `PERMISSION_DENIED`, saying to use a token or a Unix socket.
- A TCP listener with tokens: every request but `GET /v0/health` MUST carry exactly one
  `Authorization: Bearer <token>` header (the scheme in any case, one or more spaces, the token,
  trailing white space ignored). A request without one, or with a token that is neither of the
  listener's, answers 401 `UNAUTHENTICATED` with `WWW-Authenticate: Bearer`. The token of
  `--token-file` gives both scopes, the token of `--read-token-file` the `read` scope only; the
  read token on a control route answers 403 `PERMISSION_DENIED`. Tokens are compared in constant
  time, each of them on every request, so the time taken does not tell whether, or which, a token
  matched.
- Tokens are never taken from the command line or the environment, both of which other users of
  the machine can see.

### Token files

A token file holds its token on its first line, which is read without its trailing white space.
Any of these makes it a usage error (`INVALID_ARGUMENT`, exit 2) that names the file and never
its token:

- the file cannot be read, or is not a regular file;
- on POSIX, group or others may read or write it (its mode & 077 is not 0);
- the token is shorter than 32 characters;
- the token holds a character other than `A-Z a-z 0-9 - . _ ~ + / =` (a space, for example);
- the two files hold the same token: the read token and the control token MUST differ.

### The Host check

Against DNS rebinding, a request through a TCP listener MUST carry exactly one `Host` header whose
host is one of:

- the listener's own IP address, as `--listen` gave it (compared as an address, so
  `[0:0:0:0:0:0:0:1]` is `[::1]`);
- `localhost`, in any case, for a loopback listener only (also when `--allow-host` names it);
- a name given with `--allow-host`: a host name or an IPv4 address (letters, digits, `-` and `.`,
  neither first nor last a `.` or `-`, compared in any case), or an IPv6 address in brackets
  (`[2001:db8::1]`, compared as an address);

and whose port, when it gives one, is the listener's port. A request with no `Host`, several, or
another one answers 403 `PERMISSION_DENIED` with `details.reason` `host`, before its credentials
are looked at. Over a real connection cpp-httplib refuses an HTTP/1.1 request with no `Host`
header or several first, with 400 `INVALID_ARGUMENT` ([Listeners](#listeners)); an HTTP/1.0
request without one reaches the check. An `--allow-host` value of another form (with a port, for
example) is a usage error when the tool starts. A Unix socket listener has no Host check.

A wildcard or other non-loopback listener is no exception. One on `0.0.0.0:8080` accepts
`Host: 0.0.0.0:8080` and the `--allow-host` names, and nothing else: not `127.0.0.1`, not
`localhost`, not another address of the machine; one on `[::]:8080` accepts `[::]` and the
`--allow-host` names, not `[::1]`. Name with `--allow-host` every address and host name that its
clients connect by, for example `--allow-host 192.0.2.10 --allow-host [2001:db8::1]
--allow-host praenora.example.org`.

### Browsers

A web page that a browser shows MUST NOT be able to call a control route:

- Every `POST`, `POST /v0/runs/{id}/stop` included, MUST carry exactly one
  `Content-Type: application/json` (the media type in any case; a `charset` parameter, when there
  is one, `utf-8`); only `stop`'s body may be empty. Any other `POST` answers 415
  `INVALID_ARGUMENT`. A page cannot send that media type, nor a `DELETE`, without a preflight
  `OPTIONS` request first.
- Over TCP, the control scope needs `Authorization: Bearer <token>`, a header that a page cannot
  send without a preflight either; and a page cannot connect to a Unix socket.
- The API never sends a CORS header. It answers a preflight with an error (405, or 401 from a TCP
  listener with tokens, since a preflight carries no credentials), so the browser never sends the
  request that would follow it.
- A page served from a name that resolves to the listener (DNS rebinding) is refused by the Host
  check.

### What answers contain

- Status snapshots name the run's directories by placeholders (`PathForm::placeholders`, as the
  status file does; [observability.md](observability.md#known-directories-in-the-files-of-a-run)).
- Run entries name configurations and output directories relative to their roots, never by
  absolute paths, and so do the server's own error messages. Where the server's error carries the
  error that `Runtime::open()` threw (`details.cause` of the `IO_ERROR` that says it cannot tell
  whether an error lies in a run's output directory, [Serve mode](#serve-mode)), that cause is
  `open()`'s error document as thrown, like the 422 answer below: its message and location can
  name absolute paths.
- The 422 answer to a configuration that does not validate is the error document that
  `Runtime::open()` throws; its locations name the configuration's files as the runtime does.
- A handler that throws something other than a `praenora::Error` answers 500 `INTERNAL` without
  the exception's text.

### The order of the checks

`ControlApi::handle()` takes each request through these steps, and the first that refuses it
answers:

1. A TCP request whose `Host` fails the Host check: 403 `PERMISSION_DENIED` (`details.reason`
   `host`).
2. A request without valid credentials, but for `GET /v0/health`: 401 `UNAUTHENTICATED`.
3. A path that no route has: 404 `NOT_FOUND`.
4. A known path with another method: 405 `INVALID_ARGUMENT`, with `Allow` listing the path's
   methods (for example `GET, POST`). Methods are case-sensitive.
5. A request without the route's scope: 403 `PERMISSION_DENIED` (`details.reason` `scope`,
   `details.scope` the scope it needs).
6. In run mode, `POST /v0/runs` and `DELETE /v0/runs/{id}`: 404 `NOT_FOUND`.
7. A `POST` (`stop`'s included) without `Content-Type: application/json`: 415
   `INVALID_ARGUMENT`.
8. On a run's route, a run id that is not decimal without leading zeros: 404 `NOT_FOUND`.
9. A body that breaks its schema: 400 `INVALID_ARGUMENT`.
10. A run id that names no run: 404 `NOT_FOUND`; then the route's own answer, or the error it
    meets ([Errors](#errors-and-http-statuses)).

## Addresses

`--listen ADDR` takes one of:

| Form | Example | Notes |
|---|---|---|
| `unix:<path>` | `unix:/run/praenora/api.sock` | POSIX only; on Windows refused by the address parser with `UNSUPPORTED`, which the tool reports as a usage error (`INVALID_ARGUMENT`, exit 2). The path, in bytes, MUST fit the platform's `sun_path` (103 bytes on macOS, 107 on Linux); a longer one is a usage error that names the limit, never a socket somewhere else. |
| `<IPv4>:<port>` | `127.0.0.1:8080` | Dotted decimal, four parts from 0 to 255 without leading zeros. |
| `[<IPv6>]:<port>` | `[::1]:0` | The text forms of RFC 4291, without a zone. An IPv4-mapped address (`::ffff:0:0/96`, such as `[::ffff:127.0.0.1]:8080`) is refused, a usage error that names the IPv4 form to give instead (`127.0.0.1:8080`): an IPv6 listener takes IPv6 only, and the system binds no such address to it. |

The port is a decimal number from 0 to 65535; 0 lets the system choose. Host names, `localhost`
included, are refused: an address is never resolved. The tool writes addresses back in the same
syntax, IPv6 in the form of RFC 5952 (`[2001:db8::1]:443`), with the real port.

## Modes and command lines

These are the command lines of the `praenora` tool that serve the API; their flags are given
nowhere else.

### Both modes

- `--listen ADDR`: the listener ([Addresses](#addresses)).
- `--token-file F`, `--read-token-file F`: the tokens of a TCP listener
  ([Token files](#token-files)).
- `--allow-host H` (repeatable): a name the Host check accepts.
- `--address-file F`: once listening, the tool writes `<address>\n` to F, the address with the
  real port, before it answers a request. F is replaced atomically: the tool writes a file beside
  it (`F.tmp-<pid>`) and renames it over F, so a reader sees the old file or the whole new one.

When listening, the tool prints `praenora: listening on <address>` to standard error, before it
writes the address file and answers a request. The flags beside `--listen` without it are a usage
error. Every problem of the address (a host name, a path over the platform's limit,
`unix:` on Windows), of a token file, of the credentials (tokens with a Unix socket, none with an
address that is not a loopback address) and of `--allow-host` is a usage error (exit 2) that
carries the message of the check that refused it. A listener that the system refuses (an address
in use, a path that exists and is not a stale socket, a refused `bind()`) is `IO_ERROR` (exit 74),
naming the address and the system's error.

### Listeners

- **Sockets.** The tool makes the listening socket itself and hands it to cpp-httplib's server,
  which then accepts on it. A TCP listener binds the address as given (an IPv6 one for IPv6 only),
  with `SO_REUSEADDR` on POSIX and `SO_EXCLUSIVEADDRUSE` on Windows, never `SO_REUSEPORT`: no
  other process can share its port. Port 0 lets the system choose; the address then names the
  port chosen. Listening sockets are not inherited by the programs that the tool starts (module
  hosts).
- **Unix sockets.** A path that exists is refused unless it is a socket that nobody listens on (a
  connection to it is refused), which is removed first; a file of another kind, a symbolic link
  included (it is not followed), is refused and kept, and so is a socket that somebody listens
  on; so is one that a connection is refused by only now and then (a socket is stale only when
  three connections, 50 ms apart, are all refused: on macOS a full backlog refuses a connection
  too). The socket is bound, given mode 0600 and only then set to listen, so no client can connect
  before its mode is 0600. On macOS the file exists for the moment between `bind()` and
  `chmod()` (normally microseconds) with the mode the umask leaves of 0777, while it refuses every
  connection; on Linux the socket is given mode 0600 before `bind()`, which makes its file with
  that mode (less the umask). The tool removes its socket when it stops listening, unless the path
  names another file by then; a process that ends at once (a second signal, a crash) leaves it
  behind, and the next listener replaces it. A path that `sun_path` cannot hold is refused by the
  listener too, not only by the address parser.
- **Requests.** Every path and method reaches `ControlApi::handle()`, with the path decoded, the
  query as sent, header names in lower case and the listener's kind. What cpp-httplib refuses
  before (a malformed request line or header, an HTTP/1.1 request with no `Host` or several, a
  method it does not know, such as `TRACE`, a body over the limit) and a handler that throws
  answer the error document of an `Error` with their status: 413 and 400 are `INVALID_ARGUMENT`,
  500 `INTERNAL` without the exception's text. Every answer of status 400 or above closes its
  connection (cpp-httplib adds `Connection: close`), the route's own answers included; a request
  over the body limit is read up to the limit before its 413.

### Run mode

`praenora run <config.yaml> [...] --listen ADDR [--token-file F] [--read-token-file F]
[--allow-host H]... [--address-file F]`

The listener is opened first (so nothing is opened when it is refused), answers from before the
run starts, and closes when the run is over, its requests in flight answered; then the tool prints
its summary and exits as it does without a listener (with `--json`, the one document of the run).
SIGINT or SIGTERM stops the run, as without a listener. The one run of the process is served, as
id `1`; `POST /v0/runs` and `DELETE /v0/runs/{id}` answer 404. Its entry's `configuration` is the
configuration file's name, and its `output` is null.

### Serve mode

`praenora serve --listen ADDR --config-root D --output-root D [--token-file F]
[--read-token-file F] [--allow-host H]... [--address-file F] [--max-runs N] [--isolation M]`

- **Runs.** `POST /v0/runs` opens a run with one `Runtime` for the whole server (the global options
  make its `RuntimeOptions`, as for every command), and starts a thread of its own that calls
  `Workflow::run()`. Handlers only query runs and call `Workflow::request_stop()`.
- **`--config-root D`**: an existing directory, the only place runs are opened from
  ([Configuration names](#configuration-names)).
- **`--output-root D`**: created with its parents when missing. Run `k` writes to
  `<output-root>/run-<k>/` (`WorkflowOptions::output_dir`) and logs to `<output-root>/run-<k>.log`
  (`WorkflowOptions::log_file`, its level and format those of the global options); it keeps no
  status file (`WorkflowOptions::status_file` empty).
- **Run ids.** The run id is the decimal `k`. At startup `k` continues after the highest
  `run-<digits>` or `run-<digits>.log` entry of the output root (1 for an empty or new one; other
  entries, such as a file named `run-x`, are ignored); it grows with every request that reaches
  `Runtime::open()`, whether the open succeeds or not, so a refused validation leaves its number
  unused. A request refused before (its body, its configuration name, `--max-runs`, the forced
  isolation) takes none. One server per output root: a `run-<k>` directory that `Runtime::open()`
  refuses (one that holds an earlier run, or is not a directory) answers 409 `INVALID_STATE`,
  naming `run-<k>` and asking whether another server uses the output root; its number stays used.
  When where such an error lies cannot be told (a path that cannot be resolved), the answer is an
  `IO_ERROR` of its own (500), its details `{"output", "cause"}`.
- **`created_at`** is the wall-clock time the server opened the run.
- **`--max-runs N`** (default 16, at least 1): the runs held at once, running and finished, opens
  in progress included; a `POST` beyond it answers 503 `RESOURCE_EXHAUSTED`. `DELETE` frees a
  place.
- **`--isolation M`** (`in-process` or `process`) forces every run's isolation
  (`WorkflowOptions::isolation`, which the run record lists as an override); a request that asks
  for another answers 400 `INVALID_ARGUMENT`, and one that asks for the same one is accepted. In
  serve mode a crash in an in-process module ends every run of the server: `--isolation process`
  (POSIX) is RECOMMENDED.
- **`DELETE /v0/runs/{id}`** of a completed, stopped or failed run joins its thread, closes its
  workflow and forgets it; its files stay. While the run is in any other state it answers 409
  `INVALID_STATE`. A handler that found a run before keeps it alive until it has answered.
- **Shutdown.** SIGINT or SIGTERM (Ctrl+C on Windows) stops accepting connections and waits for
  the requests in flight to be answered (so no handler holds a run any more), then refuses new
  runs (409 `INVALID_STATE`), asks every run to stop, waits for each one's thread to end (a run in
  a call that never returns ends at its own deadline, or when its module host is killed), closes
  every run and exits 0, printing nothing on standard output (with `--json`,
  `{"ok": true, "result": {"address": "<address>"}}`). A second signal exits at once with 130.
  Queries that race the shutdown are answered or find the connection refused.

### Configuration names

The `configuration` of `POST /v0/runs` names a file under the configuration root:

- a relative path with `/` between its segments, none of them empty, `.` or `..`;
- no `\`, NUL or other control character (and no `:` on Windows);
- its path, with its symbolic links resolved as far as it exists, MUST lie under the
  configuration root's canonical path: a symbolic link that leaves the root is refused, one that
  stays inside it is followed;
- non-ASCII names are accepted.

The checks come in this order, and the first that fails answers:

1. A name that breaks one of the rules of its text: 400 `INVALID_ARGUMENT`.
2. A name whose path, its symbolic links resolved as far as it exists, does not lie under the
   root, or cannot be resolved (a loop of links, a directory that cannot be searched): 400
   `INVALID_ARGUMENT`, always with the message "it does not resolve to a path inside the
   configuration root". So a name beyond a link that leaves the root answers the same whether or
   not anything exists there, and no path that the client chooses outside the root can be probed.
   One residual stays: a symbolic link inside the root whose target is missing answers 404 for
   its own name, where an existing target outside the root answers 400, which tells only whether
   the target that the operator chose exists.
3. A file that does not exist inside the root: 404 `NOT_FOUND`.
4. Something that is not a regular file: 400 `INVALID_ARGUMENT`.

The messages name the configuration as the request gave it. The run is opened from the canonical
path.

## Routes

| Method, path | Scope | Answer |
|---|---|---|
| `GET /v0/health` | none | 200 `{"status": "ok"}` |
| `GET /v0/server` | read | 200 `{"api_version", "runtime_version", "mode", "pid", "started_at", "scopes"}`: `mode` `run` or `serve`, `started_at` when the server started, `scopes` this request's, sorted (`["control", "read"]` or `["read"]`) |
| `GET /v0/openapi.json` | read | 200 the OpenAPI document ([below](#the-openapi-document)), compact |
| `GET /v0/runs` | read | 200 `{"runs": [entry, ...]}` in id order |
| `POST /v0/runs` | control | serve mode: 201 the new run's entry, `Location: /v0/runs/<id>`; run mode: 404 |
| `GET /v0/runs/{id}` | read | 200 the status snapshot, `PathForm::placeholders` (`Workflow::status_json(PathForm::placeholders)`) |
| `GET /v0/runs/{id}/metrics` | read | 200 `Workflow::metrics_json()` |
| `GET /v0/runs/{id}/record` | read | 200 `Workflow::run_record_json()`, compact; 409 `INVALID_STATE` before it exists |
| `GET /v0/runs/{id}/outputs` | read | 200 `Workflow::latest_outputs_json()` |
| `POST /v0/runs/{id}/stop` | control | 202 the entry, after `Workflow::request_stop()` (idempotent: `stop` twice is 202 twice) |
| `DELETE /v0/runs/{id}` | control | serve mode: 204 for a finished run; 409 `INVALID_STATE` while it runs; run mode: 404 |
| `GET /metrics` | read | 200 the Prometheus text of every run ([below](#prometheus-text)) |

- A path no route has (`/v0/runs/` with its final `/` included) answers 404 `NOT_FOUND`; a run id
  that names no run, or is not decimal without leading zeros (`01`, `abc`), answers 404
  `NOT_FOUND`. No route reads the query string.
- JSON bodies are compact, `application/json; charset=utf-8`, bytes that are not UTF-8 replaced;
  errors are the error document of `Error::to_json()`. A 204 has no body and no `Content-Type`.

### The run entry

| Field | Content |
|---|---|
| `id` | The run id, a string. |
| `run_number` | The workflow's run number in its `Runtime`. |
| `configuration` | Run mode: the configuration file's name; serve mode: its name under the configuration root, with `/`, as the request gave it. |
| `workflow` | `{"name", "version"}` of the workflow definition. |
| `fingerprint` | The run fingerprint. |
| `state`, `health`, `ticks_completed` | From the status snapshot taken for the entry. |
| `created_at` | When the server opened the run, RFC 3339 UTC with nine fractional digits. |
| `output` | Serve mode: `run-<k>`, the run's directory under the output root; run mode: null. |

### The body of `POST /v0/runs`

A JSON object (UTF-8):

| Field | Content |
|---|---|
| `configuration` | Required: the configuration's name ([Configuration names](#configuration-names)). |
| `seed`, `ticks` | Integers from 0 to 2^53-1 (`-0` is 0); the run's overrides. |
| `trace_messages` | A boolean: trace every message to `trace/messages.jsonl`. |
| `isolation` | `"in-process"` or `"process"`: every step's isolation. |

A field of another name, a value of another type (`1.0` is not an integer), a value out of range
(2^53 included) and a body that is not a JSON object answer 400 `INVALID_ARGUMENT`, with the
field's JSON pointer as the error's `location.pointer`. A configuration that `Runtime::open()`
does not validate answers 422 with the thrown error document, its validation report in `details`.

### The body of `POST /v0/runs/{id}/stop`

Empty, or an empty JSON object `{}`, with `Content-Type: application/json` as every `POST`;
another body answers 400 `INVALID_ARGUMENT`, and another media type 415.

## Errors and HTTP statuses

The statuses that the routes and the checks above give win. Otherwise an error answers, in this
order:

1. an error whose details are a validation report (`{"ok", "diagnostics"}`, as `Runtime::open()`
   throws one): 422;
2. by code: `UNAUTHENTICATED` 401, `PERMISSION_DENIED` 403, `NOT_FOUND` 404, `INVALID_STATE`
   409, `RESOURCE_EXHAUSTED` 503;
3. by category: `usage` 400; `validation`, `compatibility`, `load` and `data` 422; `timeout` 504;
   `cancelled` 409; `execution`, `environment` and `internal` 500.

`UNAUTHENTICATED` (900) and `PERMISSION_DENIED` (901) are the control API's codes
([error-taxonomy.md](error-taxonomy.md)).

## The OpenAPI document

- Generated by `praenora::server::openapi_document()` from the route table: OpenAPI 3.1.0,
  `info.version` the API version, `0.1.0`; a security scheme `bearer` (`type: http`,
  `scheme: bearer`), which every operation may use but `GET /v0/health`, whose `security` is
  empty; for each route its `operationId`, `summary`, scope (`x-praenora-scope`: `none`, `read`
  or `control`), its `id` path parameter, its request body and its responses (the success, each
  error status it answers, and 500).
- `components.schemas` holds the API's own schemas (`Health`, `ServerInfo`, `RunEntry`,
  `RunList`, `RunRequest`, `StopRequest`, `Metrics`, `MetricPoint`, `Outputs`, `Error`,
  `OpenApiDocument`, `PrometheusText`), written to be valid in JSON Schema draft-07 and 2020-12
  alike (no `definitions`, no `dependencies`), and, verbatim with their `$schema` and `$id`, the
  runtime's embedded `status-snapshot`, `run-record` and `pmp-envelope` schemas, as
  `Runtime::schema()` gives them.
- It is committed as
  [`spec/openapi/praenora-control-api.openapi.json`](../../spec/openapi/praenora-control-api.openapi.json),
  with keys sorted, two-space indentation and a final newline, the layout of the AsyncAPI
  documents in `spec/asyncapi/`. It is refreshed only with `PRAENORA_UPDATE_GOLDEN=1`
  ([testing.md](../testing.md#golden-files)); `OpenApi.DocumentMatchesCommittedFile` compares it
  with the generated document byte for byte, and `OpenApi.EveryRouteIsDocumented` checks that it
  holds every route of the table and nothing else (label `protocol`).
- OpenAPI 3.1's schema dialect is JSON Schema 2020-12, and so is that of the runtime's schemas
  ([ADR-0046](../adr/ADR-0046-json-schema-dialects-and-validator.md)); each still names its
  dialect with `$schema`. Checking the document with official OpenAPI tooling is not yet
  verified.

## Prometheus text

`GET /metrics` answers the metric points of every run the server holds
(`Workflow::metrics_json()`) in the text exposition format 0.0.4,
`text/plain; version=0.0.4; charset=utf-8`:

| Point | Prometheus |
|---|---|
| `praenora.step.invocations` | `praenora_step_invocations_total`, a counter |
| `praenora.step.invocation.duration` | `praenora_step_invocation_duration_nanoseconds`, a summary: `{quantile="0"}` the minimum and `{quantile="1"}` the maximum (both left out at count 0), `_sum` and `_count` |
| its `last` | `praenora_step_invocation_last_duration_nanoseconds`, a gauge |
| `praenora.workflow.ticks` | `praenora_workflow_ticks_total`, a counter |
| `praenora.workflow.tick.duration` | `praenora_workflow_tick_duration_nanoseconds`, a gauge |
| `praenora.workflow.uptime` | `praenora_workflow_uptime_nanoseconds`, a gauge |

- The metrics come in this order, each with one `# HELP` and one `# TYPE` line and then the
  samples of every run, in id order; a metric without samples is left out, and a server without
  runs answers an empty body.
- A sample's labels are its point's attributes, each key with every character but `A-Z a-z 0-9 _`
  replaced by `_` (`praenora_step`, `praenora_module`, `praenora_outcome`, `praenora_workflow`),
  in the order of `metrics_json()` (sorted), then `praenora_run_number` and
  `praenora_run_fingerprint`, then the summary's `quantile`. Label values escape `\` as `\\`, `"`
  as `\"` and the line feed as `\n`. Every line ends in a line feed.
- Points of other names, and points that lack the fields they map, are left out.
- The same function, `praenora::server::prometheus_text()`, encodes the runs of a server and the
  fixed snapshots of the unit tests.

## Limits

| What | Limit |
|---|---|
| A request body | 64 KiB; a larger one answers 413. |
| Reading a request, writing an answer | 5 s each. |
| A connection kept alive | At most 100 requests and 5 s between them. |
| Requests answered at once | 8 worker threads; more connections wait for one. |
| Runs held by a server | `--max-runs`, 16 by default. |

## Determinism

No listener and no request changes a run: its outputs, the run record's reproducibility section,
its fingerprint and its message trace are byte-identical to those of the same configuration run
by `praenora run` (`Runtime::open()`, then `Workflow::run()`). Two runs of one configuration and
seed at once in serve mode have the same fingerprint and different run ids, run numbers and
`run-<k>` directories.

## The client

`praenora status --server ADDR [--run ID] [--token-file F]` reads the API: without `--run` it
prints the run list (`GET /v0/runs`, a table of id, workflow, state, health and ticks); with it,
the run's status (`GET /v0/runs/{id}`) as `praenora status <file>` prints a status file; `--json`
prints the documents. The server's errors are reported with their own codes and exit codes (from
their error documents); a connection that fails, or an answer that is no error document, is
`IO_ERROR` (exit 74).

- `ADDR` takes the syntax of `--listen`; problems with it are usage errors. Over TCP the client
  connects to the IP address (never resolved) and sends it as the `Host` header; over a Unix
  socket, `Host: localhost`.
- `--token-file F` is read with the rules of the server's token files and sent as
  `Authorization: Bearer <token>`; with a Unix socket it is a usage error, as on the server.
- `--run ID` takes a run id, decimal without leading zeros; another value is a usage error.
  `--run` and `--token-file` without `--server`, and a status file with it, are usage errors.
- The client waits at most 5 s to connect and 10 s for an answer.

## Platforms

Unix socket listeners are POSIX only: on Windows `unix:<path>` is refused by the address parser
with `UNSUPPORTED`, which the tool reports as a usage error (`INVALID_ARGUMENT`, exit 2), and TCP
listeners work; Ctrl+C, Ctrl+Break and closing the console shut `praenora serve` down there.
Token files' modes are checked on POSIX only. Serve mode's `--isolation process` needs the module
host, which Windows does not have in this version
([process-isolation.md](process-isolation.md#platforms)). CI built and tested the listeners and the
client on Linux and Windows (run 45165537866).

## Not in this version

Pushed status updates (server-sent events, long polling), pausing and resuming, uploaded
configurations, remote tick-by-tick control, TLS inside the process (a reverse proxy terminates
it), a server in the Python package, and Unix sockets on Windows.

## References

- [ADR-0042](../adr/ADR-0042-control-and-status-api-over-http.md): the control and status API
  over HTTP.
- [ADR-0043](../adr/ADR-0043-otlp-export-of-metrics-and-logs.md): OTLP export of metrics and
  logs, and the Prometheus text.
- [observability.md](observability.md): logs, metrics, status snapshots and the status file.
- [error-taxonomy.md](error-taxonomy.md): the error codes, 900 and 901 included.
- [process-isolation.md](process-isolation.md): module hosts, which `--isolation process` uses.
- [open-questions.md](../open-questions.md): OQ-2 (JSON Schema dialects), OQ-9 (a remote control
  and status API).
- [OpenAPI Specification 3.1.0](https://spec.openapis.org/oas/v3.1.0),
  [RFC 6750](https://www.rfc-editor.org/rfc/rfc6750) (bearer tokens),
  [RFC 3339](https://www.rfc-editor.org/rfc/rfc3339),
  [RFC 4291](https://www.rfc-editor.org/rfc/rfc4291) and
  [RFC 5952](https://www.rfc-editor.org/rfc/rfc5952) (IPv6 text),
  [the Prometheus text exposition format](https://prometheus.io/docs/instrumenting/exposition_formats/).
