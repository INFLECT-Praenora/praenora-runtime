// The control API's listener and client (src/server/listener.hpp, client.hpp) in the test's own
// process, over real connections (label server): a TCP listener on 127.0.0.1 at the port the
// system chose, the request as the handler gets it and the answer as the client gets it, the
// limits (a body over 64 KiB is 413), the errors of the HTTP layer as error documents, raw
// requests (HTTP/1.0 without Host, two Host headers, errors closing their connections), the
// credentials and the Host check through ControlApi, stopping (in-flight requests answered, no
// handler afterwards, the port refusing), queries racing a DELETE and a shutdown of a registry
// (run under ThreadSanitizer too), and the client's errors (on POSIX, a connection reset while
// the request is written, which fails it without SIGPIPE ending the process, in the child process
// of a death test); on POSIX, Unix sockets: mode 0600, a
// stale socket replaced, a live one and a file of another kind refused, a path that the socket
// address cannot hold refused, the socket removed at the end. Where local listeners are refused (a
// sandbox's EPERM), each test skips, saying "local listener refused:", or fails with
// PRAENORA_TESTS_REQUIRE_LISTENERS=1.
#include "listener_fixtures.hpp"

#include "client.hpp"
#include "clock.hpp"
#include "control_api.hpp"
#include "listener.hpp"
#include "run_registry.hpp"

#include "server_fixtures.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/runtime.hpp>
#include <praenora/runtime/status.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Error;
using praenora::ErrorCode;
using praenora::Runtime;
using praenora::WorkflowState;
using praenora::server::ClientAnswer;
using praenora::server::ControlApi;
using praenora::server::ControlApiSettings;
using praenora::server::Listener;
using praenora::server::ListenerKind;
using praenora::server::parse_listen_address;
using praenora::server::RegistrySettings;
using praenora::server::Request;
using praenora::server::Response;
using praenora::server::RunRegistry;
using praenora::test::bearer;
using praenora::test::client_of;
using praenora::test::config_step;
using praenora::test::error_of;
using praenora::test::eventually;
using praenora::test::ListenerTest;
using praenora::test::ScratchDir;
using praenora::test::SocketDir;
using praenora::test::utf8;
using praenora::test::write_fixture_config;

namespace {

const std::string kControl = "control-token-0123456789abcdefghijklmnop";
const std::string kRead = "read-token-ABCDEFGHIJKLMNOPQRSTUVWXYZ+/=~._";

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

const Runtime& fixture_runtime() {
    static const Runtime instance(praenora::test::fixture_runtime_options());
    return instance;
}

// The requests a handler got, and what it answers: the request's method, path and query, a
// header and the body, echoed as JSON.
class Recorder {
public:
    Response operator()(const Request& request) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(request);
        }
        Response response;
        response.status = 200;
        response.content_type = "application/json; charset=utf-8";
        json headers = json::object();
        for (const auto& [name, value] : request.headers) {
            headers[name].push_back(value);
        }
        response.body =
            json{
                {"method", request.method}, {"path", request.path},
                {"query", request.query},   {"headers", headers},
                {"body", request.body},     {"unix", request.listener == ListenerKind::unix_socket}}
                .dump();
        response.headers.emplace_back("X-Answer", "recorded");
        return response;
    }

    std::vector<Request> requests() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Request> requests_;
};

// A handler that blocks until released, counting the requests it answered.
class Gate {
public:
    Response operator()(const Request&) {
        std::unique_lock<std::mutex> lock(mutex_);
        ++entered_;
        changed_.notify_all();
        changed_.wait(lock, [this] { return open_; });
        ++answered_;
        Response response;
        response.content_type = "application/json; charset=utf-8";
        response.body = "{}";
        return response;
    }
    void open() {
        const std::lock_guard<std::mutex> lock(mutex_);
        open_ = true;
        changed_.notify_all();
    }
    bool wait_entered(int count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, std::chrono::seconds(30), [&] { return entered_ >= count; });
    }
    int answered() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return answered_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool open_ = false;
    int entered_ = 0;
    int answered_ = 0;
};

json body_json(const httplib::Result& result) { return json::parse(result->body); }

} // namespace

TEST_F(ListenerTest, ServesTcpOnThePortTheSystemChose) {
    Listener listener(parse_listen_address("127.0.0.1:0"));
    ASSERT_NE(listener.address().port, 0);
    EXPECT_EQ(listener.address().host(), "127.0.0.1");
    Recorder recorder;
    listener.start([&](const Request& request) { return recorder(request); });

    const auto client = client_of(listener.address().to_string());
    const auto result = client->Post("/v0/r%75ns/1/stop?x=1&y", httplib::Headers{{"X-Two", "a"}},
                                     "{\"k\":1}", "application/json");
    ASSERT_TRUE(result) << httplib::to_string(result.error());
    EXPECT_EQ(result->status, 200);
    EXPECT_EQ(result->get_header_value("Content-Type"), "application/json; charset=utf-8");
    EXPECT_EQ(result->get_header_value("X-Answer"), "recorded");
    EXPECT_FALSE(result->has_header("Access-Control-Allow-Origin"));
    const json echoed = body_json(result);
    EXPECT_EQ(echoed.at("method"), "POST");
    EXPECT_EQ(echoed.at("path"), "/v0/runs/1/stop"); // decoded
    EXPECT_EQ(echoed.at("query"), "x=1&y");
    EXPECT_EQ(echoed.at("body"), "{\"k\":1}");
    EXPECT_EQ(echoed.at("unix"), false);
    // Header names in lower case, the Host as the client sent it.
    EXPECT_EQ(echoed.at("headers").at("x-two"), json::array({"a"}));
    EXPECT_EQ(echoed.at("headers").at("content-type"), json::array({"application/json"}));
    EXPECT_EQ(echoed.at("headers").at("host"),
              json::array({"127.0.0.1:" + std::to_string(listener.address().port)}));

    // HEAD, OPTIONS and DELETE reach the handler too, with their methods.
    for (const std::string method : {"HEAD", "OPTIONS", "DELETE", "PUT", "PATCH"}) {
        httplib::Request request;
        request.method = method;
        request.path = "/v0/runs";
        const auto answer = client->send(request);
        ASSERT_TRUE(answer) << method;
        EXPECT_EQ(answer->status, 200) << method;
    }
    const auto requests = recorder.requests();
    ASSERT_EQ(requests.size(), 6u);
    EXPECT_EQ(requests[1].method, "HEAD");
    EXPECT_EQ(requests[2].method, "OPTIONS");
    EXPECT_EQ(requests[3].method, "DELETE");
}

TEST_F(ListenerTest, ServesIpv6Loopback) {
    std::optional<Listener> listener;
    try {
        listener.emplace(parse_listen_address("[::1]:0"));
    } catch (const Error& error) {
        // A machine without IPv6 on its loopback interface: nothing to test.
        GTEST_SKIP() << "no IPv6 loopback here: " << error.what();
    }
    ASSERT_NE(listener->address().port, 0);
    EXPECT_EQ(listener->address().to_string(), "[::1]:" + std::to_string(listener->address().port));
    Recorder recorder;
    listener->start([&](const Request& request) { return recorder(request); });
    const auto result = client_of(listener->address().to_string())->Get("/v0/health");
    ASSERT_TRUE(result) << httplib::to_string(result.error());
    EXPECT_EQ(body_json(result).at("headers").at("host"),
              json::array({"[::1]:" + std::to_string(listener->address().port)}));
}

TEST_F(ListenerTest, ABodyOver64KiBIs413AndTheHttpLayersErrorsAreErrorDocuments) {
    Listener listener(parse_listen_address("127.0.0.1:0"));
    Recorder recorder;
    listener.start([&](const Request& request) { return recorder(request); });
    const auto client = client_of(listener.address().to_string());

    const std::string fits(praenora::server::kMaxBodyBytes, 'x');
    const auto accepted = client->Post("/v0/runs", fits, "application/json");
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted->status, 200);
    EXPECT_EQ(body_json(accepted).at("body").get<std::string>().size(), fits.size());

    // A body declared as 70 KiB, of which one byte more than 64 KiB is sent: the listener refuses
    // it once it read past the limit, having read everything sent, so it closes the connection
    // without discarding data (a reset could drop the answer on some systems).
    const int port = listener.address().port;
    const std::string refused = praenora::test::raw_exchange(
        port,
        "POST /v0/runs HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(70 * 1024) +
            "\r\n\r\n" + std::string(praenora::server::kMaxBodyBytes + 1, 'x'));
    EXPECT_EQ(praenora::test::raw_status(refused), 413) << refused.substr(0, 300);
    EXPECT_TRUE(contains(praenora::test::raw_headers_lower(refused),
                         "content-type: application/json; charset=utf-8"))
        << refused.substr(0, 300);
    const json error = json::parse(praenora::test::raw_body(refused), nullptr, false);
    ASSERT_TRUE(error.is_object()) << refused.substr(0, 300);
    EXPECT_EQ(error.at("name"), "INVALID_ARGUMENT");
    EXPECT_TRUE(contains(error.at("message"), "64 KiB")) << error.dump();
    // A method that the HTTP layer does not know is refused before the handler.
    httplib::Request unknown;
    unknown.method = "TRACE";
    unknown.path = "/v0/runs";
    const auto traced = client->send(unknown);
    ASSERT_TRUE(traced);
    EXPECT_GE(traced->status, 400);
    EXPECT_EQ(json::parse(traced->body).at("category"), "usage");
    // Only the request that fits reached the handler.
    EXPECT_EQ(recorder.requests().size(), 1u);
}

// Requests that cpp-httplib's client does not send, over a real connection: HTTP/1.0 without a
// Host header reaches the Host check (403, reason host); two Host headers are refused by the HTTP
// layer first (400); every answer of status 400 or above closes its connection, the handler's own
// included, also when the request did not ask for it.
TEST_F(ListenerTest, RawRequestsGetTheHostCheckAndCloseOnErrors) {
    praenora::test::FakeSource source(praenora::server::ServerMode::serve);
    Listener listener(parse_listen_address("127.0.0.1:0"));
    ControlApiSettings settings;
    settings.listener = listener.address();
    const ControlApi api(fixture_runtime(), source, settings);
    listener.start([&](const Request& request) { return api.handle(request); });
    const int port = listener.address().port;
    const std::string host = "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";

    const std::string without_host =
        praenora::test::raw_exchange(port, "GET /v0/runs HTTP/1.0\r\n\r\n");
    EXPECT_EQ(praenora::test::raw_status(without_host), 403) << without_host;
    const json refused = json::parse(praenora::test::raw_body(without_host), nullptr, false);
    ASSERT_TRUE(refused.is_object()) << without_host;
    EXPECT_EQ(refused.at("name"), "PERMISSION_DENIED");
    EXPECT_EQ(refused.at("details").at("reason"), "host");

    const std::string two_hosts =
        praenora::test::raw_exchange(port, "GET /v0/runs HTTP/1.1\r\n" + host + host + "\r\n");
    EXPECT_EQ(praenora::test::raw_status(two_hosts), 400) << two_hosts;
    EXPECT_EQ(json::parse(praenora::test::raw_body(two_hosts)).at("name"), "INVALID_ARGUMENT");

    const std::string not_found =
        praenora::test::raw_exchange(port, "GET /v0/nothing HTTP/1.1\r\n" + host + "\r\n");
    EXPECT_EQ(praenora::test::raw_status(not_found), 404) << not_found;
    EXPECT_TRUE(contains(praenora::test::raw_headers_lower(not_found), "connection: close"))
        << not_found;
    EXPECT_EQ(json::parse(praenora::test::raw_body(not_found)).at("name"), "NOT_FOUND");
}

TEST_F(ListenerTest, AHandlerThatThrowsAnswers500WithoutItsText) {
    Listener listener(parse_listen_address("127.0.0.1:0"));
    listener.start([](const Request&) -> Response { throw std::runtime_error("secret detail"); });
    const auto result = client_of(listener.address().to_string())->Get("/v0/runs");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 500);
    EXPECT_EQ(json::parse(result->body).at("name"), "INTERNAL");
    EXPECT_FALSE(contains(result->body, "secret detail"));
}

TEST_F(ListenerTest, TokensAndTheHostCheckOverARealConnection) {
    praenora::test::FakeSource source(praenora::server::ServerMode::serve);
    source.runs.push_back(praenora::test::fake_run(1, praenora::server::ServerMode::serve));
    Listener listener(parse_listen_address("127.0.0.1:0"));
    ControlApiSettings settings;
    settings.listener = listener.address();
    settings.tokens.control = kControl;
    settings.tokens.read = kRead;
    settings.pid = praenora::server::current_process_id();
    const ControlApi api(fixture_runtime(), source, settings);
    listener.start([&](const Request& request) { return api.handle(request); });
    const auto client = client_of(listener.address().to_string());

    const auto anonymous = client->Get("/v0/runs");
    ASSERT_TRUE(anonymous);
    EXPECT_EQ(anonymous->status, 401);
    EXPECT_EQ(anonymous->get_header_value("WWW-Authenticate"), "Bearer");
    EXPECT_EQ(body_json(anonymous).at("name"), "UNAUTHENTICATED");
    const auto health = client->Get("/v0/health");
    ASSERT_TRUE(health);
    EXPECT_EQ(health->status, 200);

    const auto read = client->Get("/v0/runs", bearer(kRead));
    ASSERT_TRUE(read);
    EXPECT_EQ(read->status, 200);
    const auto refused = client->Post("/v0/runs/1/stop", bearer(kRead), "", "application/json");
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->status, 403);
    EXPECT_EQ(body_json(refused).at("details").at("reason"), "scope");
    const auto stopped = client->Post("/v0/runs/1/stop", bearer(kControl), "", "application/json");
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped->status, 202) << stopped->body;
    EXPECT_EQ(source.runs.front()->stops.load(), 1);

    // Another Host, also with the control token: 403 before the credentials, on every route.
    for (const std::string path : {"/v0/runs", "/v0/health"}) {
        const auto rebound = client->Get(path, bearer(kControl, {{"Host", "evil.example"}}));
        ASSERT_TRUE(rebound);
        EXPECT_EQ(rebound->status, 403) << path;
        EXPECT_EQ(body_json(rebound).at("details").at("reason"), "host") << path;
    }
    const auto local = client->Get(
        "/v0/runs",
        bearer(kControl, {{"Host", "localhost:" + std::to_string(listener.address().port)}}));
    ASSERT_TRUE(local);
    EXPECT_EQ(local->status, 200);
}

TEST_F(ListenerTest, StopAnswersTheRequestsInFlightAndThenRefuses) {
    Listener listener(parse_listen_address("127.0.0.1:0"));
    Gate gate;
    listener.start([&](const Request& request) { return gate(request); });
    const std::string address = listener.address().to_string();

    std::vector<std::thread> clients;
    std::atomic<int> answered{0};
    for (int i = 0; i < 3; ++i) {
        clients.emplace_back([&] {
            const auto result = client_of(address)->Get("/v0/runs");
            if (result && result->status == 200) {
                ++answered;
            }
        });
    }
    ASSERT_TRUE(gate.wait_entered(3));
    std::thread stopping([&] { listener.stop(); });
    // stop() waits for the handlers: none has answered yet.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(gate.answered(), 0);
    gate.open();
    stopping.join();
    for (std::thread& client : clients) {
        client.join();
    }
    EXPECT_EQ(gate.answered(), 3);
    EXPECT_EQ(answered.load(), 3);
    // The port refuses connections now.
    const auto after = client_of(address)->Get("/v0/runs");
    EXPECT_FALSE(after);
    listener.stop(); // idempotent
    EXPECT_EQ(error_of([&] { listener.start([&](const Request& r) { return gate(r); }); }).code(),
              ErrorCode::INVALID_STATE);
}

// A query racing a close, over real connections: GET /v0/runs/{id}, its documents and /metrics in
// a loop from several clients while DELETE closes that run, and while the server shuts down in
// the order of `praenora serve` (the listener stops, then the registry). Run under
// ThreadSanitizer (dev-tsan) too.
TEST_F(ListenerTest, QueriesRaceADeleteAndAShutdown) {
    ScratchDir dir;
    write_fixture_config(dir.path() / "configs" / "single.yaml", "test.single",
                         "run: { ticks: 20 }\n", config_step("a", "fx-echo"));
    write_fixture_config(dir.path() / "configs" / "slow.yaml", "test.single",
                         "run: { ticks: 100 }\n",
                         config_step("a", "fx-slow", "{ sleep_ms: 50, cooperative: true }"));
    RegistrySettings settings;
    settings.configuration_root = utf8(dir.path() / "configs");
    settings.output_root = utf8(dir.path() / "outputs");
    RunRegistry registry(fixture_runtime(), settings);
    Listener listener(parse_listen_address("127.0.0.1:0"));
    ControlApiSettings api_settings;
    api_settings.listener = listener.address();
    const ControlApi api(fixture_runtime(), registry, api_settings);
    listener.start([&](const Request& request) { return api.handle(request); });
    const std::string address = listener.address().to_string();

    // Opening runs needs the control scope, which a TCP listener without tokens does not give:
    // they are opened through the API in this process.
    const auto open_run = [&](const std::string& name) {
        Request request =
            praenora::test::post_json("/v0/runs", "{\"configuration\":\"" + name + "\"}");
        return api.handle(request);
    };
    ASSERT_EQ(open_run("single.yaml").status, 201);

    std::atomic<bool> done{false};
    std::atomic<int> answers{0}, unexpected{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            const char* paths[] = {"/v0/runs",          "/v0/runs/1",         "/v0/runs/1/metrics",
                                   "/v0/runs/1/record", "/v0/runs/1/outputs", "/metrics",
                                   "/v0/runs/2",        "/v0/runs/2/record"};
            const auto client = client_of(address);
            while (!done.load()) {
                for (const char* path : paths) {
                    const auto result = client->Get(path);
                    if (!result) {
                        continue; // the listener stopped
                    }
                    if (result->status != 200 && result->status != 404 && result->status != 409) {
                        ++unexpected;
                    }
                    ++answers;
                }
            }
        });
    }
    ASSERT_TRUE(eventually([&] {
        const auto run = registry.find("1");
        return run != nullptr && praenora::test::is_final(run->status().state);
    }));
    EXPECT_EQ(api.handle(praenora::test::request("DELETE", "/v0/runs/1")).status, 204);
    ASSERT_EQ(open_run("slow.yaml").status, 201);
    ASSERT_TRUE(eventually([&] {
        const auto run = registry.find("2");
        return run != nullptr && run->status().state == WorkflowState::running;
    }));
    const int before = answers.load();
    EXPECT_TRUE(eventually([&] { return answers.load() > before + 50; }));
    listener.stop();
    registry.shutdown();
    EXPECT_EQ(registry.find("2")->status().state, WorkflowState::stopped);
    done = true;
    for (std::thread& reader : readers) {
        reader.join();
    }
    EXPECT_EQ(unexpected.load(), 0);
}

TEST_F(ListenerTest, ClientGetsAnswersAndTheServersErrors) {
    praenora::test::FakeSource source(praenora::server::ServerMode::run);
    source.runs.push_back(praenora::test::fake_run(1, praenora::server::ServerMode::run));
    Listener listener(parse_listen_address("127.0.0.1:0"));
    ControlApiSettings settings;
    settings.listener = listener.address();
    settings.tokens.control = kControl;
    const ControlApi api(fixture_runtime(), source, settings);
    listener.start([&](const Request& request) { return api.handle(request); });

    const ClientAnswer list = praenora::server::get(listener.address(), "/v0/runs", kControl);
    EXPECT_EQ(list.status, 200);
    EXPECT_EQ(list.content_type, "application/json; charset=utf-8");
    EXPECT_EQ(json::parse(list.body).at("runs").at(0).at("id"), "1");

    const ClientAnswer missing = praenora::server::get(listener.address(), "/v0/runs/7", kControl);
    EXPECT_EQ(missing.status, 404);
    const Error not_found = praenora::server::answer_error(listener.address(), missing);
    EXPECT_EQ(not_found.code(), ErrorCode::NOT_FOUND);
    EXPECT_TRUE(contains(not_found.what(), "'7'")) << not_found.what();

    const ClientAnswer anonymous =
        praenora::server::get(listener.address(), "/v0/runs", std::nullopt);
    EXPECT_EQ(anonymous.status, 401);
    EXPECT_EQ(praenora::server::answer_error(listener.address(), anonymous).code(),
              ErrorCode::UNAUTHENTICATED);
}

TEST_F(ListenerTest, ClientOfAPortThatRefusesIsAnIoError) {
    praenora::test::ClosedPort closed;
    ASSERT_FALSE(closed.start());
    const auto address = parse_listen_address("127.0.0.1:" + std::to_string(closed.port()));
    const Error error = error_of([&] { praenora::server::get(address, "/v0/runs", std::nullopt); });
    EXPECT_EQ(error.code(), ErrorCode::IO_ERROR);
    EXPECT_TRUE(contains(error.what(), address.to_string())) << error.what();
}

#ifndef _WIN32
// A server that resets the connection while the request is written fails the request with an
// IO_ERROR, and never ends the process with SIGPIPE, whatever its disposition (`praenora status
// --server` keeps the default, which ends a process). In the child process of a death test, with
// SIGPIPE at its default disposition: a request with a token of 16 MiB, whose headers are still
// being written when the listener resets the connection. A request of ordinary size goes in one
// write as soon as the connection is made, before a reset can come; the long token keeps the
// write going.
TEST_F(ListenerTest, ClientOfAConnectionResetWhileWritingFailsWithoutSigpipe) {
    praenora::test::expect_failure_without_sigpipe([]() -> std::optional<std::string> {
        praenora::test::ResettingListener server;
        if (const std::optional<std::string> refusal = server.start()) {
            std::fprintf(stderr, "the child's listener was refused: %s\n", refusal->c_str());
            return std::nullopt;
        }
        const auto address = parse_listen_address("127.0.0.1:" + std::to_string(server.port()));
        const std::string token(std::size_t{16} << 20, 'a');
        try {
            const ClientAnswer answer = praenora::server::get(address, "/v0/server", token);
            std::fprintf(stderr, "answered with status %d\n", answer.status);
        } catch (const Error& error) {
            if (error.code() == ErrorCode::IO_ERROR) {
                return std::string(error.what());
            }
            std::fprintf(stderr, "failed with %s\n", error.to_json().c_str());
        }
        return std::nullopt;
    });
}
#endif

TEST(ControlClient, AnAnswerWithoutAnErrorDocumentIsAnIoError) {
    const auto address = parse_listen_address("127.0.0.1:8080");
    ClientAnswer answer;
    answer.status = 502;
    answer.body = "<html>bad gateway</html>";
    const Error error = praenora::server::answer_error(address, answer);
    EXPECT_EQ(error.code(), ErrorCode::IO_ERROR);
    EXPECT_TRUE(contains(error.what(), "502")) << error.what();
    EXPECT_TRUE(contains(error.what(), "127.0.0.1:8080")) << error.what();
    // An error document keeps its code, location and details.
    answer.status = 422;
    answer.body = Error(ErrorCode::CONFIG_INVALID, "bad", {"w.yaml", 3, 4, "/steps"},
                        R"({"ok":false,"diagnostics":[]})")
                      .to_json();
    const Error kept = praenora::server::answer_error(address, answer);
    EXPECT_EQ(kept.code(), ErrorCode::CONFIG_INVALID);
    EXPECT_STREQ(kept.what(), "bad");
    EXPECT_EQ(kept.location().file, "w.yaml");
    EXPECT_EQ(kept.location().line, 3);
    EXPECT_EQ(kept.location().column, 4);
    EXPECT_EQ(kept.location().pointer, "/steps");
    EXPECT_EQ(json::parse(kept.details_json()), json::parse(R"({"ok":false,"diagnostics":[]})"));
}

#ifndef _WIN32
TEST_F(ListenerTest, UnixSocketHasMode0600AndIsRemovedAtTheEnd) {
    const SocketDir dir;
    const std::string path = dir.socket("s");
    {
        Listener listener(parse_listen_address("unix:" + path));
        EXPECT_EQ(listener.address().to_string(), "unix:" + path);
        struct stat status{};
        ASSERT_EQ(::lstat(path.c_str(), &status), 0);
        EXPECT_TRUE(S_ISSOCK(status.st_mode));
        EXPECT_EQ(status.st_mode & 07777, 0600u);
        Recorder recorder;
        listener.start([&](const Request& request) { return recorder(request); });
        const auto result = client_of("unix:" + path)->Get("/v0/runs");
        ASSERT_TRUE(result) << httplib::to_string(result.error());
        EXPECT_EQ(body_json(result).at("unix"), true);
        listener.stop();
        EXPECT_FALSE(fs::exists(path)); // gone once it stops listening
    }
    EXPECT_FALSE(fs::exists(path));
}

TEST_F(ListenerTest, ReplacesAStaleSocketAndRefusesALiveOne) {
    const SocketDir dir;
    const std::string path = dir.socket("s");
    ASSERT_FALSE(praenora::test::leave_stale_socket(path)); // as a crashed server leaves it
    ASSERT_TRUE(fs::exists(path));
    {
        Listener listener(parse_listen_address("unix:" + path));
        Recorder recorder;
        listener.start([&](const Request& request) { return recorder(request); });
        const auto result = client_of("unix:" + path)->Get("/v0/health");
        ASSERT_TRUE(result) << httplib::to_string(result.error());
        EXPECT_EQ(result->status, 200);

        // Somebody listens on it now: refused, and the socket stays.
        const Error live = error_of([&] { Listener second(parse_listen_address("unix:" + path)); });
        EXPECT_EQ(live.code(), ErrorCode::IO_ERROR);
        EXPECT_TRUE(contains(live.what(), "listens on it")) << live.what();
        const auto again = client_of("unix:" + path)->Get("/v0/health");
        ASSERT_TRUE(again);
        EXPECT_EQ(again->status, 200);
    }
    EXPECT_FALSE(fs::exists(path));
}

TEST_F(ListenerTest, LeavesAPathThatNamesAnotherFileAtTheEnd) {
    const SocketDir dir;
    const std::string path = dir.socket("s");
    Listener listener(parse_listen_address("unix:" + path));
    // Somebody replaced the socket's file: it is not removed.
    fs::remove(path);
    praenora::test::write_text_file(path, "theirs");
    listener.stop();
    EXPECT_EQ(praenora::test::read_file_text(path), "theirs");
}
#endif

#ifndef _WIN32
// A ListenAddress made without the parser: a path that sun_path cannot hold, or one with a NUL, is
// refused by the listener itself, naming the limit, and no socket is made anywhere (not one with
// the path cut short).
TEST(UnixSocketPath, APathTheSocketAddressCannotHoldIsRefused) {
    const SocketDir dir;
    praenora::server::ListenAddress address;
    address.family = praenora::server::ListenAddress::Family::unix_socket;
    address.path = dir.socket(std::string(300, 'x'));
    const Error long_path = error_of([&] { Listener listener(address); });
    EXPECT_EQ(long_path.code(), ErrorCode::IO_ERROR);
    EXPECT_TRUE(contains(long_path.what(),
                         "holds at most " +
                             std::to_string(praenora::server::unix_socket_path_limit()) + " bytes"))
        << long_path.what();
    address.path = dir.socket("a") + std::string(1, '\0') + "b";
    const Error with_nul = error_of([&] { Listener listener(address); });
    EXPECT_EQ(with_nul.code(), ErrorCode::IO_ERROR);
    EXPECT_TRUE(contains(with_nul.what(), "NUL")) << with_nul.what();
    EXPECT_TRUE(fs::is_empty(dir.path()));
}

// No listener is opened: the path is refused before anything binds.
TEST(UnixSocketPath, AFileOfAnotherKindIsRefusedAndKept) {
    const SocketDir dir;
    const std::string file = dir.socket("file");
    praenora::test::write_text_file(file, "keep me");
    const Error regular =
        error_of([&] { Listener listener(parse_listen_address("unix:" + file)); });
    EXPECT_EQ(regular.code(), ErrorCode::IO_ERROR);
    EXPECT_TRUE(contains(regular.what(), "not a socket")) << regular.what();
    EXPECT_TRUE(contains(regular.what(), file)) << regular.what();
    EXPECT_EQ(praenora::test::read_file_text(file), "keep me");

    const std::string directory = dir.socket("dir");
    fs::create_directory(directory);
    const Error is_directory =
        error_of([&] { Listener listener(parse_listen_address("unix:" + directory)); });
    EXPECT_TRUE(contains(is_directory.what(), "not a socket")) << is_directory.what();
    EXPECT_TRUE(fs::is_directory(directory));

    const std::string link = dir.socket("link");
    fs::create_symlink(file, link);
    const Error symbolic =
        error_of([&] { Listener listener(parse_listen_address("unix:" + link)); });
    EXPECT_TRUE(contains(symbolic.what(), "not a socket")) << symbolic.what();
    EXPECT_TRUE(fs::is_symlink(link));
    EXPECT_EQ(praenora::test::read_file_text(file), "keep me");
}
#endif
