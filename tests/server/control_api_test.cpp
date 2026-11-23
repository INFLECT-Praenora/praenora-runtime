// The control API's answers (src/server/control_api.hpp) over a fake run source: every route and
// its status in run and serve mode, the run entry, scopes and tokens, the Host check, media types,
// methods, the checks of a body, and the HTTP status of every error.
#include "control_api.hpp"
#include "openapi.hpp"
#include "prometheus.hpp"

#include "server_fixtures.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/runtime.hpp>
#include <praenora/runtime/status.hpp>
#include <praenora/runtime/version.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using nlohmann::json;
using praenora::Error;
using praenora::ErrorCode;
using praenora::Runtime;
using praenora::server::ControlApi;
using praenora::server::ControlApiSettings;
using praenora::server::http_status_of;
using praenora::server::ListenerKind;
using praenora::server::parse_listen_address;
using praenora::server::Request;
using praenora::server::Response;
using praenora::server::ServerMode;
using praenora::server::Tokens;
using praenora::test::body_of;
using praenora::test::error_of;
using praenora::test::FakeSource;
using praenora::test::post_json;
using praenora::test::request;

namespace {

const std::string kControl = "control-token-0123456789abcdefghijklmnop";
const std::string kRead = "read-token-ABCDEFGHIJKLMNOPQRSTUVWXYZ+/=~._";
constexpr std::int64_t kStartedAt = 1700000000123456789LL;

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

const Runtime& runtime() {
    static const Runtime instance(praenora::test::fixture_runtime_options());
    return instance;
}

// Settings of a listener on `address` with `tokens`.
ControlApiSettings settings(const std::string& address = "127.0.0.1:8080", Tokens tokens = {},
                            std::vector<std::string> allow_hosts = {}) {
    ControlApiSettings result;
    result.listener = parse_listen_address(address);
    result.tokens = std::move(tokens);
    result.allow_hosts = std::move(allow_hosts);
    result.started_at_unix_ns = kStartedAt;
    result.pid = 4242;
    return result;
}

// A fake source of `mode` holding runs 1 and 2.
std::unique_ptr<FakeSource> source_with_runs(ServerMode mode) {
    auto source = std::make_unique<FakeSource>(mode);
    source->runs.push_back(praenora::test::fake_run(1, mode));
    source->runs.push_back(praenora::test::fake_run(2, mode));
    return source;
}

// A TCP request with the listener's Host and `headers`.
Request tcp(std::string method, std::string path,
            std::vector<std::pair<std::string, std::string>> headers = {}, std::string body = "") {
    headers.emplace_back("host", "127.0.0.1:8080");
    return request(std::move(method), std::move(path), ListenerKind::tcp, std::move(headers),
                   std::move(body));
}

// Checks that `response` is the error document of `code` with `status`.
void expect_error(const Response& response, int status, ErrorCode code) {
    EXPECT_EQ(response.status, status) << response.body;
    EXPECT_EQ(response.content_type, "application/json; charset=utf-8");
    const json body = body_of(response);
    EXPECT_EQ(body.at("code"), static_cast<int>(code)) << response.body;
    EXPECT_EQ(body.at("name"), std::string(praenora::error_code_info(code).name));
    EXPECT_TRUE(body.contains("message"));
    EXPECT_TRUE(body.contains("location"));
    EXPECT_TRUE(body.contains("details"));
}

} // namespace

TEST(ControlApi, HealthAnswersOkWithoutCredentials) {
    FakeSource source(ServerMode::serve);
    const ControlApi api(runtime(), source, settings("127.0.0.1:8080", {kControl, kRead}));
    const Response response = api.handle(tcp("GET", "/v0/health"));
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.content_type, "application/json; charset=utf-8");
    EXPECT_EQ(response.body, R"({"status":"ok"})");
    // Even with a token that is not known; but not without the Host check.
    EXPECT_EQ(api.handle(tcp("GET", "/v0/health", {{"authorization", "Bearer x"}})).status, 200);
    expect_error(api.handle(request("GET", "/v0/health", ListenerKind::tcp)), 403,
                 ErrorCode::PERMISSION_DENIED);
}

TEST(ControlApi, ServerInfoReportsModeAndTheRequestsScopes) {
    for (const ServerMode mode : {ServerMode::run, ServerMode::serve}) {
        FakeSource source(mode);
        const ControlApi api(runtime(), source, settings());
        const Response response = api.handle(tcp("GET", "/v0/server"));
        ASSERT_EQ(response.status, 200) << response.body;
        EXPECT_EQ(body_of(response), json({{"api_version", "0.1.0"},
                                           {"runtime_version", praenora::version_info().runtime},
                                           {"mode", mode == ServerMode::run ? "run" : "serve"},
                                           {"pid", 4242},
                                           {"started_at", "2023-11-14T22:13:20.123456789Z"},
                                           {"scopes", {"read"}}}));
        EXPECT_EQ(body_of(api.handle(request("GET", "/v0/server"))).at("scopes"),
                  json({"control", "read"}));
    }
}

TEST(ControlApi, OpenApiRouteServesTheCompactDocument) {
    FakeSource source(ServerMode::serve);
    const ControlApi api(runtime(), source, settings());
    const Response response = api.handle(request("GET", "/v0/openapi.json"));
    ASSERT_EQ(response.status, 200);
    EXPECT_EQ(response.content_type, "application/json; charset=utf-8");
    EXPECT_EQ(response.body, json::parse(praenora::server::openapi_document(runtime())).dump());
}

TEST(ControlApi, ListsRunsWithEveryEntryField) {
    for (const ServerMode mode : {ServerMode::run, ServerMode::serve}) {
        const auto source = source_with_runs(mode);
        source->runs.at(1)->snapshot_.state = praenora::WorkflowState::failed;
        source->runs.at(1)->snapshot_.health = praenora::Health::unhealthy;
        const ControlApi api(runtime(), *source, settings());
        const Response response = api.handle(request("GET", "/v0/runs"));
        ASSERT_EQ(response.status, 200) << response.body;
        const json runs = body_of(response).at("runs");
        ASSERT_EQ(runs.size(), 2u);
        const bool serve = mode == ServerMode::serve;
        EXPECT_EQ(runs.at(0), json({{"id", "1"},
                                    {"run_number", 1},
                                    {"configuration", serve ? "sub/single.yaml" : "single.yaml"},
                                    {"workflow", {{"name", "test.single"}, {"version", "0.1.0"}}},
                                    {"fingerprint", std::string(64, 'a')},
                                    {"state", "running"},
                                    {"health", "healthy"},
                                    {"ticks_completed", 3},
                                    {"created_at", "2026-11-22T10:00:00.000000000Z"},
                                    {"output", serve ? json("run-1") : json(nullptr)}}));
        EXPECT_EQ(runs.at(1).at("id"), "2");
        EXPECT_EQ(runs.at(1).at("state"), "failed");
        EXPECT_EQ(runs.at(1).at("health"), "unhealthy");
    }
    FakeSource empty(ServerMode::serve);
    const ControlApi api(runtime(), empty, settings());
    EXPECT_EQ(api.handle(request("GET", "/v0/runs")).body, R"({"runs":[]})");
}

TEST(ControlApi, CreateAnswers201WithTheEntryAndLocation) {
    FakeSource source(ServerMode::serve);
    const ControlApi api(runtime(), source, settings());
    const Response response = api.handle(
        post_json("/v0/runs", R"({"configuration":"sub/single.yaml","seed":9007199254740991,)"
                              R"("ticks":0,"trace_messages":true,"isolation":"process"})"));
    ASSERT_EQ(response.status, 201) << response.body;
    ASSERT_NE(response.header("Location"), nullptr);
    EXPECT_EQ(*response.header("Location"), "/v0/runs/1");
    EXPECT_EQ(body_of(response).at("id"), "1");
    EXPECT_EQ(body_of(response).at("output"), "run-1");
    ASSERT_EQ(source.created.size(), 1u);
    const auto& created = source.created.front();
    EXPECT_EQ(created.configuration, "sub/single.yaml");
    EXPECT_EQ(created.seed, std::optional<std::uint64_t>(9007199254740991ULL));
    EXPECT_EQ(created.ticks, std::optional<std::uint64_t>(0));
    EXPECT_EQ(created.trace_messages, std::optional<bool>(true));
    EXPECT_EQ(created.isolation,
              std::optional<praenora::IsolationMode>(praenora::IsolationMode::process));

    // Only the configuration is required; -0 is read as 0, as the runtime reads it.
    ASSERT_EQ(api.handle(post_json("/v0/runs", R"({"configuration":"a.yaml"})")).status, 201);
    EXPECT_FALSE(source.created.back().seed.has_value());
    EXPECT_FALSE(source.created.back().isolation.has_value());
    ASSERT_EQ(api.handle(post_json("/v0/runs", R"({"configuration":"a.yaml","seed":-0})")).status,
              201);
    EXPECT_EQ(source.created.back().seed, std::optional<std::uint64_t>(0));

    // What the source refuses answers by its code.
    source.create_error = Error(ErrorCode::RESOURCE_EXHAUSTED, "16 runs are held");
    expect_error(api.handle(post_json("/v0/runs", R"({"configuration":"a.yaml"})")), 503,
                 ErrorCode::RESOURCE_EXHAUSTED);
}

TEST(ControlApi, CreateBodyIsChecked) {
    FakeSource source(ServerMode::serve);
    const ControlApi api(runtime(), source, settings());
    const std::pair<const char*, const char*> refused[] = {
        {R"({"configuration":"a.yaml","colour":1})", "colour"},
        {R"({"configuration":"a.yaml","seed":9007199254740992})", "seed"},
        {R"({"configuration":"a.yaml","seed":-1})", "seed"},
        {R"({"configuration":"a.yaml","seed":1.5})", "seed"},
        {R"({"configuration":"a.yaml","seed":1.0})", "seed"},
        {R"({"configuration":"a.yaml","seed":"1"})", "seed"},
        {R"({"configuration":"a.yaml","seed":18446744073709551616})", "seed"},
        {R"({"configuration":"a.yaml","ticks":null})", "ticks"},
        {R"({"configuration":"a.yaml","trace_messages":1})", "trace_messages"},
        {R"({"configuration":"a.yaml","isolation":"thread"})", "isolation"},
        {R"({"configuration":"a.yaml","isolation":true})", "isolation"},
        {R"({"configuration":7})", "configuration"},
        {R"({"configuration":""})", "configuration"},
        {R"({"seed":1})", "configuration"},
        {R"(["configuration"])", "object"},
        {R"(not json)", "JSON"},
        {"", "JSON"},
    };
    for (const auto& [body, named] : refused) {
        SCOPED_TRACE(body);
        const Response response = api.handle(post_json("/v0/runs", body));
        expect_error(response, 400, ErrorCode::INVALID_ARGUMENT);
        EXPECT_TRUE(contains(body_of(response).at("message").get<std::string>(), named))
            << response.body;
    }
    EXPECT_TRUE(source.created.empty());
    const Response unknown =
        api.handle(post_json("/v0/runs", R"({"configuration":"a.yaml","colour":1})"));
    EXPECT_EQ(body_of(unknown).at("location").at("pointer"), "/colour");
}

TEST(ControlApi, RunModeAnswers404ForCreateAndDelete) {
    const auto source = source_with_runs(ServerMode::run);
    const ControlApi api(runtime(), *source, settings());
    expect_error(api.handle(post_json("/v0/runs", R"({"configuration":"a.yaml"})")), 404,
                 ErrorCode::NOT_FOUND);
    // Before the body is looked at.
    expect_error(api.handle(post_json("/v0/runs", "not json")), 404, ErrorCode::NOT_FOUND);
    expect_error(api.handle(request("DELETE", "/v0/runs/1")), 404, ErrorCode::NOT_FOUND);
    EXPECT_TRUE(source->created.empty());
    EXPECT_TRUE(source->removed.empty());
    EXPECT_EQ(source->runs.size(), 2u);
}

TEST(ControlApi, RunQueriesAnswerTheRunsDocuments) {
    const auto source = source_with_runs(ServerMode::serve);
    auto& run = *source->runs.at(0);
    run.status_json_ = R"({"kind":"status-snapshot","pid":1})";
    run.metrics_json_ = R"({"metrics":[{"name":"x"}]})";
    run.outputs_json_ = R"({"state":{"id":"e"}})";
    run.run_record_json_ = "{\n  \"kind\": \"run-record\"\n}\n";
    const ControlApi api(runtime(), *source, settings());
    const auto body = [&](const std::string& path) {
        const Response response = api.handle(request("GET", path));
        EXPECT_EQ(response.status, 200) << path << ": " << response.body;
        EXPECT_EQ(response.content_type, "application/json; charset=utf-8");
        return response.body;
    };
    EXPECT_EQ(body("/v0/runs/1"), run.status_json_);
    EXPECT_EQ(body("/v0/runs/1/metrics"), run.metrics_json_);
    EXPECT_EQ(body("/v0/runs/1/outputs"), run.outputs_json_);
    // Compact, whatever layout the run record has.
    EXPECT_EQ(body("/v0/runs/1/record"), R"({"kind":"run-record"})");
}

TEST(ControlApi, RecordBeforeItExistsIs409) {
    const auto source = source_with_runs(ServerMode::serve);
    const ControlApi api(runtime(), *source, settings());
    expect_error(api.handle(request("GET", "/v0/runs/2/record")), 409, ErrorCode::INVALID_STATE);
}

TEST(ControlApi, StopAnswers202TheEntryAndIsIdempotent) {
    for (const ServerMode mode : {ServerMode::run, ServerMode::serve}) {
        const auto source = source_with_runs(mode);
        const ControlApi api(runtime(), *source, settings());
        // The body may be empty or an empty object; the media type is JSON all the same.
        const Response first = api.handle(post_json("/v0/runs/2/stop", ""));
        ASSERT_EQ(first.status, 202) << first.body;
        EXPECT_EQ(body_of(first).at("id"), "2");
        EXPECT_EQ(api.handle(post_json("/v0/runs/2/stop", "{}")).status, 202);
        EXPECT_EQ(api.handle(post_json("/v0/runs/2/stop", "")).status, 202);
        EXPECT_EQ(source->runs.at(1)->stops.load(), 3);
        // Without Content-Type, an empty body too: a browser would send it without a preflight.
        expect_error(api.handle(request("POST", "/v0/runs/2/stop")), 415,
                     ErrorCode::INVALID_ARGUMENT);
        EXPECT_EQ(source->runs.at(1)->stops.load(), 3);
        EXPECT_EQ(source->runs.at(0)->stops.load(), 0);
        expect_error(api.handle(post_json("/v0/runs/2/stop", R"({"now":true})")), 400,
                     ErrorCode::INVALID_ARGUMENT);
        expect_error(api.handle(post_json("/v0/runs/2/stop", "[]")), 400,
                     ErrorCode::INVALID_ARGUMENT);
        expect_error(api.handle(post_json("/v0/runs/9/stop", "")), 404, ErrorCode::NOT_FOUND);
    }
}

TEST(ControlApi, DeleteAnswers204OrTheSourcesRefusal) {
    const auto source = source_with_runs(ServerMode::serve);
    const ControlApi api(runtime(), *source, settings());
    const Response deleted = api.handle(request("DELETE", "/v0/runs/1"));
    EXPECT_EQ(deleted.status, 204);
    EXPECT_EQ(deleted.body, "");
    EXPECT_EQ(deleted.content_type, "");
    EXPECT_EQ(source->removed, std::vector<std::string>{"1"});
    expect_error(api.handle(request("DELETE", "/v0/runs/1")), 404, ErrorCode::NOT_FOUND);
    source->remove_error = Error(ErrorCode::INVALID_STATE, "run 2 is running");
    expect_error(api.handle(request("DELETE", "/v0/runs/2")), 409, ErrorCode::INVALID_STATE);
}

TEST(ControlApi, UnknownRunIs404) {
    const auto source = source_with_runs(ServerMode::serve);
    const ControlApi api(runtime(), *source, settings());
    for (const char* id : {"3", "0", "01", "abc", "-1", "1.0", "%31"}) {
        SCOPED_TRACE(id);
        for (const std::string suffix : {"", "/metrics", "/record", "/outputs"}) {
            expect_error(api.handle(request("GET", std::string("/v0/runs/") + id + suffix)), 404,
                         ErrorCode::NOT_FOUND);
        }
        expect_error(api.handle(post_json(std::string("/v0/runs/") + id + "/stop", "")), 404,
                     ErrorCode::NOT_FOUND);
    }
    EXPECT_TRUE(source->removed.empty());
}

TEST(ControlApi, UnknownPathIs404AndOtherMethodIs405WithAllow) {
    FakeSource source(ServerMode::serve);
    const ControlApi api(runtime(), source, settings());
    for (const char* path : {"/", "/v0", "/v0/", "/v0/runs/", "/v0/runs/1/", "/v0/runs/1/x",
                             "/v1/health", "/metrics/", "//v0/health", "v0/health", ""}) {
        SCOPED_TRACE(path);
        expect_error(api.handle(request("GET", path)), 404, ErrorCode::NOT_FOUND);
    }
    const std::pair<std::pair<const char*, const char*>, const char*> other[] = {
        {{"PUT", "/v0/runs"}, "GET, POST"},
        {{"DELETE", "/v0/health"}, "GET"},
        {{"OPTIONS", "/v0/runs/1"}, "GET, DELETE"},
        {{"GET", "/v0/runs/1/stop"}, "POST"},
        {{"POST", "/metrics"}, "GET"},
        {{"get", "/v0/health"}, "GET"},
        {{"HEAD", "/v0/server"}, "GET"},
    };
    for (const auto& [call, allow] : other) {
        SCOPED_TRACE(std::string(call.first) + " " + call.second);
        const Response response = api.handle(request(call.first, call.second));
        expect_error(response, 405, ErrorCode::INVALID_ARGUMENT);
        ASSERT_NE(response.header("Allow"), nullptr);
        EXPECT_EQ(*response.header("Allow"), allow);
    }
}

TEST(ControlApi, PostWithoutJsonIs415) {
    const auto source = source_with_runs(ServerMode::serve);
    const ControlApi api(runtime(), *source, settings());
    const std::string body = R"({"configuration":"a.yaml"})";
    expect_error(api.handle(request("POST", "/v0/runs", ListenerKind::unix_socket, {}, body)), 415,
                 ErrorCode::INVALID_ARGUMENT);
    for (const char* type :
         {"text/plain", "application/x-www-form-urlencoded", "multipart/form-data; boundary=x",
          "application/json-seq", "application/json; charset=latin1", ""}) {
        SCOPED_TRACE(type);
        expect_error(api.handle(request("POST", "/v0/runs", ListenerKind::unix_socket,
                                        {{"content-type", type}}, body)),
                     415, ErrorCode::INVALID_ARGUMENT);
        for (const char* stop_body : {"{}", ""}) {
            expect_error(api.handle(request("POST", "/v0/runs/1/stop", ListenerKind::unix_socket,
                                            {{"content-type", type}}, stop_body)),
                         415, ErrorCode::INVALID_ARGUMENT);
        }
    }
    EXPECT_EQ(source->runs.at(0)->stops.load(), 0);
    expect_error(
        api.handle(request(
            "POST", "/v0/runs", ListenerKind::unix_socket,
            {{"content-type", "application/json"}, {"content-type", "application/json"}}, body)),
        415, ErrorCode::INVALID_ARGUMENT);
    for (const char* type :
         {"application/json", "Application/JSON", "application/json; charset=utf-8",
          "application/json;charset=\"UTF-8\"", " application/json "}) {
        SCOPED_TRACE(type);
        EXPECT_EQ(api.handle(request("POST", "/v0/runs", ListenerKind::unix_socket,
                                     {{"content-type", type}}, body))
                      .status,
                  201);
    }
    EXPECT_TRUE(source->runs.size() == 7u);
}

TEST(ControlApi, PrometheusRouteAnswersEveryRun) {
    const auto source = source_with_runs(ServerMode::serve);
    for (auto& run : source->runs) {
        run->metrics_json_ =
            R"({"metrics":[{"name":"praenora.workflow.ticks","kind":"counter","unit":"{tick}",)"
            R"("attributes":{"praenora.workflow":"test.single@0.1.0"},"value":3}]})";
    }
    const ControlApi api(runtime(), *source, settings());
    const Response response = api.handle(request("GET", "/metrics"));
    ASSERT_EQ(response.status, 200);
    EXPECT_EQ(response.content_type, "text/plain; version=0.0.4; charset=utf-8");
    std::vector<praenora::server::RunMetrics> runs;
    for (const auto& run : source->runs) {
        runs.push_back({run->facts().run_number, run->snapshot_.fingerprint, run->metrics_json_});
    }
    EXPECT_EQ(response.body, praenora::server::prometheus_text(runs));
    EXPECT_TRUE(contains(response.body, R"(praenora_run_number="2")")) << response.body;
}

TEST(ControlApi, TcpWithoutTokensIsReadOnly) {
    const auto source = source_with_runs(ServerMode::serve);
    const ControlApi api(runtime(), *source, settings());
    for (const char* path : {"/v0/server", "/v0/openapi.json", "/v0/runs", "/v0/runs/1",
                             "/v0/runs/1/metrics", "/v0/runs/1/outputs", "/metrics"}) {
        SCOPED_TRACE(path);
        EXPECT_EQ(api.handle(tcp("GET", path)).status, 200);
    }
    const std::vector<Request> control = {
        tcp("POST", "/v0/runs", {{"content-type", "application/json"}},
            R"({"configuration":"a.yaml"})"),
        tcp("POST", "/v0/runs/1/stop"), tcp("DELETE", "/v0/runs/1")};
    for (const Request& call : control) {
        SCOPED_TRACE(call.method + " " + call.path);
        const Response response = api.handle(call);
        expect_error(response, 403, ErrorCode::PERMISSION_DENIED);
        const json body = body_of(response);
        EXPECT_EQ(body.at("details").at("reason"), "scope");
        const std::string message = body.at("message");
        EXPECT_TRUE(contains(message, "token") && contains(message, "Unix socket")) << message;
    }
    EXPECT_TRUE(source->created.empty());
    EXPECT_EQ(source->runs.at(0)->stops.load(), 0);
    EXPECT_TRUE(source->removed.empty());
}

TEST(ControlApi, TcpWithTokensNeedsABearerToken) {
    const auto source = source_with_runs(ServerMode::serve);
    const ControlApi api(runtime(), *source, settings("127.0.0.1:8080", {kControl, kRead}));
    for (const std::vector<std::pair<std::string, std::string>>& headers :
         {std::vector<std::pair<std::string, std::string>>{},
          {{"authorization", "Bearer wrong-token-0123456789abcdefghijklmnop"}},
          {{"authorization", "Basic " + kControl}}}) {
        for (const char* path : {"/v0/server", "/v0/runs", "/v0/runs/1", "/metrics", "/nowhere"}) {
            SCOPED_TRACE(path);
            const Response response = api.handle(tcp("GET", path, headers));
            expect_error(response, 401, ErrorCode::UNAUTHENTICATED);
            ASSERT_NE(response.header("WWW-Authenticate"), nullptr);
            EXPECT_EQ(*response.header("WWW-Authenticate"), "Bearer");
        }
    }
    const auto with = [](const std::string& token) {
        return std::vector<std::pair<std::string, std::string>>{
            {"authorization", "Bearer " + token}};
    };
    EXPECT_EQ(api.handle(tcp("GET", "/v0/runs/1", with(kRead))).status, 200);
    EXPECT_EQ(body_of(api.handle(tcp("GET", "/v0/server", with(kRead)))).at("scopes"),
              json({"read"}));
    EXPECT_EQ(body_of(api.handle(tcp("GET", "/v0/server", with(kControl)))).at("scopes"),
              json({"control", "read"}));
    const Response read_stop = api.handle(tcp("POST", "/v0/runs/1/stop", with(kRead)));
    expect_error(read_stop, 403, ErrorCode::PERMISSION_DENIED);
    EXPECT_TRUE(contains(body_of(read_stop).at("message"), "read token")) << read_stop.body;
    EXPECT_EQ(source->runs.at(0)->stops.load(), 0);
    const auto json_with = [&](const std::string& token) {
        auto headers = with(token);
        headers.emplace_back("content-type", "application/json");
        return headers;
    };
    EXPECT_EQ(api.handle(tcp("POST", "/v0/runs/1/stop", json_with(kControl))).status, 202);
    EXPECT_EQ(source->runs.at(0)->stops.load(), 1);
    EXPECT_EQ(api.handle(tcp("DELETE", "/v0/runs/1", with(kControl))).status, 204);
    // A known path with another method needs the credentials first.
    expect_error(api.handle(tcp("PUT", "/v0/runs")), 401, ErrorCode::UNAUTHENTICATED);
    expect_error(api.handle(tcp("PUT", "/v0/runs", with(kRead))), 405, ErrorCode::INVALID_ARGUMENT);
    // A Unix socket ignores tokens: its mode is the credential.
    EXPECT_EQ(api.handle(post_json("/v0/runs/2/stop", "")).status, 202);
}

TEST(ControlApi, HostCheckProtectsTcpListeners) {
    const auto source = source_with_runs(ServerMode::serve);
    const ControlApi api(runtime(), *source,
                         settings("127.0.0.1:8080", {}, {"praenora.example.org"}));
    const auto with_host = [&](std::vector<std::pair<std::string, std::string>> headers) {
        return api.handle(request("GET", "/v0/runs", ListenerKind::tcp, std::move(headers)));
    };
    for (const std::vector<std::pair<std::string, std::string>>& refused :
         {std::vector<std::pair<std::string, std::string>>{},
          {{"host", "evil.example:8080"}},
          {{"host", "127.0.0.1:9090"}},
          {{"host", "127.0.0.1:8080"}, {"host", "127.0.0.1:8080"}}}) {
        const Response response = with_host(refused);
        expect_error(response, 403, ErrorCode::PERMISSION_DENIED);
        EXPECT_EQ(body_of(response).at("details").at("reason"), "host");
    }
    EXPECT_EQ(with_host({{"host", "localhost:8080"}}).status, 200);
    EXPECT_EQ(with_host({{"host", "LOCALHOST"}}).status, 200);
    EXPECT_EQ(with_host({{"host", "praenora.example.org"}}).status, 200);
    EXPECT_EQ(with_host({{"host", "127.0.0.1"}}).status, 200);
    // Before the credentials: a request from a rebound name learns nothing.
    const ControlApi tokens(runtime(), *source, settings("127.0.0.1:8080", {kControl, ""}));
    expect_error(
        tokens.handle(request("GET", "/v0/runs", ListenerKind::tcp, {{"host", "evil.example"}})),
        403, ErrorCode::PERMISSION_DENIED);
    // localhost is a loopback listener's name only.
    const ControlApi wide(runtime(), *source, settings("192.0.2.10:8080", {kControl, ""}));
    expect_error(
        wide.handle(request("GET", "/v0/health", ListenerKind::tcp, {{"host", "localhost:8080"}})),
        403, ErrorCode::PERMISSION_DENIED);
    EXPECT_EQ(
        wide.handle(request("GET", "/v0/health", ListenerKind::tcp, {{"host", "192.0.2.10:8080"}}))
            .status,
        200);
    // A Unix socket has no Host check.
    EXPECT_EQ(api.handle(request("GET", "/v0/runs")).status, 200);
    EXPECT_EQ(api.handle(request("GET", "/v0/runs", ListenerKind::unix_socket,
                                 {{"host", "evil.example"}}))
                  .status,
              200);
}

TEST(ControlApi, UnexpectedExceptionIs500WithoutItsText) {
    FakeSource source(ServerMode::serve);
    source.on_list = [] { throw std::runtime_error("secret detail"); };
    const ControlApi api(runtime(), source, settings());
    const Response response = api.handle(request("GET", "/v0/runs"));
    expect_error(response, 500, ErrorCode::INTERNAL);
    EXPECT_FALSE(contains(response.body, "secret")) << response.body;
    source.on_list = [] { throw Error(ErrorCode::IO_ERROR, "the disk is gone"); };
    const Response io = api.handle(request("GET", "/v0/runs"));
    expect_error(io, 500, ErrorCode::IO_ERROR);
    EXPECT_TRUE(contains(io.body, "the disk is gone")) << io.body;
}

TEST(ControlApi, ConstructionChecksTheSettings) {
    FakeSource source(ServerMode::serve);
    const auto refusal = [&](const ControlApiSettings& refused) {
        return error_of([&] { const ControlApi api(runtime(), source, refused); }).code();
    };
    EXPECT_EQ(refusal(settings("127.0.0.1:0", {}, {"bad host"})), ErrorCode::INVALID_ARGUMENT);
    EXPECT_EQ(refusal(settings("192.0.2.10:0")), ErrorCode::INVALID_ARGUMENT);
#ifndef _WIN32
    EXPECT_EQ(refusal(settings("unix:/tmp/p.sock", {kControl, ""})), ErrorCode::INVALID_ARGUMENT);
#endif
}

TEST(HttpStatus, MapsCodesThenCategories) {
    const std::pair<ErrorCode, int> table[] = {
        {ErrorCode::UNAUTHENTICATED, 401},
        {ErrorCode::PERMISSION_DENIED, 403},
        {ErrorCode::NOT_FOUND, 404},
        {ErrorCode::INVALID_STATE, 409},
        {ErrorCode::RESOURCE_EXHAUSTED, 503},
        {ErrorCode::INVALID_ARGUMENT, 400},         // usage
        {ErrorCode::CONFIG_INVALID, 422},           // validation
        {ErrorCode::MODULE_NOT_FOUND, 422},         // validation
        {ErrorCode::UNSUPPORTED, 422},              // compatibility
        {ErrorCode::SERVICE_UNAVAILABLE, 422},      // compatibility
        {ErrorCode::LIBRARY_LOAD_FAILED, 422},      // load
        {ErrorCode::MODULE_IDENTITY_MISMATCH, 422}, // load
        {ErrorCode::DATA_INVALID, 422},             // data
        {ErrorCode::TARGET_DATA_INVALID, 422},      // data
        {ErrorCode::TIMEOUT, 504},                  // timeout
        {ErrorCode::STEP_TIMEOUT, 504},             // timeout
        {ErrorCode::CANCELLED, 409},                // cancelled
        {ErrorCode::RUN_STOPPED, 409},              // cancelled
        {ErrorCode::STEP_FAILED, 500},              // execution
        {ErrorCode::MODULE_CRASHED, 500},           // execution
        {ErrorCode::IO_ERROR, 500},                 // environment
        {ErrorCode::MODULE_HOST_UNAVAILABLE, 422},  // load
        {ErrorCode::INTERNAL, 500},                 // internal
        {ErrorCode::UNKNOWN, 500},                  // internal
    };
    for (const auto& [code, status] : table) {
        SCOPED_TRACE(std::string(praenora::error_code_info(code).name));
        EXPECT_EQ(http_status_of(Error(code, "x")), status);
    }
    // Every code has a status.
    for (const praenora::ErrorCodeInfo& info : praenora::all_error_codes()) {
        const int status = http_status_of(Error(info.code, "x"));
        EXPECT_TRUE(status >= 400 && status <= 599) << info.name << ": " << status;
    }
}

TEST(HttpStatus, ValidationReportIs422WhateverItsFirstCode) {
    // As Runtime::open() throws it: the first error's code, the whole report in the details.
    const std::string report =
        R"({"ok":false,"diagnostics":[{"severity":"error","code":4,"name":"NOT_FOUND",)"
        R"("category":"environment","message":"no data file","location":{}}]})";
    EXPECT_EQ(http_status_of(Error(ErrorCode::NOT_FOUND, "workflow validation failed: 1 error(s)",
                                   {}, report)),
              422);
    EXPECT_EQ(http_status_of(Error(ErrorCode::IO_ERROR, "x", {}, report)), 422);
    EXPECT_EQ(http_status_of(Error(ErrorCode::NOT_FOUND, "x", {}, R"({"diagnostics":3})")), 404);
}
