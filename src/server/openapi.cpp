#include "openapi.hpp"

#include "control_api.hpp"

#include <frontend.hpp>

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/status.hpp>

#include <nlohmann/json.hpp>

#include <set>
#include <string>

namespace praenora::server {

namespace {

using nlohmann::json;

json ref(std::string_view schema) {
    return {{"$ref", "#/components/schemas/" + std::string(schema)}};
}

json string_enum(const std::set<std::string>& names) {
    return {{"type", "string"}, {"enum", json(names)}};
}

json workflow_states() {
    std::set<std::string> names;
    for (const WorkflowState state :
         {WorkflowState::created, WorkflowState::starting, WorkflowState::ready,
          WorkflowState::running, WorkflowState::stopping, WorkflowState::completed,
          WorkflowState::stopped, WorkflowState::failed}) {
        names.insert(to_string(state));
    }
    return string_enum(names);
}

json healths() {
    std::set<std::string> names;
    for (const Health health :
         {Health::unknown, Health::healthy, Health::degraded, Health::unhealthy}) {
        names.insert(to_string(health));
    }
    return string_enum(names);
}

json categories() {
    std::set<std::string> names;
    for (const ErrorCodeInfo& info : all_error_codes()) {
        names.insert(to_string(info.category));
    }
    return string_enum(names);
}

json count_schema(const std::string& description) {
    return {{"type", "integer"},
            {"minimum", 0},
            {"maximum", frontend::kMaxCount},
            {"description", description}};
}

// The API's own schemas: valid in JSON Schema draft-07 and 2020-12 alike.
json own_schemas() {
    json schemas = json::object();
    schemas["Health"] = {{"type", "object"},
                         {"required", {"status"}},
                         {"properties", {{"status", {{"type", "string"}, {"enum", {"ok"}}}}}},
                         {"additionalProperties", false}};
    schemas["ServerInfo"] = {
        {"type", "object"},
        {"required", {"api_version", "runtime_version", "mode", "pid", "started_at", "scopes"}},
        {"properties",
         {{"api_version", {{"type", "string"}, {"description", "The API's version, 0.1.0."}}},
          {"runtime_version", {{"type", "string"}, {"description", "The runtime's version."}}},
          {"mode",
           {{"type", "string"},
            {"enum", {"run", "serve"}},
            {"description",
             "run: praenora run --listen, its one run; serve: praenora serve, runs on request."}}},
          {"pid", {{"type", "integer"}, {"description", "The server's process id."}}},
          {"started_at",
           {{"type", "string"},
            {"format", "date-time"},
            {"description", "When the server started (RFC 3339, UTC)."}}},
          {"scopes",
           {{"type", "array"},
            {"items", {{"type", "string"}, {"enum", {"control", "read"}}}},
            {"uniqueItems", true},
            {"description", "The scopes of this request, sorted."}}}}},
        {"additionalProperties", false}};
    schemas["RunEntry"] = {
        {"type", "object"},
        {"required",
         {"id", "run_number", "configuration", "workflow", "fingerprint", "state", "health",
          "ticks_completed", "created_at", "output"}},
        {"properties",
         {{"id",
           {{"type", "string"},
            {"pattern", "^[1-9][0-9]*$"},
            {"description", "The run id: in serve mode the k of run-<k>; in run mode 1."}}},
          {"run_number",
           {{"type", "integer"},
            {"minimum", 1},
            {"description", "The workflow's number in its Runtime (Workflow::run_number())."}}},
          {"configuration",
           {{"type", "string"},
            {"description", "Run mode: the configuration file's name; serve mode: its name "
                            "under the configuration root, with '/'."}}},
          {"workflow",
           {{"type", "object"},
            {"required", {"name", "version"}},
            {"properties", {{"name", {{"type", "string"}}}, {"version", {{"type", "string"}}}}},
            {"additionalProperties", false}}},
          {"fingerprint", {{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}}},
          {"state", workflow_states()},
          {"health", healths()},
          {"ticks_completed", {{"type", "integer"}, {"minimum", 0}}},
          {"created_at",
           {{"type", "string"},
            {"format", "date-time"},
            {"description", "When the server opened the run (RFC 3339, UTC)."}}},
          {"output",
           {{"type", {"string", "null"}},
            {"description",
             "Serve mode: run-<k>, the run's directory under the output root; run mode: null."}}}}},
        {"additionalProperties", false}};
    schemas["RunList"] = {
        {"type", "object"},
        {"required", {"runs"}},
        {"properties",
         {{"runs",
           {{"type", "array"}, {"items", ref("RunEntry")}, {"description", "In id order."}}}}},
        {"additionalProperties", false}};
    json modes = json::array();
    for (const auto& mode : frontend::kIsolationModes) {
        modes.push_back(std::string(mode.name));
    }
    schemas["RunRequest"] = {
        {"type", "object"},
        {"required", {"configuration"}},
        {"properties",
         {{"configuration",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "A configuration file under the configuration root: a relative path with '/', no "
             "empty, '.' or '..' segment, whose canonical path lies under the root's."}}},
          {"seed", count_schema("The run seed, in place of the configuration's.")},
          {"ticks", count_schema("The ticks to run, in place of the configuration's.")},
          {"trace_messages",
           {{"type", "boolean"}, {"description", "Trace every message to trace/messages.jsonl."}}},
          {"isolation",
           {{"type", "string"},
            {"enum", std::move(modes)},
            {"description", "Every step's isolation, in place of the configuration's."}}}}},
        {"additionalProperties", false}};
    schemas["StopRequest"] = {{"type", "object"},
                              {"maxProperties", 0},
                              {"description",
                               "No fields. The body may also be empty; its Content-Type is "
                               "application/json all the same, as every POST's."}};
    schemas["Metrics"] = {
        {"type", "object"},
        {"required", {"metrics"}},
        {"properties", {{"metrics", {{"type", "array"}, {"items", ref("MetricPoint")}}}}},
        {"description", "Workflow::metrics_json()."}};
    json summary_fields = json::object();
    for (const char* field : {"count", "sum", "min", "max", "last"}) {
        summary_fields[field] = {{"type", "integer"}};
    }
    schemas["MetricPoint"] = {
        {"type", "object"},
        {"required", {"name", "unit", "kind", "attributes"}},
        {"properties",
         {{"name", {{"type", "string"}}},
          {"unit", {{"type", "string"}}},
          {"kind", {{"type", "string"}, {"enum", {"counter", "gauge", "summary"}}}},
          {"attributes",
           {{"type", "object"},
            {"additionalProperties", {{"type", "string"}}},
            {"description", "Every key with the prefix praenora."}}},
          {"value", {{"type", "integer"}}},
          {"summary",
           {{"type", "object"},
            {"required", {"count", "sum", "min", "max", "last"}},
            {"properties", std::move(summary_fields)}}}}}};
    schemas["Outputs"] = {
        {"type", "object"},
        {"additionalProperties", ref("pmp-envelope")},
        {"description", "The newest envelope of each workflow output; {} at first."}};
    schemas["Error"] = {
        {"type", "object"},
        {"required", {"code", "name", "category", "message", "location", "details"}},
        {"properties",
         {{"code", {{"type", "integer"}, {"description", "The error code."}}},
          {"name", {{"type", "string"}, {"description", "The code's stable name."}}},
          {"category", categories()},
          {"message", {{"type", "string"}}},
          {"location",
           {{"type", "object"},
            {"properties",
             {{"file", {{"type", "string"}}},
              {"line", {{"type", "integer"}, {"minimum", 1}}},
              {"column", {{"type", "integer"}, {"minimum", 1}}},
              {"pointer", {{"type", "string"}}}}},
            {"additionalProperties", false}}},
          {"details",
           {{"description",
             "The code's details: an object (a validation report for 422), or a string when "
             "they are not JSON."}}}}},
        {"additionalProperties", false},
        {"description", "The error document of praenora::Error::to_json()."}};
    schemas["OpenApiDocument"] = {{"type", "object"}, {"description", "This document."}};
    schemas["PrometheusText"] = {
        {"type", "string"},
        {"description", "The Prometheus text exposition format 0.0.4, every line ending in a "
                        "line feed."}};
    return schemas;
}

std::string error_description(int status) {
    switch (status) {
    case 400:
        return "INVALID_ARGUMENT: the body breaks its schema, or a value is refused.";
    case 401:
        return "UNAUTHENTICATED: a TCP listener with tokens got no valid bearer token.";
    case 403:
        return "PERMISSION_DENIED: the Host header names no address of the server "
               "(details.reason host), or the credentials lack the route's scope "
               "(details.reason scope).";
    case 404:
        return "NOT_FOUND: no such run or configuration; in run mode, POST /v0/runs and "
               "DELETE /v0/runs/{id}.";
    case 409:
        return "INVALID_STATE: the run, its run record or the server does not allow it now.";
    case 413:
        return "INVALID_ARGUMENT: the request's body is larger than 64 KiB, the limit of a "
               "listener; it is refused before the route sees it.";
    case 415:
        return "INVALID_ARGUMENT: the POST does not name exactly one Content-Type "
               "application/json, its charset, if any, utf-8; every POST needs it, also one "
               "with an empty body, as stop's may be.";
    case 422:
        return "The configuration did not validate: the error document of Runtime::open(), its "
               "validation report in details.";
    case 503:
        return "RESOURCE_EXHAUSTED: as many runs as --max-runs are held.";
    default:
        return "An error of category execution, environment or internal; INTERNAL, without its "
               "text, for anything unexpected.";
    }
}

std::string success_description(int status) {
    switch (status) {
    case 201:
        return "The run was opened and started: its entry. Location names it.";
    case 202:
        return "The stop was requested: the run's entry. The run ends in its own time.";
    case 204:
        return "The run is forgotten; its files are kept.";
    default:
        return "The answer.";
    }
}

json error_response(int status) {
    json response = {{"description", error_description(status)},
                     {"content", {{"application/json", {{"schema", ref("Error")}}}}}};
    if (status == 401) {
        response["headers"] = {
            {"WWW-Authenticate", {{"schema", {{"type", "string"}, {"enum", {"Bearer"}}}}}}};
    }
    return response;
}

json operation(const Route& route) {
    json result = {{"operationId", std::string(route.operation_id)},
                   {"summary", std::string(route.summary)},
                   {"x-praenora-scope", std::string(scope_name(route.scope))}};
    if (route.path.find("{id}") != std::string_view::npos) {
        result["parameters"] = {{{"name", "id"},
                                 {"in", "path"},
                                 {"required", true},
                                 {"description", "The run id: decimal, without leading zeros."},
                                 {"schema", {{"type", "string"}}}}};
    }
    if (!route.request_schema.empty()) {
        result["requestBody"] = {
            {"required", route.request_required},
            {"description",
             route.request_required
                 ? "A JSON object, with Content-Type: application/json."
                 : "Empty, or a JSON object. Content-Type: application/json is required all the "
                   "same: a POST without it answers 415, also one with an empty body."},
            {"content", {{"application/json", {{"schema", ref(route.request_schema)}}}}}};
    }
    json responses = json::object();
    json success = {{"description", success_description(route.success_status)}};
    if (!route.success_media.empty()) {
        success["content"] = {
            {std::string(route.success_media), {{"schema", ref(route.success_schema)}}}};
    }
    if (route.success_status == 201) {
        success["headers"] = {{"Location",
                               {{"description", "The new run's path, /v0/runs/<id>."},
                                {"schema", {{"type", "string"}}}}}};
    }
    responses[std::to_string(route.success_status)] = std::move(success);
    for (const int status : route.error_statuses) {
        responses[std::to_string(status)] = error_response(status);
    }
    responses["500"] = error_response(500);
    result["responses"] = std::move(responses);
    if (route.scope == RouteScope::none) {
        result["security"] = json::array();
    }
    return result;
}

} // namespace

std::string openapi_document(const Runtime& runtime) {
    json paths = json::object();
    for (const Route& route : routes()) {
        paths[std::string(route.path)][frontend::lower(route.method)] = operation(route);
    }
    json schemas = own_schemas();
    for (const char* kind : {"status-snapshot", "run-record", "pmp-envelope"}) {
        schemas[kind] = json::parse(runtime.schema(kind));
    }
    const json document = {
        {"openapi", "3.1.0"},
        {"info",
         {{"title", "Praenora control API"},
          {"version", std::string(kApiVersion)},
          {"license", {{"name", "MIT"}, {"identifier", "MIT"}}},
          {"description",
           "The control and status API of the Praenora runtime, served by praenora run --listen "
           "(run mode: the one run of the process, id 1) and praenora serve (serve mode: runs "
           "opened on request from configurations under a configuration root). Over a Unix "
           "socket every request has the read and control scopes; over TCP without tokens only "
           "the read scope; over TCP with tokens, every request but GET /v0/health needs "
           "\"Authorization: Bearer <token>\". TCP requests need a Host header that names the "
           "server. A known path with another method answers 405 with Allow. Every POST, stop "
           "included, needs Content-Type: application/json (stop's body may be empty), else 415. "
           "Bodies are compact JSON in UTF-8; errors are error documents. The runtime's schemas "
           "below are JSON Schema 2020-12, embedded with their own $schema and $id."}}},
        {"security", {{{"bearer", json::array()}}, json::object()}},
        {"paths", std::move(paths)},
        {"components",
         {{"securitySchemes", {{"bearer", {{"type", "http"}, {"scheme", "bearer"}}}}},
          {"schemas", std::move(schemas)}}}};
    return document.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
}

} // namespace praenora::server
