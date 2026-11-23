// The control API as a plain function: ControlApi::handle() answers a Request over a RunSource,
// with the route table, the checks of every request (Host, credentials, scope, media type, body)
// and the HTTP status of every error. A listener only moves bytes and enforces the limits of the
// transport (body size, timeouts).
#ifndef PRAENORA_SERVER_CONTROL_API_HPP
#define PRAENORA_SERVER_CONTROL_API_HPP

#include "address.hpp"
#include "http_types.hpp"
#include "run_source.hpp"
#include "tokens.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/runtime.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace praenora::server {

// The version of the API (the OpenAPI document's info.version, GET /v0/server's api_version);
// its paths lie under /v0/.
inline constexpr std::string_view kApiVersion = "0.1.0";

// The content type of every JSON body the API writes (compact JSON).
inline constexpr std::string_view kJsonContentType = "application/json; charset=utf-8";

// The scope a route needs: none (GET /v0/health), read or control.
enum class RouteScope { none, read, control };

// "none", "read" or "control".
std::string_view scope_name(RouteScope scope);

// One route of the API. The OpenAPI document is generated from these.
struct Route {
    std::string_view method;       // "GET", "POST" or "DELETE"
    std::string_view path;         // for example "/v0/runs/{id}"; {id} is one path segment
    std::string_view operation_id; // for example "getRunStatus"
    RouteScope scope;
    std::string_view summary;
    std::string_view request_schema; // the component schema of its JSON body; empty: none
    bool request_required = false;   // whether it needs a body
    int success_status = 200;        // 200, 201, 202 or 204
    std::string_view success_media;  // the media type of its answer; empty for 204
    std::string_view success_schema; // the component schema of its answer; empty for 204
    std::vector<int> error_statuses; // the statuses of the errors it answers, besides 500
};

// Every route, in the order of the API's documentation.
const std::vector<Route>& routes();

// Whether the details of `error` are a validation report ({"ok": a boolean, "diagnostics": an
// array}): a validation failure, as Runtime::open() throws it, whatever its first code.
bool carries_validation_report(const Error& error);

// The HTTP status of an error that no route answers with a status of its own: 422 for an error
// that carries_validation_report(); otherwise by code: UNAUTHENTICATED 401, PERMISSION_DENIED
// 403, NOT_FOUND 404, INVALID_STATE 409, RESOURCE_EXHAUSTED 503; otherwise by category: usage
// 400; validation, compatibility, load and data 422; timeout 504; cancelled 409; execution,
// environment and internal 500.
int http_status_of(const Error& error);

// The process id of this process.
std::int64_t current_process_id() noexcept;

struct ControlApiSettings {
    // The listener's address, with its real port: the Host check of TCP requests compares with
    // it.
    ListenAddress listener;
    Tokens tokens;                        // a TCP listener's tokens; none for a Unix socket
    std::vector<std::string> allow_hosts; // --allow-host names
    std::int64_t started_at_unix_ns = 0;  // when the server started (GET /v0/server)
    std::int64_t pid = 0;                 // the server's process id (GET /v0/server)
};

class ControlApi {
public:
    // Checks the settings (check_credentials(), check_allow_host()) and generates the OpenAPI
    // document once. Throws praenora::Error(INVALID_ARGUMENT) for settings that break a rule, and
    // what openapi_document() throws. `source` must outlive the ControlApi.
    ControlApi(const Runtime& runtime, RunSource& source, ControlApiSettings settings);

    // The answer to `request`, from any thread. In order: a TCP request whose Host fails
    // host_allowed() is 403 PERMISSION_DENIED (details.reason "host"); a request without valid
    // credentials (decide_scopes()) is 401 UNAUTHENTICATED with "WWW-Authenticate: Bearer",
    // except GET /v0/health; an unknown path is 404 NOT_FOUND; a known path with another method is
    // 405 INVALID_ARGUMENT with Allow; a request without the route's scope is 403
    // PERMISSION_DENIED (details.reason "scope"); POST /v0/runs and DELETE /v0/runs/{id} are 404
    // in run mode; a POST without Content-Type application/json (stop's included, whose body may
    // be empty) is 415 INVALID_ARGUMENT; a body that breaks its schema is 400 INVALID_ARGUMENT; an
    // unknown run id is 404. Errors answer the error document of Error::to_json() with
    // http_status_of()'s status; anything else thrown answers 500 INTERNAL without its text.
    Response handle(const Request& request) const;

private:
    Response answer(const Request& request) const;

    RunSource& source_;
    ControlApiSettings settings_;
    std::string openapi_; // compact
};

} // namespace praenora::server

#endif // PRAENORA_SERVER_CONTROL_API_HPP
