#include "control_api.hpp"

#include "clock.hpp"
#include "openapi.hpp"
#include "prometheus.hpp"

#include <frontend.hpp>

#include <praenora/runtime/status.hpp>
#include <praenora/runtime/version.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace praenora::server {

namespace {

using nlohmann::json;

constexpr std::string_view kJsonMedia = "application/json";
constexpr std::string_view kPrometheusMedia = "text/plain; version=0.0.4";

// Dumps without throwing on bytes that are not UTF-8.
std::string dump(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

Response json_response(int status, std::string body) {
    Response response;
    response.status = status;
    response.content_type = std::string(kJsonContentType);
    response.body = std::move(body);
    return response;
}

Response error_response(const Error& error, int status) {
    return json_response(status, error.to_json());
}

// `text` without the spaces and tabs around it.
std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// Whether a Content-Type value is application/json (in any case), its charset, if it names one,
// UTF-8.
bool is_json_media_type(std::string_view value) {
    const std::size_t semicolon = value.find(';');
    if (frontend::lower(trimmed(value.substr(0, semicolon))) != kJsonMedia) {
        return false;
    }
    std::size_t begin = semicolon;
    while (begin != std::string_view::npos) {
        const std::size_t end = value.find(';', begin + 1);
        const std::string_view parameter = trimmed(value.substr(
            begin + 1, end == std::string_view::npos ? std::string_view::npos : end - begin - 1));
        const std::size_t equals = parameter.find('=');
        if (equals != std::string_view::npos &&
            frontend::lower(trimmed(parameter.substr(0, equals))) == "charset") {
            std::string_view charset = trimmed(parameter.substr(equals + 1));
            if (charset.size() >= 2 && charset.front() == '"' && charset.back() == '"') {
                charset = charset.substr(1, charset.size() - 2);
            }
            if (frontend::lower(charset) != "utf-8") {
                return false;
            }
        }
        begin = end;
    }
    return true;
}

// The segments of a path that starts with '/': "/v0/runs/1" is {"v0", "runs", "1"}.
std::optional<std::vector<std::string_view>> segments_of(std::string_view path) {
    if (path.empty() || path.front() != '/') {
        return std::nullopt;
    }
    std::vector<std::string_view> segments;
    std::size_t begin = 1;
    while (true) {
        const std::size_t end = path.find('/', begin);
        segments.push_back(path.substr(begin, end == std::string_view::npos ? std::string_view::npos
                                                                            : end - begin));
        if (end == std::string_view::npos) {
            return segments;
        }
        begin = end + 1;
    }
}

// Whether the route path `pattern` matches `segments`; its {id} segment, if any, goes to `id`.
bool matches(std::string_view pattern, const std::vector<std::string_view>& segments,
             std::string& id) {
    const auto parts = segments_of(pattern);
    if (!parts || parts->size() != segments.size()) {
        return false;
    }
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if ((*parts)[i] == "{id}") {
            if (segments[i].empty()) {
                return false;
            }
            id = std::string(segments[i]);
        } else if ((*parts)[i] != segments[i]) {
            return false;
        }
    }
    return true;
}

struct Match {
    const Route* route = nullptr;     // the route of the path and the method
    std::vector<std::string> allowed; // the methods of the routes of the path, in table order
    std::string id;                   // the path's {id} segment
};

Match match_route(const std::string& method, const std::string& path) {
    Match match;
    const auto segments = segments_of(path);
    if (!segments) {
        return match;
    }
    for (const Route& route : routes()) {
        std::string id;
        if (!matches(route.path, *segments, id)) {
            continue;
        }
        match.allowed.emplace_back(route.method);
        if (route.method == method) {
            match.route = &route;
            match.id = std::move(id);
        }
    }
    return match;
}

// A run id: decimal, without leading zeros.
bool is_run_id(const std::string& id) {
    if (id.empty() || id.front() == '0') {
        return false;
    }
    for (const char c : id) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

Error no_run(const std::string& id) {
    return Error(ErrorCode::NOT_FOUND, "there is no run '" + id + "'");
}

// The JSON pointer of the field `key` of the body.
std::string pointer_to(const std::string& key) {
    std::string pointer = "/";
    for (const char c : key) {
        if (c == '~') {
            pointer += "~0";
        } else if (c == '/') {
            pointer += "~1";
        } else {
            pointer += c;
        }
    }
    return pointer;
}

Error body_error(const std::string& message, const std::string& key = "") {
    SourceLocation location;
    if (!key.empty()) {
        location.pointer = pointer_to(key);
    }
    return Error(ErrorCode::INVALID_ARGUMENT, message, location);
}

// The body as a JSON object; INVALID_ARGUMENT for anything else.
json body_object(const std::string& body, const std::string& what) {
    json document = json::parse(body, nullptr, false);
    if (document.is_discarded()) {
        throw body_error("the body of " + what + " is not JSON (UTF-8)");
    }
    if (!document.is_object()) {
        throw body_error("the body of " + what + " must be a JSON object");
    }
    return document;
}

std::uint64_t count_field(const json& value, const std::string& key) {
    if (value.is_number_unsigned() && value.get<std::uint64_t>() <= frontend::kMaxCount) {
        return value.get<std::uint64_t>();
    }
    // -0 is 0, as every reader of the runtime takes it.
    if (value.is_number_integer() && !value.is_number_unsigned() &&
        value.get<std::int64_t>() == 0) {
        return 0;
    }
    throw body_error(
        "'" + key + "' must be an integer from 0 to " + std::to_string(frontend::kMaxCount), key);
}

RunRequest parse_run_request(const std::string& body) {
    const json document = body_object(body, "POST /v0/runs");
    RunRequest request;
    bool configured = false;
    for (const auto& item : document.items()) {
        const std::string& key = item.key();
        const json& value = item.value();
        if (key == "configuration") {
            if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
                throw body_error("'configuration' must be the name of a configuration file under "
                                 "the configuration root, a relative path with '/'",
                                 key);
            }
            request.configuration = value.get<std::string>();
            configured = true;
        } else if (key == "seed") {
            request.seed = count_field(value, key);
        } else if (key == "ticks") {
            request.ticks = count_field(value, key);
        } else if (key == "trace_messages") {
            if (!value.is_boolean()) {
                throw body_error("'trace_messages' must be true or false", key);
            }
            request.trace_messages = value.get<bool>();
        } else if (key == "isolation") {
            const std::optional<IsolationMode> mode =
                value.is_string() ? frontend::isolation_mode(value.get_ref<const std::string&>())
                                  : std::nullopt;
            if (!mode) {
                throw body_error(
                    "'isolation' must be " + frontend::names_of(frontend::kIsolationModes), key);
            }
            request.isolation = mode;
        } else {
            throw body_error("the body of POST /v0/runs has no field '" + key +
                                 "'; its fields are configuration, seed, ticks, trace_messages "
                                 "and isolation",
                             key);
        }
    }
    if (!configured) {
        throw body_error("the body of POST /v0/runs needs 'configuration'");
    }
    return request;
}

// The run entry: the run's facts and the state of its status now.
json entry_json(const RunHandle& run) {
    const RunFacts& facts = run.facts();
    const StatusSnapshot status = run.status();
    return {{"id", facts.id},
            {"run_number", facts.run_number},
            {"configuration", facts.configuration},
            {"workflow", {{"name", status.workflow_name}, {"version", status.workflow_version}}},
            {"fingerprint", status.fingerprint},
            {"state", to_string(status.state)},
            {"health", to_string(status.health)},
            {"ticks_completed", status.ticks_completed},
            {"created_at", facts.created_at},
            {"output", facts.output ? json(*facts.output) : json(nullptr)}};
}

} // namespace

std::string_view scope_name(RouteScope scope) {
    switch (scope) {
    case RouteScope::none:
        return "none";
    case RouteScope::read:
        return "read";
    case RouteScope::control:
        return "control";
    }
    return "control";
}

const std::vector<Route>& routes() {
    static const std::vector<Route> table = {
        {"GET",
         "/v0/health",
         "getHealth",
         RouteScope::none,
         "Whether the server answers; needs no credentials.",
         "",
         false,
         200,
         kJsonMedia,
         "Health",
         {403}},
        {"GET",
         "/v0/server",
         "getServer",
         RouteScope::read,
         "The server: the API and runtime versions, its mode, process and start, and the scopes "
         "of this request.",
         "",
         false,
         200,
         kJsonMedia,
         "ServerInfo",
         {401, 403}},
        {"GET",
         "/v0/openapi.json",
         "getOpenApiDocument",
         RouteScope::read,
         "This OpenAPI document.",
         "",
         false,
         200,
         kJsonMedia,
         "OpenApiDocument",
         {401, 403}},
        {"GET",
         "/v0/runs",
         "listRuns",
         RouteScope::read,
         "The runs the server holds, in id order.",
         "",
         false,
         200,
         kJsonMedia,
         "RunList",
         {401, 403}},
        {"POST",
         "/v0/runs",
         "createRun",
         RouteScope::control,
         "Opens and starts a run of a configuration under the configuration root (serve mode; "
         "404 in run mode).",
         "RunRequest",
         true,
         201,
         kJsonMedia,
         "RunEntry",
         {400, 401, 403, 404, 409, 413, 415, 422, 503}},
        {"GET",
         "/v0/runs/{id}",
         "getRunStatus",
         RouteScope::read,
         "The run's status snapshot, its error records naming the run's directories by "
         "placeholders.",
         "",
         false,
         200,
         kJsonMedia,
         "status-snapshot",
         {401, 403, 404}},
        {"GET",
         "/v0/runs/{id}/metrics",
         "getRunMetrics",
         RouteScope::read,
         "The run's metric points.",
         "",
         false,
         200,
         kJsonMedia,
         "Metrics",
         {401, 403, 404}},
        {"GET",
         "/v0/runs/{id}/record",
         "getRunRecord",
         RouteScope::read,
         "The run's run record as last written (409 before it exists).",
         "",
         false,
         200,
         kJsonMedia,
         "run-record",
         {401, 403, 404, 409}},
        {"GET",
         "/v0/runs/{id}/outputs",
         "getRunOutputs",
         RouteScope::read,
         "The newest envelope of each of the run's workflow outputs.",
         "",
         false,
         200,
         kJsonMedia,
         "Outputs",
         {401, 403, 404}},
        {"POST",
         "/v0/runs/{id}/stop",
         "stopRun",
         RouteScope::control,
         "Asks the run to stop; idempotent.",
         "StopRequest",
         false,
         202,
         kJsonMedia,
         "RunEntry",
         {400, 401, 403, 404, 413, 415}},
        {"DELETE",
         "/v0/runs/{id}",
         "deleteRun",
         RouteScope::control,
         "Forgets a finished run, its files kept (serve mode; 404 in run mode).",
         "",
         false,
         204,
         "",
         "",
         {401, 403, 404, 409}},
        {"GET",
         "/metrics",
         "getMetrics",
         RouteScope::read,
         "The metrics of every run the server holds, in the Prometheus text format 0.0.4.",
         "",
         false,
         200,
         kPrometheusMedia,
         "PrometheusText",
         {401, 403}},
    };
    return table;
}

bool carries_validation_report(const Error& error) {
    const json details = json::parse(error.details_json(), nullptr, false);
    return details.is_object() && details.contains("ok") && details.at("ok").is_boolean() &&
           details.contains("diagnostics") && details.at("diagnostics").is_array();
}

int http_status_of(const Error& error) {
    // A validation failure, as Runtime::open() throws it: the whole report in the details.
    if (carries_validation_report(error)) {
        return 422;
    }
    switch (error.code()) {
    case ErrorCode::UNAUTHENTICATED:
        return 401;
    case ErrorCode::PERMISSION_DENIED:
        return 403;
    case ErrorCode::NOT_FOUND:
        return 404;
    case ErrorCode::INVALID_STATE:
        return 409;
    case ErrorCode::RESOURCE_EXHAUSTED:
        return 503;
    default:
        break;
    }
    switch (error_code_info(error.code()).category) {
    case ErrorCategory::usage:
        return 400;
    case ErrorCategory::validation:
    case ErrorCategory::compatibility:
    case ErrorCategory::load:
    case ErrorCategory::data:
        return 422;
    case ErrorCategory::timeout:
        return 504;
    case ErrorCategory::cancelled:
        return 409;
    case ErrorCategory::execution:
    case ErrorCategory::environment:
    case ErrorCategory::internal:
        return 500;
    }
    return 500;
}

std::int64_t current_process_id() noexcept {
#ifdef _WIN32
    return static_cast<std::int64_t>(_getpid());
#else
    return static_cast<std::int64_t>(getpid());
#endif
}

ControlApi::ControlApi(const Runtime& runtime, RunSource& source, ControlApiSettings settings)
    : source_(source), settings_(std::move(settings)) {
    check_credentials(settings_.listener, settings_.tokens);
    for (const std::string& name : settings_.allow_hosts) {
        check_allow_host(name);
    }
    openapi_ = json::parse(openapi_document(runtime)).dump();
}

Response ControlApi::handle(const Request& request) const {
    try {
        return answer(request);
    } catch (const Error& error) {
        return error_response(error, http_status_of(error));
    } catch (...) {
        // Whatever it said stays in the server: it may name what a client must not learn.
        return error_response(Error(ErrorCode::INTERNAL, "the server failed to answer the request"),
                              500);
    }
}

Response ControlApi::answer(const Request& request) const {
    // The Host check first: a page served from a rebound name learns nothing, not even whether
    // its credentials would do.
    if (request.listener == ListenerKind::tcp &&
        !host_allowed(request.header_values("host"), settings_.listener, settings_.allow_hosts)) {
        throw Error(ErrorCode::PERMISSION_DENIED,
                    "the request's Host header does not name this server: give the listener's "
                    "address (" +
                        settings_.listener.to_string() +
                        "), localhost for a loopback listener, or a name of --allow-host",
                    {}, R"({"reason":"host"})");
    }
    const Match match = match_route(request.method, request.path);
    const bool open_route = match.route != nullptr && match.route->scope == RouteScope::none;
    const ScopeDecision decision =
        decide_scopes(request.listener, settings_.tokens, request.header_values("authorization"));
    if (!open_route && !decision.authenticated) {
        Response response = error_response(
            Error(ErrorCode::UNAUTHENTICATED,
                  "the request carries no valid credentials: send \"Authorization: Bearer "
                  "<token>\" with the token of --token-file or of --read-token-file"),
            401);
        response.headers.emplace_back("WWW-Authenticate", "Bearer");
        return response;
    }
    if (match.allowed.empty()) {
        throw Error(ErrorCode::NOT_FOUND, "'" + request.path +
                                              "' is not a path of the control "
                                              "API (GET /v0/openapi.json lists "
                                              "them)");
    }
    if (match.route == nullptr) {
        std::string allow;
        for (const std::string& method : match.allowed) {
            allow += (allow.empty() ? "" : ", ") + method;
        }
        Response response = error_response(
            Error(ErrorCode::INVALID_ARGUMENT,
                  "'" + request.path + "' takes " + allow + ", not " + request.method),
            405);
        response.headers.emplace_back("Allow", allow);
        return response;
    }
    const Route& route = *match.route;
    const std::string call = std::string(route.method) + " " + std::string(route.path);
    if ((route.scope == RouteScope::read && !decision.scopes.read) ||
        (route.scope == RouteScope::control && !decision.scopes.control)) {
        const std::string why =
            settings_.tokens.empty()
                ? "a TCP listener without tokens grants only the read scope; use a token of "
                  "--token-file or a Unix socket"
                : "the read token grants only the read scope; use the token of --token-file";
        throw Error(ErrorCode::PERMISSION_DENIED,
                    call + " needs the " + std::string(scope_name(route.scope)) + " scope: " + why,
                    {},
                    dump({{"reason", "scope"}, {"scope", std::string(scope_name(route.scope))}}));
    }
    const std::string_view operation = route.operation_id;
    if (source_.mode() == ServerMode::run &&
        (operation == "createRun" || operation == "deleteRun")) {
        throw Error(ErrorCode::NOT_FOUND, call + " is not served in run mode, which serves its "
                                                 "one run; praenora serve opens and deletes "
                                                 "runs");
    }
    // Every POST names its media type, stop's too (whose body may be empty): a browser then sends
    // a preflight first, which nothing here answers with a CORS header.
    if (route.method == std::string_view("POST")) {
        const std::vector<std::string> types = request.header_values("content-type");
        if (types.size() != 1 || !is_json_media_type(types.front())) {
            return error_response(
                Error(ErrorCode::INVALID_ARGUMENT,
                      call + " takes Content-Type application/json" +
                          (operation == "stopRun" ? std::string(" (its body may be empty)") : "")),
                415);
        }
    }

    if (operation == "getHealth") {
        return json_response(200, R"({"status":"ok"})");
    }
    if (operation == "getServer") {
        json scopes = json::array();
        if (decision.scopes.control) {
            scopes.push_back("control");
        }
        if (decision.scopes.read) {
            scopes.push_back("read");
        }
        return json_response(200,
                             dump({{"api_version", std::string(kApiVersion)},
                                   {"runtime_version", version_info().runtime},
                                   {"mode", source_.mode() == ServerMode::run ? "run" : "serve"},
                                   {"pid", settings_.pid},
                                   {"started_at", format_rfc3339_utc(settings_.started_at_unix_ns)},
                                   {"scopes", std::move(scopes)}}));
    }
    if (operation == "getOpenApiDocument") {
        return json_response(200, openapi_);
    }
    if (operation == "listRuns") {
        json runs = json::array();
        for (const auto& run : source_.list()) {
            runs.push_back(entry_json(*run));
        }
        return json_response(200, dump({{"runs", std::move(runs)}}));
    }
    if (operation == "getMetrics") {
        std::vector<RunMetrics> metrics;
        for (const auto& run : source_.list()) {
            metrics.push_back(
                {run->facts().run_number, run->status().fingerprint, run->metrics_json()});
        }
        Response response;
        response.content_type = std::string(kPrometheusContentType);
        response.body = prometheus_text(metrics);
        return response;
    }
    if (operation == "createRun") {
        const std::shared_ptr<RunHandle> run = source_.create(parse_run_request(request.body));
        Response response = json_response(201, dump(entry_json(*run)));
        response.headers.emplace_back("Location", "/v0/runs/" + run->facts().id);
        return response;
    }

    // The routes of one run.
    if (!is_run_id(match.id)) {
        throw no_run(match.id);
    }
    if (operation == "stopRun") {
        if (!request.body.empty() && !body_object(request.body, call).empty()) {
            throw body_error(call + " takes no fields");
        }
        const std::shared_ptr<RunHandle> run = source_.stop(match.id);
        if (!run) {
            throw no_run(match.id);
        }
        return json_response(202, dump(entry_json(*run)));
    }
    if (operation == "deleteRun") {
        source_.remove(match.id);
        Response response;
        response.status = 204;
        return response;
    }
    const std::shared_ptr<RunHandle> run = source_.find(match.id);
    if (!run) {
        throw no_run(match.id);
    }
    if (operation == "getRunStatus") {
        return json_response(200, run->status_json());
    }
    if (operation == "getRunMetrics") {
        return json_response(200, run->metrics_json());
    }
    if (operation == "getRunOutputs") {
        return json_response(200, run->latest_outputs_json());
    }
    if (operation == "getRunRecord") {
        const json record = json::parse(run->run_record_json(), nullptr, false);
        if (record.is_discarded() || record.is_null()) {
            throw Error(ErrorCode::INVALID_STATE,
                        "run " + match.id +
                            " has no run record yet: it is written once the run "
                            "starts");
        }
        return json_response(200, dump(record));
    }
    throw Error(ErrorCode::INTERNAL, "the route " + call + " has no answer");
}

} // namespace praenora::server
