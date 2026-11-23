// Helpers for the tests of the control API's server core (src/server): a fake RunSource with fake
// runs, requests as a listener builds them, the fixture runtime and configurations of the test
// catalog, and waiting for a run to end. Header-only.
#ifndef PRAENORA_TESTS_SERVER_SERVER_FIXTURES_HPP
#define PRAENORA_TESTS_SERVER_SERVER_FIXTURES_HPP

#include "control_api.hpp"
#include "http_types.hpp"
#include "run_source.hpp"

#include "module_fixtures.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/runtime.hpp>
#include <praenora/runtime/status.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace praenora::test {

inline std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

// A run whose documents are fixed texts.
class FakeRun final : public server::RunHandle {
public:
    FakeRun(server::RunFacts facts, StatusSnapshot snapshot)
        : facts_(std::move(facts)), snapshot_(std::move(snapshot)) {}

    const server::RunFacts& facts() const override { return facts_; }
    StatusSnapshot status() const override { return snapshot_; }
    std::string status_json() const override { return status_json_; }
    std::string metrics_json() const override { return metrics_json_; }
    std::string run_record_json() const override { return run_record_json_; }
    std::string latest_outputs_json() const override { return outputs_json_; }
    void request_stop() noexcept override { ++stops; }

    server::RunFacts facts_;
    StatusSnapshot snapshot_;
    std::string status_json_ = R"({"kind":"status-snapshot"})";
    std::string metrics_json_ = R"({"metrics":[]})";
    std::string run_record_json_ = "null";
    std::string outputs_json_ = "{}";
    std::atomic<int> stops{0};
};

// A fake run with id `id`: run number `id`, workflow test.single@0.1.0 and its fingerprint.
inline std::shared_ptr<FakeRun> fake_run(std::uint64_t id, server::ServerMode mode) {
    server::RunFacts facts;
    facts.id = std::to_string(id);
    facts.run_number = id;
    facts.configuration = mode == server::ServerMode::serve ? "sub/single.yaml" : "single.yaml";
    facts.created_at = "2026-11-22T10:00:00.000000000Z";
    if (mode == server::ServerMode::serve) {
        facts.output = "run-" + std::to_string(id);
    }
    StatusSnapshot snapshot;
    snapshot.workflow_name = "test.single";
    snapshot.workflow_version = "0.1.0";
    snapshot.fingerprint = std::string(64, 'a');
    snapshot.state = WorkflowState::running;
    snapshot.health = Health::healthy;
    snapshot.ticks_completed = 3;
    return std::make_shared<FakeRun>(std::move(facts), std::move(snapshot));
}

// A source that holds fake runs and records what it was asked. Single-threaded.
class FakeSource final : public server::RunSource {
public:
    explicit FakeSource(server::ServerMode mode) : mode_(mode) {}

    server::ServerMode mode() const override { return mode_; }
    std::vector<std::shared_ptr<server::RunHandle>> list() const override {
        if (on_list) {
            on_list();
        }
        return {runs.begin(), runs.end()};
    }
    std::shared_ptr<server::RunHandle> find(const std::string& id) const override {
        for (const auto& run : runs) {
            if (run->facts().id == id) {
                return run;
            }
        }
        return nullptr;
    }
    std::shared_ptr<server::RunHandle> create(const server::RunRequest& request) override {
        created.push_back(request);
        if (create_error) {
            throw *create_error;
        }
        auto run = fake_run(runs.size() + 1, mode_);
        runs.push_back(run);
        return run;
    }
    std::shared_ptr<server::RunHandle> stop(const std::string& id) override {
        const auto run = find(id);
        if (run) {
            run->request_stop();
        }
        return run;
    }
    void remove(const std::string& id) override {
        removed.push_back(id);
        if (remove_error) {
            throw *remove_error;
        }
        for (auto it = runs.begin(); it != runs.end(); ++it) {
            if ((*it)->facts().id == id) {
                runs.erase(it);
                return;
            }
        }
        throw Error(ErrorCode::NOT_FOUND, "no run " + id);
    }

    std::vector<std::shared_ptr<FakeRun>> runs;
    std::vector<server::RunRequest> created;
    std::vector<std::string> removed;
    std::optional<Error> create_error, remove_error;
    std::function<void()> on_list;

private:
    server::ServerMode mode_;
};

// A request as a listener hands it over: header names in lower case.
inline server::Request request(std::string method, std::string path,
                               server::ListenerKind listener = server::ListenerKind::unix_socket,
                               std::vector<std::pair<std::string, std::string>> headers = {},
                               std::string body = "") {
    server::Request result;
    result.method = std::move(method);
    result.path = std::move(path);
    result.listener = listener;
    for (auto& [name, value] : headers) {
        result.headers.emplace(std::move(name), std::move(value));
    }
    result.body = std::move(body);
    return result;
}

// A POST with a JSON body through a Unix socket.
inline server::Request post_json(std::string path, std::string body) {
    return request("POST", std::move(path), server::ListenerKind::unix_socket,
                   {{"content-type", "application/json"}}, std::move(body));
}

// The body of a JSON response, parsed.
inline nlohmann::json body_of(const server::Response& response) {
    return nlohmann::json::parse(response.body);
}

// The fixture modules and the test catalog, without the environment and without logging to
// standard error.
inline RuntimeOptions fixture_runtime_options() {
    RuntimeOptions options;
    options.module_paths = {PRAENORA_TEST_MODULES_DIR};
    options.catalog_paths = {PRAENORA_TEST_CATALOG_DIR};
    options.use_environment = false;
    options.logging.to_stderr = false;
    return options;
}

// A step block binding `id` to fixture module `module` with `parameters`.
inline std::string config_step(const std::string& id, const std::string& module,
                               const std::string& parameters = "{}") {
    return "  " + id + ":\n    module: { name: " + module +
           ", version: \"0.1.0\" }\n    parameters: " + parameters + "\n";
}

// Writes `file`, a configuration of the test workflow `workflow` with `run` and `steps` (YAML
// blocks), and source.json beside it.
inline void write_fixture_config(const std::filesystem::path& file, const std::string& workflow,
                                 const std::string& run, const std::string& steps) {
    write_text_file(file.parent_path() / "source.json", "{\"value\": 42}");
    write_text_file(file, "kind: workflow-config\n"
                          "format_version: \"0.1.0\"\n"
                          "workflow: { name: " +
                              workflow + ", version: \"0.1.0\" }\n" + run +
                              "inputs:\n"
                              "  source: { source: json-document, path: source.json }\n"
                              "steps:\n" +
                              steps);
}

// Whether `state` is one that a run ends in.
inline bool is_final(WorkflowState state) {
    return state == WorkflowState::completed || state == WorkflowState::stopped ||
           state == WorkflowState::failed;
}

// Waits, at most `timeout`, until `done()` holds; returns whether it does.
inline bool wait_for(const std::function<bool()>& done,
                     std::chrono::milliseconds timeout = std::chrono::seconds(60)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return done();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// The praenora::Error that `call` throws; a test failure when it throws none.
template <class Call> Error error_of(Call&& call) {
    try {
        call();
    } catch (const Error& error) {
        return error;
    }
    ADD_FAILURE() << "no praenora::Error was thrown";
    return Error(ErrorCode::OK, "nothing thrown");
}

} // namespace praenora::test

#endif // PRAENORA_TESTS_SERVER_SERVER_FIXTURES_HPP
