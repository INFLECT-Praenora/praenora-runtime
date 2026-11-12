// Helpers for white-box tests that drive fixture modules through ModuleInstance: a log hub whose
// events are captured, instance options, the documents of the lifecycle calls, and the test-only
// service test.gate. Header-only.
#ifndef PRAENORA_TESTS_SUPPORT_INSTANCE_FIXTURES_HPP
#define PRAENORA_TESTS_SUPPORT_INSTANCE_FIXTURES_HPP

#include "discovery/manifest.hpp"
#include "instance/module_instance.hpp"
#include "loader/loaded_module.hpp"
#include "observability/logging.hpp"

#include "log_capture.hpp"
#include "module_fixtures.hpp"
#include "test_gate.h"

#include <praenora/runtime/options.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace praenora::test {

// A log hub that logs every level as JSON lines, to its capture only.
class CapturedLogs {
public:
    CapturedLogs() : hub_(options()), capture_(hub_) {}

    detail::LogHub& hub() noexcept { return hub_; }

    // The captured events, parsed.
    std::vector<nlohmann::json> events() const { return capture_.json_lines(); }

    // The bodies of the captured events that start with `prefix`, in order.
    std::vector<std::string> bodies(std::string_view prefix) const {
        std::vector<std::string> bodies;
        for (const auto& event : capture_.json_lines()) {
            const std::string body = event.value("body", std::string());
            if (body.compare(0, prefix.size(), prefix) == 0) {
                bodies.push_back(body);
            }
        }
        return bodies;
    }

private:
    static LogOptions options() {
        LogOptions options;
        options.level = LogLevel::trace;
        options.format = LogFormat::json;
        options.to_stderr = false;
        return options;
    }

    detail::LogHub hub_;
    LogCapture capture_;
};

inline std::shared_ptr<detail::LoadedModule> load_fixture_module(std::string_view name) {
    return detail::LoadedModule::load(detail::load_manifest(fixture_manifest(name)));
}

// Options of an instance for step `step` that logs to `logs` through the logger module.<step>.
// Timeouts are generous; tests about timeouts set their own.
inline detail::InstanceOptions instance_options(CapturedLogs& logs, const std::string& step = "s") {
    detail::InstanceOptions options;
    options.step = step;
    options.lifecycle_timeout = std::chrono::seconds(30);
    options.invoke_timeout = std::chrono::seconds(30);
    options.cancel_grace = std::chrono::seconds(5);
    options.logger = logs.hub().logger("module." + step);
    return options;
}

// The create document of step `step` bound to fixture module `module` with `parameters`.
inline std::string create_document(std::string_view module,
                                   const nlohmann::json& parameters = nlohmann::json::object(),
                                   std::string_view step = "s") {
    return nlohmann::json{{"pmp_version", "0.1.0"},
                          {"step", step},
                          {"capability", {{"id", "test.echo"}, {"version", "0.1.0"}}},
                          {"module", {{"name", module}, {"version", "0.1.0"}}},
                          {"parameters", parameters}}
        .dump();
}

inline std::string initialize_document() {
    return nlohmann::json{{"pmp_version", "0.1.0"},
                          {"run", {{"fingerprint", std::string(64, '0')}, {"seed", 7}}},
                          {"time", {{"unit", "1"}, {"start", 0.0}, {"period", 1.0}}}}
        .dump();
}

// The invocation document of step "s" at `tick`, with `data` (when given) as the data of input
// `in`.
inline std::string invocation_document(std::uint64_t tick, const nlohmann::json* data = nullptr) {
    const double logical_time = static_cast<double>(tick) + 1.0;
    nlohmann::json document = {{"pmp_version", "0.1.0"},
                               {"step", "s"},
                               {"tick", tick},
                               {"logical_time", logical_time},
                               {"inputs", nlohmann::json::object()}};
    if (data != nullptr) {
        document["inputs"]["in"] = {
            {"pmp_version", "0.1.0"},
            {"id", "0000000000000000:" + std::to_string(tick) + ":inputs.in"},
            {"type", "test.blob"},
            {"type_version", "0.1.0"},
            {"source", "inputs.in"},
            {"tick", tick},
            {"logical_time", logical_time},
            {"data", *data}};
    }
    return document.dump();
}

// The test.gate service (test_gate.h), major version 0: closed until open() is called. fx-hang
// blocks until it is open. A gate whose module may outlive the test (an unresponsive instance)
// must be static, so it stays valid while the module still polls it.
class TestGate {
public:
    TestGate() noexcept {
        table_.header.abi_major = 0;
        table_.header.abi_minor = 0;
        table_.header.struct_size = static_cast<std::uint32_t>(sizeof(praenora_test_gate_v0));
        table_.header.reserved = 0;
        table_.context = this;
        table_.is_open = &TestGate::is_open;
    }
    TestGate(const TestGate&) = delete;
    TestGate& operator=(const TestGate&) = delete;

    const praenora_test_gate_v0* table() const noexcept { return &table_; }
    void open() noexcept { open_.store(true); }
    // Closes the gate again and forgets who saw it open. Only safe once no module polls it.
    void close() noexcept {
        open_.store(false);
        seen_open_.store(0);
    }
    // How many times a module has asked and found the gate open.
    int seen_open() const noexcept { return seen_open_.load(); }

private:
    static std::int32_t PRAENORA_CALL is_open(void* context) {
        auto* self = static_cast<TestGate*>(context);
        if (!self->open_.load()) {
            return 0;
        }
        self->seen_open_.fetch_add(1);
        return 1;
    }

    praenora_test_gate_v0 table_{};
    std::atomic<bool> open_{false};
    std::atomic<int> seen_open_{0};
};

} // namespace praenora::test

#endif // PRAENORA_TESTS_SUPPORT_INSTANCE_FIXTURES_HPP
