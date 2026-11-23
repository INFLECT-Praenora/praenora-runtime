# ADR-0042: Control and status API over HTTP

- Status: Accepted
- Date: 2026-11-22
- Supersedes: none
- Superseded by: none

## Context

The in-process API and an optional status file say what a run is doing
([ADR-0015](ADR-0015-observability.md)); nothing lets another process stop it, read its metrics,
its run record or its latest outputs, or open a run. OQ-9 asks whether there should be a remote
control and status API. An operator who runs the runtime as a long-running service needs one:
watching and stopping a run from another process, and opening runs on request from configurations
that the operator placed on the machine, with run directories the service chooses.

The runtime library must stay free of a server: an application that embeds it opens no listener
it did not ask for, and the library's dependencies do not grow. Whatever serves the API must not
change a run: outputs, the run record's reproducibility section, the fingerprint and the message
trace are the same with and without it. A listener on a machine is reachable by other users and,
through a browser, by web pages, so access needs credentials and a check of the `Host` header.

## Decision

Accepted by the maintainer on 2026-11-23; with
[ADR-0043](ADR-0043-otlp-export-of-metrics-and-logs.md), it resolves OQ-9. On 2026-11-22 the
maintainer chose to build a long-running mode now and to allow listeners in tests under written
rules (the last bullet). The builder notes are Proposed.

- **A third front end.** The API is served by a static library `praenora_server` (namespace
  `praenora::server`, directory `src/server/`), beside the CLI and the Python package. It is
  internal, never installed and linked by the `praenora` tool only. It uses only the public C++
  API, `praenora_frontend`, nlohmann/json and cpp-httplib; only its listener and client sources
  include `httplib.h`. Its logic is plain functions over plain types, testable without sockets: a
  `Request` (method, path, query, lower-cased headers, body, and the listener's kind, `unix` or
  `tcp`), a `Response` (status, content type, headers, body) and
  `ControlApi::handle(const Request&) -> Response` over an interface `RunSource` (list, find,
  create, stop and delete runs), with a real `RunRegistry` and fakes in tests.
- **Version.** The API is `0.1.0` (the OpenAPI document's `info.version`, and `api_version` of
  `GET /v0/server`), its paths under `/v0/`. It is not added to `praenora version`.
- **Run mode.** `praenora run <config.yaml> [...] --listen ADDR [--token-file F]
  [--read-token-file F] [--allow-host H]... [--address-file F]` serves the one run of the process
  (id `1`). The listener is up before the run starts and closes when the run is over; the tool
  then prints its summary and exits as it does without a listener. `POST /v0/runs` and
  `DELETE /v0/runs/{id}` answer 404.
- **Serve mode.** `praenora serve --listen ADDR --config-root D --output-root D [--token-file F]
  [--read-token-file F] [--allow-host H]... [--address-file F] [--max-runs N] [--isolation M]`
  opens runs on `POST /v0/runs` with one `Runtime`, whose `RuntimeOptions` come from the global
  options as for every command; each run is driven on a thread of its own that calls
  `Workflow::run()`, and handlers only query and call `request_stop()`. `--max-runs N` (default
  16, at least 1) bounds the runs held at once, running and finished; a `POST` beyond it answers
  503 `RESOURCE_EXHAUSTED`. `--isolation M` forces every run's isolation; a request that asks for
  another answers 400. Run `k` writes to `<output-root>/run-<k>/` and logs to
  `<output-root>/run-<k>.log`, with no status file; at startup `k` continues after the highest
  `run-<digits>` entry of the output root, which is created with its parents. One server per
  output root; a directory that `Runtime::open()` refuses answers 409 `INVALID_STATE`. The run id
  is the decimal `k`; the run's `run_number` is reported beside it. SIGINT or SIGTERM (Ctrl+C on
  Windows) stops accepting connections, asks every run to stop, waits for every run to end,
  closes them and exits 0; a second signal exits at once with 130.
- **Addresses.** `ADDR` is `unix:<path>` (POSIX only: on Windows the address parser refuses it
  with `UNSUPPORTED`, which the tool reports as a usage error, `INVALID_ARGUMENT` with exit code
  2), `<IPv4>:<port>` or `[<IPv6>]:<port>`; port 0 lets the system choose; host names are refused
  (a usage error too). The tool prints `praenora: listening on <address>` to standard error, with
  the real port, and with `--address-file F` writes `<address>\n` to F (replaced atomically)
  before it accepts a request.
  A Unix socket is created with mode 0600; a path that exists is refused unless it is a socket
  that nobody listens on, which is removed first; the tool removes its socket when it ends.
  Limits: request bodies of at most 64 KiB (413), read and write timeouts of 5 s, keep-alive for
  at most 100 requests and 5 s, 8 worker threads.
- **Scopes and tokens.** Two scopes: `read` and `control`. `--token-file F` gives a token of both
  scopes, `--read-token-file F` one of `read` only: the file's first line without trailing white
  space, at least 32 characters of `A-Z a-z 0-9 - . _ ~ + / =`, in a file that on POSIX neither
  group nor others may read or write; the two tokens must differ, and tokens are never taken from
  the command line or the environment. Any failure is a usage error naming the file. Over a Unix
  socket every connection has both scopes (the socket's mode is the credential), and tokens are a
  usage error. Over TCP without tokens only loopback addresses may listen; every request has the
  `read` scope and none the `control` scope (403 `PERMISSION_DENIED`, saying to use a token or a
  Unix socket). Over TCP with tokens, every request but `GET /v0/health` needs
  `Authorization: Bearer <token>`: none or an unknown one answers 401 `UNAUTHENTICATED` with
  `WWW-Authenticate: Bearer`, the read token on a control route 403 `PERMISSION_DENIED`. Tokens
  are compared in constant time.
- **Host check** (TCP only, against DNS rebinding). The `Host` header must be present, its host
  (case-insensitive) the listener's address, `localhost` (loopback listeners only) or an
  `--allow-host` name, and its port, when given, the listener's; otherwise 403
  `PERMISSION_DENIED` with `details.reason` `host`. Every `POST` needs
  `Content-Type: application/json` (an empty body is allowed for `stop`), else 415
  `INVALID_ARGUMENT`; no CORS header is ever sent, so a web page cannot call the API.
- **Routes.**

  | Method, path | Scope | Answer |
  |---|---|---|
  | `GET /v0/health` | none | 200 `{"status": "ok"}` |
  | `GET /v0/server` | read | 200 `api_version`, `runtime_version`, `mode` (`run`, `serve`), `pid`, `started_at`, `scopes` (this request's, sorted) |
  | `GET /v0/openapi.json` | read | 200 the OpenAPI document |
  | `GET /v0/runs` | read | 200 `{"runs": [entry, ...]}` in id order |
  | `POST /v0/runs` | control | serve mode: 201 the new run's entry, `Location: /v0/runs/<id>`; run mode: 404 |
  | `GET /v0/runs/{id}` | read | 200 the run's status snapshot, its error records naming the run's directories by placeholders |
  | `GET /v0/runs/{id}/metrics` | read | 200 `Workflow::metrics_json()` |
  | `GET /v0/runs/{id}/record` | read | 200 `Workflow::run_record_json()`; 409 `INVALID_STATE` before it exists |
  | `GET /v0/runs/{id}/outputs` | read | 200 `Workflow::latest_outputs_json()` |
  | `POST /v0/runs/{id}/stop` | control | 202 the entry, after `request_stop()` (idempotent) |
  | `DELETE /v0/runs/{id}` | control | serve mode: 204 for a finished run (its thread joined, its workflow closed, its files kept), 409 `INVALID_STATE` while it runs; run mode: 404 |
  | `GET /metrics` | read | 200 the Prometheus text of every run ([ADR-0043](ADR-0043-otlp-export-of-metrics-and-logs.md)) |

  A run entry has `id` (a string), `run_number`, `configuration` (run mode: the configuration
  file's name; serve mode: its name under the configuration root, with `/`), `workflow`
  (`{"name", "version"}`), `fingerprint`, `state`, `health`, `ticks_completed`, `created_at`
  (RFC 3339) and `output` (serve mode: `run-<k>`; run mode: null). The body of `POST /v0/runs` has
  `configuration` (required: a relative path with `/` and no empty, `.` or `..` segment, whose
  canonical path lies under the configuration root's; a missing file is 404 `NOT_FOUND`), `seed`
  and `ticks` (integers from 0 to 2^53-1), `trace_messages` (a boolean) and `isolation`
  (`"in-process"` or `"process"`); an unknown field, a wrong type or another value is 400
  `INVALID_ARGUMENT`, and a validation failure of `Runtime::open()` answers 422 with the thrown
  error document. An unknown path is 404 `NOT_FOUND`, a known path with another method 405
  `INVALID_ARGUMENT` with `Allow`, and an unknown run id (or one that is not decimal) 404
  `NOT_FOUND`. JSON bodies are compact, `application/json; charset=utf-8`; errors are the error
  document of `Error::to_json()`.
- **Public API additions**, in the current versions: `enum class PathForm { absolute,
  placeholders }` and `Workflow::status_json(PathForm)` (the existing `status_json()` is
  `status_json(PathForm::absolute)`; `placeholders` gives the status file's form, through the
  code the status-file writer uses); `Workflow::run_number()`, the workflow's number in its
  `Runtime` (1 for the first workflow `Runtime::open()` returned, then 2, 3, ..., shared by every
  copy of the `Runtime`, assigned only when `open()` succeeds); an empty
  `WorkflowOptions::status_file` means no status file (the CLI's `--status-file ""` and Python's
  `status_file=""` follow); Python's `Workflow.status(path_form="absolute")` and
  `Workflow.run_number`.
- **Error codes.** A component range `9xx` for the control API, with `UNAUTHENTICATED = 900` ("The
  request carries no valid credentials.") and `PERMISSION_DENIED = 901` ("The credentials do not
  allow the operation."), both of category `usage`, exit code 2, owner the caller and not
  retryable. They are runtime codes, not in `status.h`. `IO_ERROR`'s summary becomes "Reading or
  writing a file or a connection failed."
- **HTTP statuses.** The statuses above win. Otherwise by code, then by category:
  `UNAUTHENTICATED` 401, `PERMISSION_DENIED` 403, `NOT_FOUND` 404, `INVALID_STATE` 409,
  `RESOURCE_EXHAUSTED` 503; category `usage` 400; `validation`, `compatibility`, `load` and `data`
  422; `timeout` 504; `cancelled` 409; `execution`, `environment` and `internal` 500. A handler
  that throws something other than `praenora::Error` answers 500 `INTERNAL` without the
  exception's text.
- **The OpenAPI document** is generated by `praenora::server::openapi_document()` from the route
  table (method, path, operation id, scope, summary, request and response schemas): OpenAPI 3.1.0,
  `info.version` `0.1.0`, a security scheme `bearer` (`type: http`, `scheme: bearer`), each
  operation's scope in `x-praenora-scope`. `components.schemas` holds the API's own schemas,
  written to be valid in draft-07 and in 2020-12 (no `definitions`, no `dependencies`), and,
  verbatim with their `$schema` and `$id`, the runtime's embedded `status-snapshot`, `run-record`
  and `pmp-envelope` schemas, as `Runtime::schema()` gives them. It is committed as
  `spec/openapi/praenora-control-api.openapi.json` and refreshed only with
  `PRAENORA_UPDATE_GOLDEN=1`; a test of the `protocol` label compares it with the generated
  document, and another checks that it holds every route of the table and nothing else. Checking
  it with official OpenAPI tooling is not yet verified (the runtime's schemas are draft-07, OQ-2).
- **The client.** `praenora status --server ADDR [--run ID] [--token-file F]` prints the run list
  (a table of id, workflow, state, health and ticks) or, with `--run`, the run's status as
  `praenora status <file>` prints it; `--json` prints the documents. The server's errors are
  reported with their own codes and exit codes; a connection that fails is `IO_ERROR` (exit 74).
- **cpp-httplib** carries HTTP: header-only, MIT, without TLS (a reverse proxy terminates TLS).
  `find_package(httplib 0.67 CONFIG)`: the floor is 0.67, the only version built and tested. With
  `PRAENORA_FETCH_DEPENDENCIES`, FetchContent at the pinned commit
  `7800c3dc5b33285cbb18baa5998de4ff7ef63515`, the tag `v0.67.0` (never the upstream tag `latest`,
  which moves), its options for TLS, compression, compiling, tests and installation off before it is
  made available. An internal INTERFACE target `praenora_httplib` carries only the found or fetched
  target's include directory (as `SYSTEM`), `ws2_32` on Windows and, on POSIX systems other than
  macOS, `CPPHTTPLIB_SEND_FLAGS=MSG_NOSIGNAL` (below); the build defines no
  `CPPHTTPLIB_*_SUPPORT`, and a found package whose target is not header-only is refused at
  configuration with a message that says to use fetch mode or a header-only install. The CI presets
  fetch it like yaml-cpp, spdlog and GoogleTest; it is not added to the praenora-runtime-deps
  bundles. Its license is installed when it is built in the tree.
- **Listeners in tests** (chosen by the maintainer). A test may listen, under rules: it binds only
  `127.0.0.1` or `::1`, never a wildcard address; it never resolves `localhost`; it always asks
  for port 0 and reads the chosen port back (`--address-file` for the CLI); its Unix sockets lie
  under the system temporary directory with paths below 100 bytes; it connects only to listeners
  it opened itself. Listener tests have the label `server`; where a listener is refused (EPERM in
  a sandbox) they skip, saying `local listener refused: <reason>`, unless
  `PRAENORA_TESTS_REQUIRE_LISTENERS=1`, which every `ci-*` test preset sets, makes the refusal a
  failure. The prohibition reads: "No network access in tests or in a default build: a test
  connects only to listeners it opens itself, on a loopback address (127.0.0.1, ::1) or a Unix
  domain socket, and resolves no host names."

### Builder notes (Proposed)

- `Workflow::status_json(PathForm)` and the status file share one function,
  `Workflow::Impl::status_document(PathForm)` (`src/runtime/api/workflow.cpp`): the snapshot's
  document from `detail::status_snapshot_json()`, with `PathAnchors::apply_to_status_snapshot()`
  for `placeholders`, dumped as compact JSON with bytes that are not UTF-8 replaced. The
  status-file writer's producer calls it with `placeholders`, and `status_json()` with `absolute`.
  The run's path anchors became a member of `Workflow::Impl`, declared before the writer, which
  uses them until its last write in its destructor. A `PathForm` value outside the enumeration
  gives the absolute form.
- The run number is an atomic count in the `Runtime`'s shared state
  (`Runtime::Impl::workflows_opened`, `mutable std::atomic<std::uint64_t>`, which every copy
  shares), advanced with `fetch_add(1, std::memory_order_relaxed)` once the `Workflow` exists,
  when nothing in `open()` can throw any more: an `open()` that throws takes no number, also one
  that fails after validation (an output directory that holds an earlier run, a log file that
  cannot be opened). The number is the only data the count guards, and it reaches the caller with
  the returned workflow, so relaxed ordering suffices. It enters neither the fingerprint, the run
  record nor the status snapshot.
- Python's `Workflow` opens each run with a `Runtime` of its own, as before (the facade constructs
  one per handle, with the handle's logging settings), so its `run_number` is always 1; the
  documentation says so.
- `Workflow.status(path_form=...)` takes the names exactly as written, as for isolation modes; a
  name that is not a `str` raises `TypeError`, as a path of another type does, and another name
  raises `PraenoraError` `INVALID_ARGUMENT` from the facade ("path_form '<name>' is not a path
  form: absolute or placeholders", the names from `praenora::frontend::kPathForms`).
- An empty `WorkflowOptions::status_file` is handled where the overrides are applied
  (`apply_overrides()`, `src/runtime/config/workflow_config.cpp`): the configuration's status file
  is dropped. An empty `output_dir` stays `INVALID_ARGUMENT`. The CLI and the Python package pass
  the empty value through unchanged.
- `UNAUTHENTICATED` and `PERMISSION_DENIED` follow `TARGET_DATA_UNAVAILABLE` in the runtime's
  table (`src/runtime/core/error.cpp`); `status.h` is unchanged, and the runtime library raises
  neither. The error taxonomy gives them a note and a triage row.
- The server core is `src/server/` (`praenora_server`, STATIC): `http_types.hpp` (`Request`,
  `Response`, `ListenerKind`), `address` (`ListenAddress`, `parse_listen_address()`),
  `tokens` (`read_token_file()`, `load_tokens()`, `check_credentials()`, `decide_scopes()`,
  `constant_time_equal()`, `check_allow_host()`, `host_allowed()`), `control_api` (`routes()`,
  `http_status_of()`, `ControlApi`), `run_source.hpp` (`RunSource`, `RunHandle`, `RunFacts`,
  `RunRequest`, `ServerMode`), `run_registry` (`SingleRun`, `RunRegistry`,
  `resolve_configuration()`, `first_run_number()`), `prometheus`, `openapi` and `clock` (the
  server's RFC 3339 instants, written without the runtime's internal helper). It links
  `Praenora::Runtime` publicly and `praenora_frontend`, nlohmann/json and the thread library
  privately; its include directory is `src/server`, as `praenora_frontend`'s is `src/frontend`. It
  is configured with `PRAENORA_BUILD_CLI`, beside the tool that links it.
- Run mode's source, `SingleRun`, shares the workflow's ownership (`std::shared_ptr<Workflow>`):
  a handler that found the run keeps it alive whatever the caller does meanwhile. Its
  `created_at` is given by the caller, who opened the workflow.
- `ControlApi::handle()` checks in this order: the Host check (TCP), the credentials (but for
  `GET /v0/health`), the path (404), the method (405), the scope (403), run mode's 404 for
  `POST /v0/runs` and `DELETE /v0/runs/{id}`, the media type (415), a run id that is not
  decimal (404), the body (400), a run id that names no run (404), then the route. So an
  unauthenticated request learns no path, and run mode answers 404 before it reads a body.
  Methods are case-sensitive; `HEAD` and `OPTIONS` answer 405 like any other method (a browser's
  preflight from a TCP listener with tokens 401); no route reads the query string; a path with a
  final `/` is unknown.
- A scope refusal has `details` `{"reason": "scope", "scope": "control"}`, beside the Host
  check's `{"reason": "host"}`. Its message says to use a token or a Unix socket (no tokens) or
  the token of `--token-file` (the read token).
- Media types: `application/json` in any case, with surrounding white space; a `charset`
  parameter, quoted or not, must be `utf-8`; other parameters are ignored; two `Content-Type`
  headers answer 415. `stop` needs `application/json` too, as every `POST` does, and takes an
  empty body or `{}`; another body is 400. So a page cannot send it as a simple request without a
  preflight, whatever the listener's credentials.
- Bodies: a field's error is located at its JSON pointer (`location.pointer`, `/seed`); `-0` is
  read as 0, as the runtime's readers read it; `1.0` and numbers beyond 2^64 are not integers.
- A validation failure of `Runtime::open()` is recognized by its details, a validation report
  (`ok` a boolean, `diagnostics` an array): it answers 422 whatever its first code is, also
  `NOT_FOUND` or `IO_ERROR`. `http_status_of()` applies this rule before the codes.
- Serve mode's numbering also counts `run-<digits>.log` entries, so a log left without its
  directory is never appended to by another run; digits with leading zeros count (`run-0011` is
  11) and numbers above 2^53-1 are ignored. `k` is taken once a request reaches
  `Runtime::open()`, under the registry's mutex, whether the open succeeds or not; a request
  refused before (its body, its configuration name, the forced isolation, `--max-runs`, a
  shutdown) takes none. `--max-runs` counts the opens in progress.
- An output directory that `Runtime::open()` refuses is recognized as an `IO_ERROR` located in
  `run-<k>`, both paths with their links resolved (`output_directory_refusal()`); it answers 409
  `INVALID_STATE` naming `run-<k>` only, with `details.output`. When either path cannot be
  resolved (a loop of links, a directory that cannot be searched), where the error lies cannot
  be told: the answer is an `IO_ERROR` of its own (500), its details `{"output", "cause"}`, the
  cause the error document `Runtime::open()` threw, as it is (its message and location can name
  absolute paths, as those of the 422 answer can). A validation failure (an error that carries a
  validation report) is never looked at, whatever its first diagnostic locates: it answers 422
  as it is.
- Configuration names also refuse NUL and other control characters (and `:` on Windows, where
  `C:a.yaml` would name another drive's directory); a name of something that is not a regular
  file is 400; the run is opened from the canonical path. A path through a symbolic link that
  leaves the root and comes back inside it is accepted, since only the resolved path counts.
- Containment is checked before existence, on the path with its links resolved as far as it
  exists (`weakly_canonical()`): a name beyond a link that leaves the root, whether anything
  exists there or not, and one whose resolution fails (a loop, a directory that cannot be
  searched), answer 400 with one message, "it does not resolve to a path inside the
  configuration root", so a client cannot probe outside the root. A missing file inside the root
  is 404. A dangling link inside the root still answers 404 for its own name: the client learns
  only whether the target the operator chose exists.
- `DELETE` decides by the workflow's state (completed, stopped or failed), not by its thread
  having ended: a run whose state is final is deleted, its thread joined (it is returning from
  `run()`), then its workflow closed. Each driver thread is moved out of its entry under the
  registry's mutex by whoever joins it, `remove()` or `shutdown()`, so it is joined once.
- While `shutdown()` runs, new runs answer 409 `INVALID_STATE` ("the server is shutting down");
  `shutdown()` waits for the opens in progress, and a run opened meanwhile is closed unstarted.
- The Host check takes exactly one `Host` header; an IP literal is compared as an address; an
  empty port is refused; `localhost` answers only on a loopback listener, also when
  `--allow-host` names it. `--allow-host` takes host names, IPv4 addresses and IPv6 addresses in
  brackets (`[2001:db8::1]`, compared as addresses), never a port. A wildcard listener
  (`0.0.0.0`, `[::]`) accepts only its literal address and the `--allow-host` names, which
  control-api.md says next to the check.
- Bearer tokens: exactly one `Authorization` header; the scheme in any case, then one or more
  spaces (a tab is refused), the token, trailing white space dropped; both tokens are compared
  on every request.
- Token files are checked before they are read: their status (following a symbolic link), then
  the mode (POSIX), then the first line. Trailing white space is spaces, tabs, carriage returns,
  form feeds and vertical tabs.
- Addresses: IPv4 parts with leading zeros are refused (some read them as octal), and IPv6 zones
  (`%en0`); an IPv4-mapped address (`::ffff:0:0/96`) is refused too, with a message that names the
  IPv4 form to give instead: an IPv6 listener takes IPv6 only (`IPV6_V6ONLY`), and the system
  binds no IPv4-mapped address to it. IPv6 addresses are written in the form of RFC 5952.
  `unix:<path>` on Windows throws `UNSUPPORTED` (category compatibility, whose own exit code is
  3); the tool turns every error of the address parser, this one included, into a usage error
  (exit 2) that carries its message, for `--listen` and for `status --server` alike. The length of
  a Unix socket's path is checked by the parser, against `sizeof(sockaddr_un::sun_path) - 1`.
- The run entry and the server document are canonical JSON, their keys sorted. The run record is
  answered compact (`run_record_json()` writes it indented); the other documents as the workflow
  gives them.
- The OpenAPI document: operation ids `getHealth`, `getServer`, `getOpenApiDocument`, `listRuns`,
  `createRun`, `getRunStatus`, `getRunMetrics`, `getRunRecord`, `getRunOutputs`, `stopRun`,
  `deleteRun` and `getMetrics`; a top-level `security` of `bearer` or none (Unix sockets and TCP
  without tokens need no token), with `GET /v0/health`'s `security` empty; every operation lists
  500 besides its own error statuses, and 413 for both `POST` routes; its 415 says that a
  `charset`, when given, is `utf-8`; `GET /metrics`'s media type is `text/plain; version=0.0.4`.
  `GET /v0/openapi.json` serves it compact, generated once when the `ControlApi` is constructed.
- The listener (`src/server/listener.{hpp,cpp}`, `Listener`) makes the listening socket itself and
  hands it to cpp-httplib's server, a subclass that sets its protected `svr_sock_`, so that
  `listen_after_bind()` accepts on it and `stop()` closes it. cpp-httplib binds and listens in one
  step, which leaves no moment to set a Unix socket's mode before connections are accepted, and
  reports a refused `bind()` without the system's error; a TCP port is bound with `SO_REUSEADDR`
  on POSIX (a restarted server rebinds a port in TIME_WAIT) and `SO_EXCLUSIVEADDRUSE` on Windows,
  never cpp-httplib's default `SO_REUSEPORT`, which would let another process share the port. An
  IPv6 listener takes IPv6 only (`IPV6_V6ONLY`); sockets are close-on-exec (`SOCK_CLOEXEC`, or
  `FD_CLOEXEC` where there is none; `WSA_FLAG_NO_HANDLE_INHERIT` on Windows); the backlog is 128.
  This relies on a protected member of cpp-httplib 0.67, the only version built and tested.
- A Unix socket: the listener checks the path itself as well, since a `ListenAddress` can be made
  without the parser, so no address is copied into `sun_path` unchecked: one that `sun_path`
  cannot hold with its final NUL is `IO_ERROR` naming the limit, and one that is empty or holds a
  NUL is `IO_ERROR` too (its message does not quote the path, whose NUL would end it). The path is
  examined with `lstat()` (a symbolic link is not followed and is refused); a socket there is
  probed with a non-blocking `connect()`: success, `EAGAIN` or `EINPROGRESS` mean somebody
  listens; any error but `ECONNREFUSED` is refused, naming it; and it is stale, and removed, only
  when three probes, 50 ms apart, are all refused, because on macOS and the BSDs a server whose
  backlog is full refuses a connection with `ECONNREFUSED` too (Linux says `EAGAIN`), and one probe
  could take a busy server's socket for a dead one's. Then `bind()`, `chmod(0600)`, a check that
  the file is a socket of mode 0600, and only then `listen()`: a socket that is bound and not
  listening refuses connections, so no client can connect before its mode is 0600. On Linux the
  socket is given mode 0600 with `fchmod()` before `bind()` as well, which makes its file with that
  mode (less the umask). On macOS the file exists, for the moment between `bind()` and `chmod()`
  (normally microseconds), with the mode the umask leaves of 0777, while it refuses every
  connection. No umask is changed: it is the process's, and other threads may create files. The
  listener removes the file when it stops listening, only when the path still names the socket it
  made (same device and inode).
- Limits through cpp-httplib's settings: `set_payload_max_length(64 KiB)`, read and write
  timeouts of 5 s, `set_keep_alive_max_count(100)`, `set_keep_alive_timeout(5)` and a
  `ThreadPool(8)` as task queue, without a limit on queued connections (one beyond the 8 workers
  waits for a worker). One handler on the pattern `.*` for GET (and so HEAD, with its method
  kept), POST, PUT, PATCH, DELETE and OPTIONS hands every request to `ControlApi::handle()`.
  cpp-httplib's own refusals and a handler that throws carry error documents through its error
  handler, which leaves an answer with a body alone: 413 (a body over 64 KiB) and 400 (a malformed
  request, an unknown method such as `TRACE`, an HTTP/1.1 request with no `Host` header or several)
  are `INVALID_ARGUMENT`, 404 `NOT_FOUND`, 500 `INTERNAL` without the exception's text. So over a
  real connection the Host check sees only requests with one `Host` header, or HTTP/1.0 ones with
  none. cpp-httplib adds `Connection: close` to every answer of status 400 or above, the route's
  own included, and closes the connection after it. A body over the limit is read up to the limit
  before the 413, then the rest is drained for at most 100 ms; the in-process test sends a declared
  70 KiB body of which 64 KiB and one byte come, so the listener has read everything sent when it
  closes, and no reset can take the answer away (as one may on Windows).
- cpp-httplib's server object ignores SIGPIPE for the whole process (its constructor calls
  `signal(SIGPIPE, SIG_IGN)`). The module host sets no disposition of its own for SIGPIPE (it
  ignores only `SIGTTOU` and `SIGTTIN`), so it would inherit that when the tool listens: the
  runtime starts every module host with SIGPIPE at its default disposition
  (`POSIX_SPAWN_SETSIGDEF`, `src/runtime/platform/posix/child_process_posix.cpp`), whatever its own
  process has, and process-isolation.md says so. A module in the runtime's process shares the
  process's disposition.
- No write of cpp-httplib's clients raises SIGPIPE, whatever the process's disposition: the
  client of `status --server` (`src/server/client.cpp`) and the OTLP exporter's in the runtime,
  which also runs in plain `praenora run`, `validate` and applications that embed the runtime,
  none of which ignores SIGPIPE. On POSIX systems other than macOS every `send()` of cpp-httplib
  passes `MSG_NOSIGNAL`: `CPPHTTPLIB_SEND_FLAGS`, a compile definition of `praenora_httplib`, so
  that every source that includes `httplib.h` agrees on it (the definition is used inline, in
  `SocketStream::write()`), and a `static_assert` in both clients checks it is set. macOS's
  `send()` documents no `MSG_NOSIGNAL`: both clients set `SO_NOSIGPIPE` on their sockets through
  `set_socket_options()`, which replaces nothing (a client has no callback by default; the
  `SO_REUSEPORT` or `SO_REUSEADDR` of `default_socket_options()` is a server's default). The
  server keeps cpp-httplib's own `signal(SIGPIPE, SIG_IGN)`. A large request is where a reset
  meets a write: cpp-httplib's client sends a body of 1 KiB or more after waiting up to 1 s for
  `100 Continue`, so a peer that reads the request line and resets the connection (a collector
  restarted, or an endpoint that is a TLS or gRPC port) is met by the body's first write. The tests
  `OtlpHttp.AResetConnectionFailsTheRequestNotTheProcess` and
  `ListenerTest.ClientOfAConnectionResetWhileWritingFailsWithoutSigpipe` (label `server`, POSIX)
  send 16 MiB to a listener that does so, in the child process of a death test with SIGPIPE at its
  default disposition, which must exit normally, its request failed; where the settings are
  missing, SIGPIPE ends it.
- `Listener::stop()` closes the listening socket, then joins the thread of the accept loop, which
  returns once the worker threads answered what they hold (a keep-alive connection notices the
  closed socket within 10 ms); then the socket's file is removed. `praenora serve` stops the
  listener before it shuts the registry down, so no handler holds a run of the registry when it
  closes them; `praenora run --listen` stops it once `Workflow::run()` returned, before the
  summary.
- The tool's side (`src/cli/serving.{hpp,cpp}`): `listen_settings()` checks the address, the token
  files, the credentials and `--allow-host` and turns each error into a usage error with its
  message; the flags beside `--listen` without it are usage errors. `Server`'s constructor opens
  the listener (`IO_ERROR`, exit 74, when the system refuses), prints the note and writes the
  address file (`IO_ERROR` when it cannot), so a script that waits for the address file finds the
  note printed; `start()` makes the control API and starts answering, and a client that connected
  before waits for it. `praenora run --listen` constructs the `Server` before `Runtime::open()`, so
  nothing is opened (no output directory, no log file) when the listener is refused or the address
  file cannot be written, and starts it once the workflow is open, with its `SingleRun`, which the
  `Server` never outlives. The UTF-8 names of token files, configuration roots and address files
  become paths through one helper, `praenora::frontend::path_from_utf8()`.
- The address file is written as `F.tmp-<pid>` beside `F`, in binary mode (a line feed only), and
  renamed over `F`; it is left in place when the tool ends.
- `praenora serve` waits for a signal with `StopOnSignal::wait_for_signal()`, which looks at the
  flag that the signal handler sets every 50 ms, as the watcher does; it then prints
  "praenora: shutting down: ..." on standard error, stops the listener, shuts the registry down
  and exits 0, printing nothing on standard output (`{"address": "<address>"}` as the result with
  `--json`). A second signal ends it with 130 at once, leaving its socket file, which the stale
  rule replaces. On Windows, a closed console waits for the `StopOnSignal` to go, at most 4 s.
- The client (`src/server/client.{hpp,cpp}`, `get()` and `answer_error()`) is cpp-httplib's client
  with the IP address mapped to itself (`set_direct_hostname_addr_map`), so the system takes it as
  a numeric address (`AI_NUMERICHOST`) and resolves nothing; its `Host` header is cpp-httplib's,
  the address and the port (`[::1]:<port>` for IPv6, no port for 80). A Unix socket's path that
  starts with `@` is given as `./@...`, since cpp-httplib takes `@` for Linux's abstract
  namespace. It waits 5 s to connect and 10 s for an answer, keeps no connection alive, and maps a
  failed connection to `IO_ERROR` naming the address. An answer that is not 200 is turned back into
  its error: code by name (`UNKNOWN` for a name it does not know), message, location and details;
  a body that is no error document is `IO_ERROR` with the status. `praenora status --server`
  refuses `--token-file` with a Unix socket, as the server does, and a `--run` that is not a run
  id; its table has a header row (`id`, `workflow`, `state`, `health`, `ticks`).
- Tests: `Workflow.StatusJsonPlaceholdersMatchesStatusFile`,
  `Workflow.RunNumbersCountOpensAcrossRuntimeCopies` (copies opening at once from four threads
  too) and `WorkflowConfig.EmptyStatusFileOverrideMeansNone` (label `integration`);
  `ErrorTaxonomy.UnauthenticatedIs900` and `ErrorTaxonomy.PermissionDeniedIs901` and the path
  forms in `Frontend.*` (labels `unit`, `cli`); `cli.run.empty_status_file`, with the control
  `cli.run.configured_status_file`, on a configuration that keeps a status file beside it
  (`tests/fixtures/configs/cli/status-file/`, copied into the build tree), its last command
  `praenora status` on the file that must not exist (exit 74); and the Python tests
  `WorkflowTest.test_status_path_forms`, `test_run_number` and `test_empty_status_file`.
- Tests (labels `unit` and `protocol`, without sockets): `ListenAddress.*`, `ServerClock.*`,
  `TokenFile.*`, `Credentials.*`, `Scopes.*`, `HostCheck.*`, `Prometheus.*`, `ControlApi.*` and
  `HttpStatus.*` over a fake run source; `RunRegistry.*`, `ConfigurationRoot.*`,
  `OutputRoot.*` and `SingleRun.*` with the fixture modules (`praenora_server_tests`), among them
  `RunRegistry.QueriesRaceRemoveAndShutdown` (run under ThreadSanitizer too) and
  `RunRegistry.RunGivesTheResultsOfPraenoraRun`, which compares a served run of the example
  workflow with the calls `praenora run` makes (`Runtime::open()`, then `Workflow::run()`); and
  `OpenApi.*` (`praenora_openapi_tests`).
- Listener tests (the maintainer's rules): `tests/support/listener_support.hpp` holds
  `kListenerRefused` (`local listener refused: `), `PRAENORA_START_LISTENER` and
  `PRAENORA_LISTENER_REFUSED`, which skip or, with `PRAENORA_TESTS_REQUIRE_LISTENERS=1`, fail; the
  failure's words differ from the skip's, because CTest marks a test whose output matches
  `SKIP_REGULAR_EXPRESSION` as skipped whatever its exit code. The reason of a refusal comes from a
  listener of the system's own sockets on `127.0.0.1` (cpp-httplib's server says only that it
  failed). A refused port is the port of a listener the test opened and closed (`ClosedPort`); a
  peer that never answers is a listener of the test's that never accepts (`SilentListener`, whose
  backlog the system completes); `FakeCollector` is an `httplib::Server` that records requests and
  answers a script. The Python test fails itself with the variable set
  (`support.listener_refused()`), since `python.unittest` is one CTest test, and binds with
  `socketserver.TCPServer`, which resolves no name (`http.server.HTTPServer` looks its host name
  up).
- Tests: `praenora_server_listener_tests` (label `server`, `LISTENER_SKIP`): the listener and the
  client in the test's own process (`ListenerTest.*`, `ControlClient.*`, `UnixSocketPath.*`, the
  last two without a listener) and, on POSIX, the tool as a server, started with `posix_spawn()`
  (`ListenerTest.Run*`, `Serve*`, `StatusServer*`, among them
  `ListenerTest.ServeWaitsForARunInALongCallAndThenExits0` and
  `ListenerTest.RunsWithAListenerGiveTheResultsOfPraenoraRun`); the CLI tests of the command
  lines, their usage and those refused before anything listens (the `cli.serve.*` tests, the
  `cli.status.*` tests of `--server`, `cli.run.listen_host_name_refused`,
  `cli.run.server_flags_need_listen`, `cli.run.help_lists_listen`,
  `cli.usage.tool_help_lists_serve` and `cli.usage.tool_help_lists_status_server`); and the
  install test's check of `serve`'s and `status --server`'s usage and of `serve` without
  `--config-root`. The tests that start the tool send it signals, so they are POSIX only; on
  Windows the in-process ones run. The tests that signal a server while a run sits in a call wait
  first for the file that fx-slow writes as its invoke starts (its parameter `entered_file`), so
  the signal never lands between ticks.
  `ChildProcess.StartsWithSigpipeAtItsDefaultDisposition` (label `isolation`) checks that a child
  started while its parent ignores SIGPIPE is ended by one.

## Alternatives considered

- **A server inside the runtime library.** Every application that embeds the runtime would carry
  an HTTP stack and could open a listener; a front end over the public API keeps the library as
  it is and proves the public API sufficient.
- **gRPC or another binary protocol.** It needs a schema compiler and a heavier dependency;
  curl, browsers' tools and scripts speak HTTP and JSON, and the documents are JSON already.
- **The status file only, polled over a shared file system** (the situation before). It cannot
  stop a run or open one, and it needs a shared directory.
- **Tokens on the command line or in the environment.** Both are visible to other users of the
  machine (process listings, `/proc`); a file with a restricted mode is not.
- **TLS in the process.** It needs a TLS library and certificate handling in the tool; a reverse
  proxy does it better, and loopback and Unix-socket listeners need none.
- **Pushed updates** (server-sent events, long polling). Polling a snapshot is enough to watch a
  run; pushing can be added later without changing the routes.

## Consequences

- An operator can watch and stop a run, and with `praenora serve` open runs, from another process,
  without the runtime library changing.
- The `praenora` tool gains a dependency (cpp-httplib, header-only), fetched by CI.
- Listener tests need a machine that allows loopback or Unix-socket listeners; in a sandbox that
  refuses them they skip, except in CI, where they fail.
- In serve mode a crash in an in-process module ends every run of the server; the documentation
  recommends `--isolation process` (POSIX).
- Not part of this version: a server in the Python package, Unix sockets on Windows, uploaded
  configurations and pushed updates.

## References

- [ADR-0015](ADR-0015-observability.md): observability
- [ADR-0017](ADR-0017-dependency-management.md): dependency management
- [ADR-0023](ADR-0023-public-cpp-api.md): the public C++ API
- [ADR-0043](ADR-0043-otlp-export-of-metrics-and-logs.md): OTLP export of metrics and logs,
  Prometheus text
- [docs/specs/control-api.md](../specs/control-api.md): the normative specification
- [spec/openapi/praenora-control-api.openapi.json](../../spec/openapi/praenora-control-api.openapi.json):
  the OpenAPI document
- [docs/testing.md](../testing.md#listeners): the rules of the tests that open listeners
- [docs/specs/observability.md](../specs/observability.md)
- [docs/specs/error-taxonomy.md](../specs/error-taxonomy.md)
- [docs/open-questions.md](../open-questions.md) (OQ-2, OQ-9)
- [OpenAPI Specification 3.1.0](https://spec.openapis.org/oas/v3.1.0)
- [RFC 6750: Bearer Token Usage](https://www.rfc-editor.org/rfc/rfc6750)
- [RFC 3339](https://www.rfc-editor.org/rfc/rfc3339)
- [cpp-httplib](https://github.com/yhirose/cpp-httplib)
