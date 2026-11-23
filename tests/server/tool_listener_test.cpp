// The praenora tool as a server, end to end (label server, POSIX): the test starts the tool, reads
// the address it chose from --address-file and talks to it over real connections. `run --listen`
// on a Unix socket (mode 0600, the run's status while it runs, stop ends it with exit 130, the
// socket gone afterwards) and on 127.0.0.1 at port 0 (the address file and the note on standard
// error, tokens); a stale socket replaced, a live one and a file refused; a refused listener and an
// address file that cannot be written opening nothing; `serve`: runs opened under the
// configuration root, polled to their end, their records, DELETE, a second run of the same
// configuration and seed (another id, directory and run number, the same fingerprint), /metrics,
// tokens and the Host check, a body over 64 KiB, SIGTERM with a run inside a long call (the server
// waits for the call's own deadline, refusing connections meanwhile, then exits 0) and a second
// signal (exit 130 at once); `praenora status --server` (the table, --run, --json, the server's
// errors); and the outputs, the run record's reproducibility section and the fingerprint of the
// example workflow, the same with `praenora run`, `praenora run --listen` and a run served by
// `praenora serve`. Where local listeners are refused (a sandbox's EPERM), each test skips, saying
// "local listener refused:", or fails with PRAENORA_TESTS_REQUIRE_LISTENERS=1.
#include "listener_fixtures.hpp"

#include "server_fixtures.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::test::bearer;
using praenora::test::client_of;
using praenora::test::config_step;
using praenora::test::eventually;
using praenora::test::ListenerTest;
using praenora::test::read_file_text;
using praenora::test::ScratchDir;
using praenora::test::SocketDir;
using praenora::test::ToolProcess;
using praenora::test::utf8;
using praenora::test::write_fixture_config;
using praenora::test::write_text_file;
using praenora::test::write_token_file;
using Clock = std::chrono::steady_clock;

namespace {

const std::string kControl = "control-token-0123456789abcdefghijklmnop";
const std::string kRead = "read-token-ABCDEFGHIJKLMNOPQRSTUVWXYZ+/=~._";

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// The fixture modules and the test catalog.
std::vector<std::string> fixture_paths() {
    return {"--module-path", PRAENORA_TEST_MODULES_DIR, "--catalog-path",
            PRAENORA_TEST_CATALOG_DIR};
}

std::vector<std::string> joined(std::vector<std::string> first,
                                const std::vector<std::string>& second) {
    first.insert(first.end(), second.begin(), second.end());
    return first;
}

// A run of 600 ticks of fx-slow (50 ms each, stopping when cancelled): one that runs, for 30 s,
// until the test stops it.
void write_slow(const fs::path& file) {
    write_fixture_config(file, "test.single", "run: { ticks: 600 }\n",
                         config_step("a", "fx-slow", "{ sleep_ms: 50, cooperative: true }"));
}

// The parameters of fx-slow for a call of `sleep_ms` that ignores cancellation and writes the
// file `entered` as it starts.
std::string long_call(int sleep_ms, const fs::path& entered) {
    return "{ sleep_ms: " + std::to_string(sleep_ms) + ", cooperative: false, entered_file: \"" +
           utf8(entered) + "\" }";
}

// Whether `file` appears while `tool` runs, within 60 s.
bool wait_for_file(const fs::path& file, ToolProcess& tool) {
    return eventually([&] { return fs::exists(file) || !tool.running(); }) && fs::exists(file);
}

// A POST of JSON (an empty body for stop).
httplib::Result post(httplib::Client& client, const std::string& path, const std::string& body,
                     const httplib::Headers& headers = {}) {
    return client.Post(path, headers, body, "application/json");
}

// The status snapshot of run `id`, once its state is `state`; null when it never is.
json wait_for_state(httplib::Client& client, const std::string& id, const std::string& state,
                    const httplib::Headers& headers = {}) {
    json snapshot;
    const bool reached = eventually([&] {
        const auto result = client.Get("/v0/runs/" + id, headers);
        if (!result || result->status != 200) {
            return false;
        }
        snapshot = json::parse(result->body);
        return snapshot.at("workflow").at("state") == state;
    });
    return reached ? snapshot : json();
}

// The files below `dir`, by their relative paths (with '/'), with their bytes.
std::map<std::string, std::string> files_below(const fs::path& dir) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            files[entry.path().lexically_relative(dir).generic_string()] =
                read_file_text(entry.path());
        }
    }
    return files;
}

} // namespace

TEST_F(ListenerTest, RunListensOnAUnixSocketUntilItsRunIsStopped) {
    ScratchDir dir;
    const SocketDir sockets;
    write_slow(dir.path() / "workflow.yaml");
    const std::string socket = sockets.socket("s");
    const fs::path address_file = dir.path() / "address";
    ToolProcess tool(
        joined({"run", utf8(dir.path() / "workflow.yaml"), "--listen", "unix:" + socket,
                "--address-file", utf8(address_file), "--output-dir", utf8(dir.path() / "out")},
               fixture_paths()),
        dir.path());
    const std::optional<std::string> address = tool.address_in(address_file);
    ASSERT_TRUE(address) << tool.outputs();
    EXPECT_EQ(*address, "unix:" + socket);
    EXPECT_TRUE(contains(tool.err(), "praenora: listening on unix:" + socket + "\n"))
        << tool.outputs();
    struct stat status{};
    ASSERT_EQ(::lstat(socket.c_str(), &status), 0);
    EXPECT_TRUE(S_ISSOCK(status.st_mode));
    EXPECT_EQ(status.st_mode & 07777, 0600u);

    const auto client = client_of(*address);
    const json running = wait_for_state(*client, "1", "running");
    ASSERT_FALSE(running.is_null()) << tool.outputs();
    EXPECT_EQ(running.at("kind"), "status-snapshot");
    const auto list = client->Get("/v0/runs");
    ASSERT_TRUE(list);
    const json entry = json::parse(list->body).at("runs").at(0);
    EXPECT_EQ(entry.at("id"), "1");
    EXPECT_EQ(entry.at("configuration"), "workflow.yaml");
    EXPECT_TRUE(entry.at("output").is_null());
    EXPECT_EQ(entry.at("run_number"), 1);
    // Run mode opens and deletes no run.
    const auto opened = post(*client, "/v0/runs", R"({"configuration":"workflow.yaml"})");
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->status, 404);

    const auto stopped = post(*client, "/v0/runs/1/stop", "");
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped->status, 202) << stopped->body;
    EXPECT_EQ(tool.wait(), 130) << tool.outputs();
    EXPECT_TRUE(contains(tool.out(), "state             stopped")) << tool.outputs();
    EXPECT_FALSE(fs::exists(socket));
    EXPECT_FALSE(client_of(*address)->Get("/v0/health"));
}

TEST_F(ListenerTest, RunListensOnTcpAtThePortTheSystemChose) {
    ScratchDir dir;
    write_slow(dir.path() / "workflow.yaml");
    write_token_file(dir.path() / "control.token", kControl);
    const fs::path address_file = dir.path() / "address";
    write_text_file(address_file, "stale\n"); // replaced
    ToolProcess tool(joined({"run", utf8(dir.path() / "workflow.yaml"), "--listen", "127.0.0.1:0",
                             "--address-file", utf8(address_file), "--token-file",
                             utf8(dir.path() / "control.token"), "--json"},
                            fixture_paths()),
                     dir.path());
    std::optional<std::string> address;
    EXPECT_TRUE(eventually([&] {
        address = tool.address_in(address_file);
        return !tool.running() || (address && *address != "stale");
    }));
    ASSERT_TRUE(address) << tool.outputs();
    ASSERT_EQ(address->rfind("127.0.0.1:", 0), 0u) << *address;
    EXPECT_NE(*address, "127.0.0.1:0");
    EXPECT_TRUE(contains(tool.err(), "praenora: listening on " + *address + "\n"))
        << tool.outputs();
    for (const auto& entry : fs::directory_iterator(dir.path())) {
        EXPECT_FALSE(contains(utf8(entry.path().filename()), ".tmp"))
            << "left behind: " << utf8(entry.path());
    }

    const auto client = client_of(*address);
    const auto health = client->Get("/v0/health");
    ASSERT_TRUE(health);
    EXPECT_EQ(health->status, 200);
    const auto anonymous = client->Get("/v0/runs/1");
    ASSERT_TRUE(anonymous);
    EXPECT_EQ(anonymous->status, 401);
    ASSERT_FALSE(wait_for_state(*client, "1", "running", bearer(kControl)).is_null())
        << tool.outputs();
    const auto stopped = post(*client, "/v0/runs/1/stop", "", bearer(kControl));
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped->status, 202) << stopped->body;
    EXPECT_EQ(tool.wait(), 130) << tool.outputs();
    // With --json, standard output holds the one document of the run, as without a listener.
    const json document = json::parse(tool.out());
    EXPECT_EQ(document.at("ok"), false);
    EXPECT_EQ(document.at("error").at("name"), "RUN_STOPPED");
    EXPECT_FALSE(client_of(*address)->Get("/v0/health"));
}

TEST_F(ListenerTest, RunReplacesAStaleSocketAndRefusesALiveOneAndAFile) {
    ScratchDir dir;
    const SocketDir sockets;
    write_slow(dir.path() / "workflow.yaml");
    const std::string socket = sockets.socket("s");
    ASSERT_FALSE(praenora::test::leave_stale_socket(socket)); // as a crashed server leaves it
    const fs::path first_dir = dir.path() / "first";
    fs::create_directories(first_dir);
    ToolProcess first(joined({"run", utf8(dir.path() / "workflow.yaml"), "--listen",
                              "unix:" + socket, "--address-file", utf8(first_dir / "address")},
                             fixture_paths()),
                      first_dir);
    ASSERT_TRUE(first.address_in(first_dir / "address")) << first.outputs();
    const auto client = client_of("unix:" + socket);
    ASSERT_FALSE(wait_for_state(*client, "1", "running").is_null()) << first.outputs();

    // Somebody listens on it: refused, exit 74, and the first run's socket stays.
    const fs::path second_dir = dir.path() / "second";
    fs::create_directories(second_dir);
    ToolProcess second(
        joined({"run", utf8(dir.path() / "workflow.yaml"), "--listen", "unix:" + socket},
               fixture_paths()),
        second_dir);
    EXPECT_EQ(second.wait(), 74) << second.outputs();
    EXPECT_TRUE(contains(second.err(), "listens on it")) << second.outputs();
    const auto still = client->Get("/v0/health");
    ASSERT_TRUE(still);
    EXPECT_EQ(still->status, 200);
    ASSERT_TRUE(post(*client, "/v0/runs/1/stop", ""));
    EXPECT_EQ(first.wait(), 130) << first.outputs();

    // A regular file at the path: refused, and kept.
    const std::string file = sockets.socket("f");
    write_text_file(file, "keep me");
    const fs::path third_dir = dir.path() / "third";
    fs::create_directories(third_dir);
    ToolProcess third(
        joined({"run", utf8(dir.path() / "workflow.yaml"), "--listen", "unix:" + file},
               fixture_paths()),
        third_dir);
    EXPECT_EQ(third.wait(), 74) << third.outputs();
    EXPECT_TRUE(contains(third.err(), "not a socket")) << third.outputs();
    EXPECT_EQ(read_file_text(file), "keep me");
}

// A listener that is refused, or an address file that cannot be written, opens nothing: no output
// directory is made (the listener is opened, and the address file written, before the workflow).
// The first needs no listener: the path is refused before anything binds.
TEST(ToolListen, ARefusedListenerOpensNothing) {
    ScratchDir dir;
    const SocketDir sockets;
    write_slow(dir.path() / "workflow.yaml");
    const std::string file = sockets.socket("f");
    write_text_file(file, "not a socket");
    ToolProcess tool(joined({"run", utf8(dir.path() / "workflow.yaml"), "--listen", "unix:" + file,
                             "--output-dir", utf8(dir.path() / "out")},
                            fixture_paths()),
                     dir.path());
    EXPECT_EQ(tool.wait(), 74) << tool.outputs();
    EXPECT_TRUE(contains(tool.err(), "not a socket")) << tool.outputs();
    EXPECT_FALSE(fs::exists(dir.path() / "out"));
}

TEST_F(ListenerTest, RunWithAnAddressFileItCannotWriteOpensNothing) {
    ScratchDir dir;
    write_slow(dir.path() / "workflow.yaml");
    ToolProcess tool(joined({"run", utf8(dir.path() / "workflow.yaml"), "--listen", "127.0.0.1:0",
                             "--address-file", utf8(dir.path() / "missing" / "address"),
                             "--output-dir", utf8(dir.path() / "out")},
                            fixture_paths()),
                     dir.path());
    EXPECT_EQ(tool.wait(), 74) << tool.outputs();
    EXPECT_TRUE(contains(tool.err(), "the address file")) << tool.outputs();
    EXPECT_FALSE(fs::exists(dir.path() / "out"));
}

TEST_F(ListenerTest, ServeOpensRunsOnRequestAndDeletesThem) {
    ScratchDir dir;
    const SocketDir sockets;
    write_fixture_config(dir.path() / "configs" / "sub" / "single.yaml", "test.single",
                         "run: { ticks: 3 }\n", config_step("a", "fx-echo"));
    const fs::path outputs = dir.path() / "outputs";
    ToolProcess tool(joined({"serve", "--listen", sockets.address("s"), "--config-root",
                             utf8(dir.path() / "configs"), "--output-root", utf8(outputs),
                             "--address-file", utf8(dir.path() / "address")},
                            fixture_paths()),
                     dir.path());
    const std::optional<std::string> address = tool.address_in(dir.path() / "address");
    ASSERT_TRUE(address) << tool.outputs();
    const auto client = client_of(*address);

    const auto server = client->Get("/v0/server");
    ASSERT_TRUE(server);
    EXPECT_EQ(json::parse(server->body).at("mode"), "serve");
    EXPECT_EQ(json::parse(server->body).at("scopes"), json::array({"control", "read"}));

    const auto first = post(*client, "/v0/runs", R"({"configuration":"sub/single.yaml","seed":7})");
    ASSERT_TRUE(first);
    ASSERT_EQ(first->status, 201) << first->body;
    EXPECT_EQ(first->get_header_value("Location"), "/v0/runs/1");
    const json first_entry = json::parse(first->body);
    EXPECT_EQ(first_entry.at("id"), "1");
    EXPECT_EQ(first_entry.at("output"), "run-1");
    EXPECT_EQ(first_entry.at("configuration"), "sub/single.yaml");
    ASSERT_FALSE(wait_for_state(*client, "1", "completed").is_null()) << tool.outputs();
    const auto record = client->Get("/v0/runs/1/record");
    ASSERT_TRUE(record);
    EXPECT_EQ(record->status, 200);
    EXPECT_EQ(json::parse(record->body).at("kind"), "run-record");
    EXPECT_TRUE(fs::exists(outputs / "run-1" / "run-record.json"));
    EXPECT_TRUE(fs::exists(outputs / "run-1.log"));

    const auto metrics = client->Get("/metrics");
    ASSERT_TRUE(metrics);
    EXPECT_EQ(metrics->status, 200);
    EXPECT_EQ(metrics->get_header_value("Content-Type"),
              "text/plain; version=0.0.4; charset=utf-8");
    EXPECT_TRUE(contains(metrics->body, "praenora_workflow_ticks_total{")) << metrics->body;

    httplib::Request remove;
    remove.method = "DELETE";
    remove.path = "/v0/runs/1";
    const auto deleted = client->send(remove);
    ASSERT_TRUE(deleted);
    EXPECT_EQ(deleted->status, 204) << deleted->body;
    const auto gone = client->Get("/v0/runs/1");
    ASSERT_TRUE(gone);
    EXPECT_EQ(gone->status, 404);
    EXPECT_TRUE(fs::exists(outputs / "run-1" / "run-record.json")); // its files stay

    const auto again = post(*client, "/v0/runs", R"({"configuration":"sub/single.yaml","seed":7})");
    ASSERT_TRUE(again);
    ASSERT_EQ(again->status, 201) << again->body;
    const json second_entry = json::parse(again->body);
    EXPECT_EQ(second_entry.at("id"), "2");
    EXPECT_EQ(second_entry.at("output"), "run-2");
    EXPECT_EQ(second_entry.at("run_number"), first_entry.at("run_number").get<int>() + 1);
    EXPECT_EQ(second_entry.at("fingerprint"), first_entry.at("fingerprint"));
    ASSERT_FALSE(wait_for_state(*client, "2", "completed").is_null()) << tool.outputs();

    tool.signal(SIGTERM);
    EXPECT_EQ(tool.wait(), 0) << tool.outputs();
    EXPECT_FALSE(fs::exists(sockets.socket("s")));
}

TEST_F(ListenerTest, ServeChecksTokensTheHostAndTheBodySizeOverARealConnection) {
    ScratchDir dir;
    write_fixture_config(dir.path() / "configs" / "single.yaml", "test.single",
                         "run: { ticks: 1 }\n", config_step("a", "fx-echo"));
    write_token_file(dir.path() / "control.token", kControl);
    write_token_file(dir.path() / "read.token", kRead);
    ToolProcess tool(
        joined({"serve", "--listen", "127.0.0.1:0", "--config-root", utf8(dir.path() / "configs"),
                "--output-root", utf8(dir.path() / "outputs"), "--token-file",
                utf8(dir.path() / "control.token"), "--read-token-file",
                utf8(dir.path() / "read.token"), "--allow-host", "praenora.test", "--address-file",
                utf8(dir.path() / "address")},
               fixture_paths()),
        dir.path());
    const std::optional<std::string> address = tool.address_in(dir.path() / "address");
    ASSERT_TRUE(address) << tool.outputs();
    const auto client = client_of(*address);

    const auto anonymous = client->Get("/v0/runs");
    ASSERT_TRUE(anonymous);
    EXPECT_EQ(anonymous->status, 401);
    EXPECT_EQ(anonymous->get_header_value("WWW-Authenticate"), "Bearer");
    const auto wrong = client->Get("/v0/runs", bearer(std::string(40, 'x')));
    ASSERT_TRUE(wrong);
    EXPECT_EQ(wrong->status, 401);
    const auto read = client->Get("/v0/runs", bearer(kRead));
    ASSERT_TRUE(read);
    EXPECT_EQ(read->status, 200);
    const auto refused =
        post(*client, "/v0/runs", R"({"configuration":"single.yaml"})", bearer(kRead));
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->status, 403);
    EXPECT_EQ(json::parse(refused->body).at("details").at("reason"), "scope");
    const auto rebound = client->Get("/v0/runs", bearer(kControl, {{"Host", "evil.example"}}));
    ASSERT_TRUE(rebound);
    EXPECT_EQ(rebound->status, 403);
    EXPECT_EQ(json::parse(rebound->body).at("details").at("reason"), "host");
    const auto allowed = client->Get("/v0/runs", bearer(kControl, {{"Host", "praenora.test"}}));
    ASSERT_TRUE(allowed);
    EXPECT_EQ(allowed->status, 200);

    const std::string large = R"({"configuration":")" + std::string(70 * 1024, 'a') + "\"}";
    const auto too_large = post(*client, "/v0/runs", large, bearer(kControl));
    ASSERT_TRUE(too_large);
    EXPECT_EQ(too_large->status, 413);
    EXPECT_EQ(json::parse(too_large->body).at("name"), "INVALID_ARGUMENT");
    const auto opened =
        post(*client, "/v0/runs", R"({"configuration":"single.yaml"})", bearer(kControl));
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->status, 201) << opened->body;

    tool.signal(SIGINT);
    EXPECT_EQ(tool.wait(), 0) << tool.outputs();
}

// A run that does not stop: SIGTERM while a run sits in a call that ignores cancellation (sent once
// fx-slow has written its marker file, inside the call): the server stops listening at once,
// waits for the call's own deadline (3 s, then the cancellation grace of 1 s), and then exits 0,
// not after the call (ten minutes).
TEST_F(ListenerTest, ServeWaitsForARunInALongCallAndThenExits0) {
    ScratchDir dir;
    const SocketDir sockets;
    const fs::path entered = dir.path() / "entered";
    write_fixture_config(dir.path() / "configs" / "long.yaml", "test.single",
                         "run:\n  ticks: 1\n  timeouts: { invoke: 3s, cancel_grace: 1s }\n",
                         config_step("a", "fx-slow", long_call(600000, entered)));
    ToolProcess tool(
        joined({"serve", "--listen", sockets.address("s"), "--config-root",
                utf8(dir.path() / "configs"), "--output-root", utf8(dir.path() / "outputs"),
                "--address-file", utf8(dir.path() / "address")},
               fixture_paths()),
        dir.path());
    const std::optional<std::string> address = tool.address_in(dir.path() / "address");
    ASSERT_TRUE(address) << tool.outputs();
    const auto client = client_of(*address);
    const auto opened = post(*client, "/v0/runs", R"({"configuration":"long.yaml"})");
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened->status, 201) << opened->body;
    ASSERT_TRUE(wait_for_file(entered, tool)) << tool.outputs();

    const auto signalled = Clock::now();
    tool.signal(SIGTERM);
    // Not listening any more, while it waits for the run.
    EXPECT_TRUE(eventually([&] { return !client_of(*address)->Get("/v0/health"); },
                           std::chrono::seconds(2)));
    EXPECT_TRUE(tool.running()) << tool.outputs();
    const std::optional<int> exit_code = tool.wait(std::chrono::seconds(60));
    const auto waited = Clock::now() - signalled;
    EXPECT_EQ(exit_code, 0) << tool.outputs();
    EXPECT_LT(waited, std::chrono::seconds(15));
    const json record =
        json::parse(read_file_text(dir.path() / "outputs" / "run-1" / "run-record.json"));
    EXPECT_EQ(record.at("execution").at("status"), "failed") << record.dump();
    EXPECT_EQ(record.at("execution").at("error").at("code"), 703) << record.dump(); // unresponsive
    EXPECT_TRUE(
        contains(read_file_text(dir.path() / "outputs" / "run-1.log"), "MODULE_UNRESPONSIVE"));
    EXPECT_FALSE(fs::exists(sockets.socket("s")));
}

TEST_F(ListenerTest, ServeExitsAtOnceWith130OnASecondSignal) {
    ScratchDir dir;
    const SocketDir sockets;
    const fs::path entered = dir.path() / "entered";
    write_fixture_config(dir.path() / "configs" / "hung.yaml", "test.single",
                         "run:\n  ticks: 1\n  timeouts: { invoke: none }\n",
                         config_step("a", "fx-slow", long_call(600000, entered)));
    ToolProcess tool(
        joined({"serve", "--listen", sockets.address("s"), "--config-root",
                utf8(dir.path() / "configs"), "--output-root", utf8(dir.path() / "outputs"),
                "--address-file", utf8(dir.path() / "address")},
               fixture_paths()),
        dir.path());
    const std::optional<std::string> address = tool.address_in(dir.path() / "address");
    ASSERT_TRUE(address) << tool.outputs();
    const auto client = client_of(*address);
    const auto opened = post(*client, "/v0/runs", R"({"configuration":"hung.yaml"})");
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened->status, 201) << opened->body;
    // Inside the call, which never returns: only a second signal ends the server.
    ASSERT_TRUE(wait_for_file(entered, tool)) << tool.outputs();
    tool.signal(SIGTERM);
    EXPECT_FALSE(tool.wait(std::chrono::milliseconds(500))) << tool.outputs();
    const auto second = Clock::now();
    tool.signal(SIGTERM);
    EXPECT_EQ(tool.wait(std::chrono::seconds(10)), 130) << tool.outputs();
    EXPECT_LT(Clock::now() - second, std::chrono::seconds(5));
}

TEST_F(ListenerTest, StatusServerPrintsTheRunListTheRunAndJson) {
    ScratchDir dir;
    const SocketDir sockets;
    write_slow(dir.path() / "workflow.yaml");
    write_token_file(dir.path() / "control.token", kControl);
    const fs::path run_dir = dir.path() / "run";
    fs::create_directories(run_dir);
    ToolProcess tool(joined({"run", utf8(dir.path() / "workflow.yaml"), "--listen",
                             sockets.address("s"), "--address-file", utf8(run_dir / "address")},
                            fixture_paths()),
                     run_dir);
    const std::optional<std::string> address = tool.address_in(run_dir / "address");
    ASSERT_TRUE(address) << tool.outputs();
    const auto client = client_of(*address);
    ASSERT_FALSE(wait_for_state(*client, "1", "running").is_null()) << tool.outputs();

    const auto status = [&](const std::string& name, const std::vector<std::string>& args) {
        const fs::path out = dir.path() / name;
        fs::create_directories(out);
        auto process = std::make_unique<ToolProcess>(joined({"status", "--server"}, args), out);
        process->wait();
        return process;
    };
    const auto table = status("table", {*address});
    EXPECT_EQ(table->wait(), 0) << table->outputs();
    EXPECT_TRUE(contains(table->out(), "id  workflow")) << table->outputs();
    EXPECT_TRUE(contains(table->out(), "1   test.single@0.1.0  running  healthy"))
        << table->outputs();

    const auto one = status("one", {*address, "--run", "1"});
    EXPECT_EQ(one->wait(), 0) << one->outputs();
    EXPECT_TRUE(contains(one->out(), "test.single@0.1.0")) << one->outputs();
    EXPECT_TRUE(contains(one->out(), "running (health healthy)")) << one->outputs();
    EXPECT_TRUE(contains(one->out(), "steps:")) << one->outputs();

    const auto listed = status("json", {*address, "--json"});
    EXPECT_EQ(listed->wait(), 0) << listed->outputs();
    const json document = json::parse(listed->out());
    EXPECT_EQ(document.at("ok"), true);
    EXPECT_EQ(document.at("result").at("runs").at(0).at("id"), "1");

    const auto snapshot = status("snapshot", {*address, "--run", "1", "--json"});
    EXPECT_EQ(snapshot->wait(), 0) << snapshot->outputs();
    EXPECT_EQ(json::parse(snapshot->out()).at("result").at("kind"), "status-snapshot");

    // The server's errors keep their codes and exit codes: NOT_FOUND is 74.
    const auto missing = status("missing", {*address, "--run", "2", "--json"});
    EXPECT_EQ(missing->wait(), 74) << missing->outputs();
    EXPECT_EQ(json::parse(missing->out()).at("error").at("name"), "NOT_FOUND");

    ASSERT_TRUE(post(*client, "/v0/runs/1/stop", ""));
    EXPECT_EQ(tool.wait(), 130) << tool.outputs();
    // Nobody listens any more: IO_ERROR, exit 74.
    const auto unreachable = status("unreachable", {*address});
    EXPECT_EQ(unreachable->wait(), 74) << unreachable->outputs();
    EXPECT_TRUE(contains(unreachable->err(), "IO_ERROR")) << unreachable->outputs();
}

TEST_F(ListenerTest, StatusServerSendsTheTokenOverTcp) {
    ScratchDir dir;
    write_fixture_config(dir.path() / "configs" / "single.yaml", "test.single",
                         "run: { ticks: 1 }\n", config_step("a", "fx-echo"));
    write_token_file(dir.path() / "control.token", kControl);
    const fs::path serve_dir = dir.path() / "serve";
    fs::create_directories(serve_dir);
    ToolProcess tool(
        joined({"serve", "--listen", "127.0.0.1:0", "--config-root", utf8(dir.path() / "configs"),
                "--output-root", utf8(dir.path() / "outputs"), "--token-file",
                utf8(dir.path() / "control.token"), "--address-file", utf8(serve_dir / "address")},
               fixture_paths()),
        serve_dir);
    const std::optional<std::string> address = tool.address_in(serve_dir / "address");
    ASSERT_TRUE(address) << tool.outputs();

    const fs::path with_dir = dir.path() / "with";
    fs::create_directories(with_dir);
    ToolProcess with_token(
        {"status", "--server", *address, "--token-file", utf8(dir.path() / "control.token")},
        with_dir);
    EXPECT_EQ(with_token.wait(), 0) << with_token.outputs();
    EXPECT_TRUE(contains(with_token.out(), "id  workflow")) << with_token.outputs();

    const fs::path without_dir = dir.path() / "without";
    fs::create_directories(without_dir);
    ToolProcess without({"status", "--server", *address}, without_dir);
    EXPECT_EQ(without.wait(), 2) << without.outputs();
    EXPECT_TRUE(contains(without.err(), "UNAUTHENTICATED")) << without.outputs();

    tool.signal(SIGTERM);
    EXPECT_EQ(tool.wait(), 0) << tool.outputs();
}

#ifdef PRAENORA_TEST_EXAMPLE_WORKFLOW_DIR
// No listener and no request changes a run: the example workflow's outputs, message trace, run
// record's reproducibility section and fingerprint are the same from `praenora run`, from
// `praenora run --listen` and from a run that `praenora serve` opens, for one seed.
TEST_F(ListenerTest, RunsWithAListenerGiveTheResultsOfPraenoraRun) {
    ScratchDir dir;
    const SocketDir sockets;
    const fs::path example = praenora::test::utf8_path(PRAENORA_TEST_EXAMPLE_WORKFLOW_DIR);
    const std::vector<std::string> modules = {"--module-path", PRAENORA_EXAMPLE_MODULES_DIR};
    const std::string config = utf8(example / "workflow.yaml");

    const fs::path plain_dir = dir.path() / "plain";
    fs::create_directories(plain_dir);
    ToolProcess plain(
        joined({"run", config, "--seed", "11", "--trace", "--output-dir", utf8(plain_dir / "out")},
               modules),
        plain_dir);
    ASSERT_EQ(plain.wait(), 0) << plain.outputs();

    const fs::path listen_dir = dir.path() / "listen";
    fs::create_directories(listen_dir);
    ToolProcess listening(
        joined({"run", config, "--seed", "11", "--trace", "--output-dir", utf8(listen_dir / "out"),
                "--listen", sockets.address("r"), "--address-file", utf8(listen_dir / "address")},
               modules),
        listen_dir);
    ASSERT_EQ(listening.wait(), 0) << listening.outputs();
    EXPECT_TRUE(contains(listening.err(), "praenora: listening on unix:")) << listening.outputs();

    const fs::path serve_dir = dir.path() / "serve";
    fs::create_directories(serve_dir);
    ToolProcess serving(joined({"serve", "--listen", sockets.address("s"), "--config-root",
                                utf8(example), "--output-root", utf8(serve_dir / "outputs"),
                                "--address-file", utf8(serve_dir / "address")},
                               modules),
                        serve_dir);
    const std::optional<std::string> address = serving.address_in(serve_dir / "address");
    ASSERT_TRUE(address) << serving.outputs();
    const auto client = client_of(*address);
    const auto opened =
        post(*client, "/v0/runs",
             R"({"configuration":"workflow.yaml","seed":11,"trace_messages":true})");
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened->status, 201) << opened->body;
    ASSERT_FALSE(wait_for_state(*client, "1", "completed").is_null()) << serving.outputs();
    serving.signal(SIGTERM);
    ASSERT_EQ(serving.wait(), 0) << serving.outputs();

    const fs::path plain_out = plain_dir / "out";
    const json plain_record = json::parse(read_file_text(plain_out / "run-record.json"));
    const auto plain_outputs = files_below(plain_out / "outputs");
    EXPECT_FALSE(plain_outputs.empty());
    const std::string plain_trace = read_file_text(plain_out / "trace" / "messages.jsonl");
    EXPECT_FALSE(plain_trace.empty());
    for (const fs::path& out : {listen_dir / "out", serve_dir / "outputs" / "run-1"}) {
        const json record = json::parse(read_file_text(out / "run-record.json"));
        EXPECT_EQ(record.at("fingerprint"), plain_record.at("fingerprint")) << utf8(out);
        EXPECT_EQ(record.at("reproducibility").dump(), plain_record.at("reproducibility").dump())
            << utf8(out);
        EXPECT_TRUE(files_below(out / "outputs") == plain_outputs) << utf8(out);
        EXPECT_TRUE(read_file_text(out / "trace" / "messages.jsonl") == plain_trace) << utf8(out);
    }
    EXPECT_EQ(json::parse(opened->body).at("fingerprint"), plain_record.at("fingerprint"));
}
#endif
