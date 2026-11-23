// Module instances in a module host (praenora-module-host), white-box: calls with the results they
// have in the runtime's process; logs and lookups; cancellation and deadlines; a call that never
// returns (its module host is killed, and nothing of its library is held on to); crashes in every
// call and at load; a module host that breaks the channel's protocol; sink limits, documents and
// paths; load errors with the codes, messages and locations of the runtime's process; and a module
// host that is missing, of another build, or whose runtime dies. Every test that ends a module
// host checks that its process is gone.
#include "core/paths.hpp"
#include "discovery/manifest.hpp"
#include "instance/module_instance.hpp"
#include "isolation/child_module.hpp"
#include "isolation/child_process.hpp"
#include "loader/library_gate.hpp"
#include "loader/loaded_module.hpp"
#include "platform/platform.hpp"

#include "instance_fixtures.hpp"
#include "isolation_support.hpp"
#include "module_fixtures.hpp"
#include "scoped_environment.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/status.hpp>
#include <praenora/sdk/status.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <spdlog/logger.h>
#include <spdlog/sinks/base_sink.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <dlfcn.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Error;
using praenora::ErrorCode;
using praenora::StepState;
using praenora::detail::CallOutcome;
using praenora::detail::CallResult;
using praenora::detail::ChildSettings;
using praenora::detail::InstanceOptions;
using praenora::detail::LibraryGate;
using praenora::detail::load_manifest;
using praenora::detail::LoadedModule;
using praenora::detail::ModuleInstance;
using praenora::detail::ModuleManifest;
using praenora::test::CapturedLogs;
using praenora::test::create_document;
using praenora::test::fixture_manifest;
using praenora::test::initialize_document;
using praenora::test::instance_options;
using praenora::test::invocation_document;
using praenora::test::load_fixture_module;
using praenora::test::ScratchDir;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

#ifndef _WIN32

using praenora::test::child_of;
using praenora::test::child_settings;
using praenora::test::gone_within;
using praenora::test::load_in_child;
using praenora::test::module_host_pid;
using praenora::test::process_gone;
using praenora::test::process_running;

namespace {

// What a call came to, as the instance's caller sees it.
struct Seen {
    CallOutcome outcome;
    praenora_status status;
    std::string output, error_json;

    bool operator==(const Seen& other) const {
        return outcome == other.outcome && status == other.status && output == other.output &&
               error_json == other.error_json;
    }
};

std::ostream& operator<<(std::ostream& out, const Seen& seen) {
    return out << praenora::detail::to_string(seen.outcome) << " status " << seen.status
               << " output '" << seen.output << "' error '" << seen.error_json << "'";
}

Seen seen(const CallResult& result) {
    return Seen{result.outcome, result.status, result.output, result.error_json};
}

std::shared_ptr<LoadedModule> load(bool child, std::string_view name, CapturedLogs& logs) {
    return child ? load_in_child(name, logs) : load_fixture_module(name);
}

const char* mode_name(bool child) { return child ? "in a module host" : "in this process"; }

// The event of `logs` whose attributes.event is `name`, the last one; null without one.
json event_named(const CapturedLogs& logs, const std::string& name) {
    json found;
    for (const json& event : logs.events()) {
        if (event.at("attributes").value("praenora.event", std::string()) == name) {
            found = event;
        }
    }
    return found;
}

// The pid of the last module host that `logs` saw start.
std::int64_t started_pid(const CapturedLogs& logs) {
    const json started = event_named(logs, "module_host.started");
    return started.is_null() ? 0 : started.at("attributes").at("praenora.pid").get<std::int64_t>();
}

// Waits up to 10 s until `logs` hold a message starting with `prefix`.
bool logged_within_10s(const CapturedLogs& logs, const std::string& prefix) {
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (logs.bodies(prefix).empty()) {
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(milliseconds(5));
    }
    return true;
}

// The pid that fx-crash logged for the process it started ("fx-crash: child <pid>"), or 0.
std::int64_t child_from_logs(const CapturedLogs& logs) {
    const auto bodies = logs.bodies("fx-crash: child ");
    return bodies.empty() ? 0 : std::stoll(bodies.back().substr(16));
}

// A log sink that holds whoever logs a module host's crash for four seconds.
class SlowCrashSink final : public spdlog::sinks::base_sink<std::mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& message) override {
        const std::string_view text(message.payload.data(), message.payload.size());
        if (text.find("crashed during") != std::string_view::npos) {
            std::this_thread::sleep_for(std::chrono::seconds(4));
        }
    }
    void flush_() override {}
};

// A thread that waits until a call into `instance` runs, then `delay` more, then cancels it.
std::thread cancel_when_busy(ModuleInstance& instance, milliseconds delay) {
    return std::thread([&instance, delay] {
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (instance.state() != StepState::busy && Clock::now() < deadline) {
            std::this_thread::sleep_for(milliseconds(1));
        }
        std::this_thread::sleep_for(delay);
        instance.request_cancel();
    });
}

// Checks the details of a MODULE_CRASHED for a call ended `how` ("segv", "abort", "exit"), in a
// module host started under OwnSignals.
void expect_crash_details(const json& details, const std::string& phase, const std::string& how) {
    if (how == "exit") {
        EXPECT_EQ(details, (json{{"phase", phase}, {"exit_code", 0}}));
        return;
    }
    const int signal = how == "segv" ? SIGSEGV : SIGABRT;
    EXPECT_EQ(details, (json{{"phase", phase},
                             {"signal", signal},
                             {"signal_name", praenora::detail::signal_name(signal)}}));
}

// Runs an fx-crash instance in a module host until the call `when` ends its process `how`, and
// checks that the crash is reported, that no module host is left, and that no later call reaches
// the channel.
void expect_crash(const std::string& when, const std::string& how) {
    SCOPED_TRACE(when + " by " + how);
    const praenora::test::OwnSignals own_signals;
    CapturedLogs logs;
    const auto module = load_in_child("fx-crash", logs);
    const std::int64_t pid = module_host_pid(*module);
    ModuleInstance instance(module, instance_options(logs));
    std::optional<CallResult> crashed;
    const auto call = [&crashed](const CallResult& result, const char* name) {
        if (result.outcome == CallOutcome::crashed) {
            crashed = result;
            return false;
        }
        EXPECT_EQ(result.outcome, CallOutcome::succeeded) << name << ": " << result.error_json;
        return true;
    };
    if (call(instance.create(create_document("fx-crash", {{"when", when}, {"how", how}})),
             "create") &&
        call(instance.initialize(initialize_document()), "initialize") &&
        call(instance.invoke(invocation_document(0), 0), "invoke")) {
        if (when == "diagnostics") {
            EXPECT_EQ(instance.diagnostics(), std::nullopt);
        } else if (call(instance.finalize(), "finalize")) {
            EXPECT_EQ(instance.destroy(), CallOutcome::crashed);
        }
    }
    EXPECT_EQ(instance.state(), StepState::crashed);
    // The module host is gone, reaped, by the time the call has returned.
    EXPECT_TRUE(process_gone(pid)) << pid;
    if (crashed) {
        EXPECT_EQ(crashed->status, PRAENORA_STATUS_INTERNAL);
        EXPECT_TRUE(crashed->called);
        expect_crash_details(json::parse(crashed->details_json), when, how);
    }
    const json event = event_named(logs, "module_host.crashed");
    ASSERT_FALSE(event.is_null());
    EXPECT_EQ(event.at("severity_text"), "ERROR");
    EXPECT_EQ(event.at("attributes").at("praenora.pid"), pid);
    EXPECT_NE(event.at("body").get<std::string>().find("during " + when), std::string::npos)
        << event.at("body");

    // Nothing reaches the channel any more.
    const std::uint64_t sent = child_of(*module).requests_sent();
    EXPECT_EQ(instance.diagnostics(), std::nullopt);
    EXPECT_EQ(instance.destroy(), CallOutcome::crashed);
    EXPECT_THROW(instance.finalize(), Error);
    EXPECT_THROW(instance.invoke(invocation_document(1), 1), Error);
    EXPECT_EQ(child_of(*module).requests_sent(), sent);
}

struct LoadFailure {
    ErrorCode code = ErrorCode::OK;
    std::string message;
    praenora::SourceLocation location;
    std::string details;
};

LoadFailure failure_of(bool child, const ModuleManifest& manifest,
                       const std::optional<std::string>& expected = std::nullopt) {
    try {
        if (child) {
            CapturedLogs logs;
            ChildSettings settings = child_settings(logs);
            settings.library_sha256 = expected;
            static_cast<void>(LoadedModule::load_in_child(manifest, settings));
        } else {
            static_cast<void>(LoadedModule::load(manifest, nullptr, expected));
        }
    } catch (const Error& error) {
        return LoadFailure{error.code(), error.what(), error.location(), error.details_json()};
    }
    return LoadFailure{};
}

// Loading `manifest` fails with `code`, and alike in this process and in a module host.
void expect_same_load_failure(const ModuleManifest& manifest, ErrorCode code,
                              const std::optional<std::string>& expected = std::nullopt) {
    SCOPED_TRACE(manifest.name);
    const LoadFailure here = failure_of(false, manifest, expected);
    const LoadFailure there = failure_of(true, manifest, expected);
    EXPECT_EQ(here.code, code) << here.message;
    EXPECT_EQ(there.code, here.code) << there.message;
    EXPECT_EQ(there.message, here.message);
    EXPECT_EQ(there.location.file, here.location.file);
    EXPECT_EQ(there.location.pointer, here.location.pointer);
    EXPECT_EQ(there.location.line, here.location.line);
    EXPECT_EQ(there.location.column, here.location.column);
    EXPECT_EQ(json::parse(there.details), json::parse(here.details));
    EXPECT_EQ(here.location.file, praenora::detail::path_to_utf8(manifest.manifest_path));
}

// Copies fixture module `name` (library and manifest) into `directory`; returns the manifest.
fs::path copy_fixture(std::string_view name, const fs::path& directory) {
    const ModuleManifest manifest = load_manifest(fixture_manifest(name));
    fs::create_directories(directory);
    const auto options = fs::copy_options::overwrite_existing;
    fs::copy_file(manifest.library_path, directory / manifest.library_path.filename(), options);
    const fs::path copied = directory / manifest.manifest_path.filename();
    fs::copy_file(manifest.manifest_path, copied, options);
    return copied;
}

template <class Edit> void edit_manifest(const fs::path& path, Edit edit) {
    json document = json::parse(praenora::detail::read_file_bytes(path));
    edit(document);
    praenora::test::write_text_file(path, document.dump(2));
}

} // namespace

TEST(Isolation, LoadsAModuleInItsModuleHostOnly) {
    CapturedLogs logs;
    const auto in_process = load_fixture_module("fx-echo");
    const auto module = load_in_child("fx-echo", logs);
    EXPECT_TRUE(module->runs_in_child());
    EXPECT_FALSE(in_process->runs_in_child());
    EXPECT_EQ(module->describe(), in_process->describe());
    EXPECT_EQ(module->library_sha256(), in_process->library_sha256());
    EXPECT_TRUE(module->code().has_diagnostics());
    EXPECT_FALSE(module->code().has_self_test());
    EXPECT_EQ(module->api().invoke, nullptr);
    EXPECT_FALSE(module->pinned());
    EXPECT_EQ(module->call_access(), LibraryGate::Access::exclusive);
    const std::int64_t pid = module_host_pid(*module);
    EXPECT_TRUE(process_running(pid));
    EXPECT_NE(pid, praenora::detail::current_process_id());
    EXPECT_EQ(child_of(*module).requests_sent(), 1u); // the load
    const json started = event_named(logs, "module_host.started");
    ASSERT_FALSE(started.is_null());
    EXPECT_EQ(started.at("severity_text"), "DEBUG");
    EXPECT_EQ(started.at("attributes").at("praenora.pid"), pid);
    EXPECT_EQ(started.at("attributes").at("praenora.module"), "fx-echo@0.1.0");

    // This process never opens the library of a module that runs in a module host.
    const auto crash = load_in_child("fx-crash", logs);
    EXPECT_EQ(crash->describe().at("name"), "fx-crash");
    void* opened = ::dlopen(crash->manifest().library_path.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    EXPECT_EQ(opened, nullptr);
    if (opened != nullptr) {
        ::dlclose(opened);
    }
}

TEST(Isolation, CallsGiveTheResultsOfTheRuntimesProcess) {
    std::vector<std::vector<Seen>> runs;
    std::vector<std::optional<std::string>> diagnostics;
    for (const bool child : {false, true}) {
        SCOPED_TRACE(mode_name(child));
        CapturedLogs logs;
        const auto module = load(child, "fx-echo", logs);
        const std::int64_t pid = child ? module_host_pid(*module) : 0;
        std::vector<Seen> run;
        {
            ModuleInstance instance(module, instance_options(logs));
            const json data = {{"text", "gr\xC3\xBC\xC3\x9F\x65 \xE6\x97\xA5\xE6\x9C\xAC"},
                               {"values", {1.5, -2, nullptr, true}},
                               {"nested", {{"deep", {{"er", "x"}}}}}};
            run.push_back(seen(instance.create(create_document("fx-echo"))));
            run.push_back(seen(instance.initialize(initialize_document())));
            run.push_back(seen(instance.invoke(invocation_document(0, &data), 0)));
            run.push_back(seen(instance.invoke(invocation_document(1), 1)));
            diagnostics.push_back(instance.diagnostics());
            run.push_back(seen(instance.finalize()));
            EXPECT_EQ(instance.destroy(), CallOutcome::succeeded);
            EXPECT_EQ(instance.state(), StepState::destroyed);
            if (child) {
                // The module host exits after destroy, and is waited for.
                EXPECT_TRUE(process_gone(pid)) << pid;
                const json exited = event_named(logs, "module_host.exited");
                ASSERT_FALSE(exited.is_null());
                EXPECT_NE(exited.at("body").get<std::string>().find("exited with code 0"),
                          std::string::npos)
                    << exited;
            }
        }
        runs.push_back(run);
    }
    EXPECT_EQ(runs[1], runs[0]);
    EXPECT_EQ(diagnostics[1], diagnostics[0]);
    EXPECT_EQ(diagnostics[0], std::optional<std::string>(R"({"invocations":2})"));
    EXPECT_EQ(json::parse(runs[0][2].output).at("outputs").at("out").at("data").at("text"),
              "gr\xC3\xBC\xC3\x9F\x65 \xE6\x97\xA5\xE6\x9C\xAC");
}

TEST(Isolation, LogsArriveInOrderBeforeTheirCallReturns) {
    CapturedLogs logs;
    InstanceOptions options = instance_options(logs);
    options.run_fingerprint = std::string(64, 'a');
    ModuleInstance instance(load_in_child("fx-lifecycle", logs), options);
    using Calls = std::vector<std::string>;
    ASSERT_EQ(instance.create(create_document("fx-lifecycle")).outcome, CallOutcome::succeeded);
    EXPECT_EQ(logs.bodies("lifecycle:"), (Calls{"lifecycle:create"}));
    ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
    EXPECT_EQ(logs.bodies("lifecycle:"), (Calls{"lifecycle:create", "lifecycle:initialize"}));
    ASSERT_EQ(instance.invoke(invocation_document(7), 7).outcome, CallOutcome::succeeded);
    EXPECT_EQ(logs.bodies("lifecycle:").size(), 3u);
    ASSERT_EQ(instance.finalize().outcome, CallOutcome::succeeded);
    instance.destroy();
    EXPECT_EQ(logs.bodies("lifecycle:"),
              (Calls{"lifecycle:create", "lifecycle:initialize", "lifecycle:invoke",
                     "lifecycle:finalize", "lifecycle:destroy"}));

    // The records keep the attributes of the runtime's process: tick during an invocation only.
    for (const json& event : logs.events()) {
        const std::string body = event.value("body", std::string());
        if (body.rfind("lifecycle:", 0) != 0) {
            continue;
        }
        const json& attributes = event.at("attributes");
        EXPECT_EQ(event.at("severity_text"), "INFO");
        EXPECT_EQ(attributes.at("logger"), "module.s");
        EXPECT_EQ(attributes.at("praenora.step"), "s");
        EXPECT_EQ(attributes.at("praenora.module"), "fx-lifecycle@0.1.0");
        EXPECT_EQ(attributes.at("praenora.run.fingerprint"), std::string(64, 'a'));
        if (body == "lifecycle:invoke") {
            EXPECT_EQ(attributes.at("praenora.tick"), 7);
        } else {
            EXPECT_FALSE(attributes.contains("praenora.tick")) << event;
        }
    }
}

TEST(Isolation, ALogFromAModuleThreadBetweenCallsArrives) {
    CapturedLogs logs;
    ModuleInstance instance(load_in_child("fx-crash", logs), instance_options(logs));
    ASSERT_EQ(instance.create(create_document("fx-crash", {{"log_later_ms", 50}})).outcome,
              CallOutcome::succeeded);
    ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
    ASSERT_EQ(instance.invoke(invocation_document(3), 3).outcome, CallOutcome::succeeded);
    EXPECT_TRUE(logs.bodies("fx-crash: later").empty());
    ASSERT_TRUE(logged_within_10s(logs, "fx-crash: later"));
    for (const json& event : logs.events()) {
        if (event.value("body", std::string()) == "fx-crash: later") {
            EXPECT_FALSE(event.at("attributes").contains("praenora.tick")) << event;
            EXPECT_EQ(event.at("attributes").at("praenora.step"), "s");
        }
    }
    EXPECT_EQ(instance.finalize().outcome, CallOutcome::succeeded);
    EXPECT_EQ(instance.destroy(), CallOutcome::succeeded);
}

TEST(Isolation, ACrashInCreateIsReported) {
    for (const char* how : {"segv", "abort", "exit"}) {
        expect_crash("create", how);
    }
}

TEST(Isolation, ACrashInInitializeIsReported) {
    for (const char* how : {"segv", "abort", "exit"}) {
        expect_crash("initialize", how);
    }
}

TEST(Isolation, ACrashInInvokeIsReported) {
    for (const char* how : {"segv", "abort", "exit"}) {
        expect_crash("invoke", how);
    }
}

TEST(Isolation, ACrashInFinalizeIsReported) {
    for (const char* how : {"segv", "abort", "exit"}) {
        expect_crash("finalize", how);
    }
}

TEST(Isolation, ACrashInDestroyIsReported) {
    for (const char* how : {"segv", "abort", "exit"}) {
        expect_crash("destroy", how);
    }
}

TEST(Isolation, ACrashInDiagnosticsIsReported) {
    for (const char* how : {"segv", "abort", "exit"}) {
        expect_crash("diagnostics", how);
    }
}

TEST(Isolation, ACrashAtLoadIsModuleCrashed) {
    const praenora::test::OwnSignals own_signals;
    CapturedLogs logs;
    const ModuleManifest manifest = load_manifest(fixture_manifest("fx-crash-at-load"));
    try {
        static_cast<void>(LoadedModule::load_in_child(manifest, child_settings(logs)));
        FAIL() << "the load did not fail";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::MODULE_CRASHED) << error.what();
        EXPECT_EQ(error.location().file, praenora::detail::path_to_utf8(manifest.manifest_path));
        expect_crash_details(json::parse(error.details_json()), "load", "segv");
        EXPECT_NE(std::string(error.what()).find("module fx-crash-at-load@0.1.0: "),
                  std::string::npos)
            << error.what();
    }
    const std::int64_t pid = started_pid(logs);
    EXPECT_TRUE(process_gone(pid)) << pid;
    EXPECT_FALSE(event_named(logs, "module_host.crashed").is_null());
}

TEST(Isolation, GarbageOnTheChannelIsACrashWithReasonProtocol) {
    for (const char* write : {"garbage", "huge-frame"}) {
        SCOPED_TRACE(write);
        CapturedLogs logs;
        const auto module = load_in_child("fx-scribble", logs);
        const std::int64_t pid = module_host_pid(*module);
        ModuleInstance instance(module, instance_options(logs));
        ASSERT_EQ(instance.create(create_document("fx-scribble", {{"write", write}})).outcome,
                  CallOutcome::succeeded);
        ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
        const CallResult result = instance.invoke(invocation_document(0), 0);
        EXPECT_EQ(result.outcome, CallOutcome::crashed);
        EXPECT_EQ(json::parse(result.details_json),
                  (json{{"phase", "invoke"}, {"reason", "protocol"}}));
        EXPECT_EQ(instance.state(), StepState::crashed);
        EXPECT_TRUE(process_gone(pid)) << pid;
        const json event = event_named(logs, "module_host.crashed");
        ASSERT_FALSE(event.is_null());
        const std::string body = event.at("body").get<std::string>();
        EXPECT_NE(body.find(std::string(write) == "garbage" ? "magic" : "over its limit"),
                  std::string::npos)
            << body;
    }
}

TEST(Isolation, ACrashIsReportedAtOnceWhenAChildHoldsTheChannel) {
    // A child the module started outlives the module host: it must neither hide the crash
    // (which an inherited, or forked, channel would) nor survive it.
    const praenora::test::OwnSignals own_signals;
    for (const char* spawn : {"sleep", "fork"}) {
        SCOPED_TRACE(spawn);
        CapturedLogs logs;
        const auto module = load_in_child("fx-crash", logs);
        const std::int64_t pid = module_host_pid(*module);
        InstanceOptions options = instance_options(logs);
        options.invoke_timeout = std::chrono::seconds(20);
        options.cancel_grace = std::chrono::seconds(5);
        ModuleInstance instance(module, std::move(options));
        ASSERT_EQ(instance
                      .create(create_document(
                          "fx-crash", {{"when", "invoke"}, {"how", "segv"}, {"spawn", spawn}}))
                      .outcome,
                  CallOutcome::succeeded);
        ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
        const auto start = Clock::now();
        const CallResult result = instance.invoke(invocation_document(0), 0);
        EXPECT_LT(Clock::now() - start, std::chrono::seconds(10));
        EXPECT_EQ(result.outcome, CallOutcome::crashed)
            << praenora::detail::to_string(result.outcome) << " " << result.details_json;
        if (result.outcome == CallOutcome::crashed) {
            expect_crash_details(json::parse(result.details_json), "invoke", "segv");
        }
        EXPECT_TRUE(process_gone(pid)) << pid;
        const std::int64_t child = child_from_logs(logs);
        ASSERT_GT(child, 0);
        EXPECT_TRUE(gone_within(child, std::chrono::seconds(5))) << child;
    }
}

TEST(Isolation, WhatAModuleStartsCannotWriteOnTheChannel) {
    CapturedLogs logs;
    ModuleInstance instance(load_in_child("fx-crash", logs), instance_options(logs));
    ASSERT_EQ(instance.create(create_document("fx-crash", {{"spawn", "write-3"}})).outcome,
              CallOutcome::succeeded);
    ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
    const CallResult result = instance.invoke(invocation_document(0), 0);
    EXPECT_EQ(result.outcome, CallOutcome::succeeded) << result.details_json;
    EXPECT_GT(child_from_logs(logs), 0);
    EXPECT_EQ(instance.invoke(invocation_document(1), 1).outcome, CallOutcome::succeeded);
    EXPECT_TRUE(event_named(logs, "module_host.crashed").is_null());
}

TEST(Isolation, WhatAModuleHostLeavesBehindEndsWithIt) {
    CapturedLogs logs;
    ModuleInstance instance(load_in_child("fx-crash", logs), instance_options(logs));
    ASSERT_EQ(instance.create(create_document("fx-crash", {{"spawn", "sleep"}})).outcome,
              CallOutcome::succeeded);
    ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
    ASSERT_EQ(instance.invoke(invocation_document(0), 0).outcome, CallOutcome::succeeded);
    const std::int64_t child = child_from_logs(logs);
    ASSERT_GT(child, 0);
    EXPECT_TRUE(process_running(child));
    EXPECT_EQ(instance.finalize().outcome, CallOutcome::succeeded);
    EXPECT_EQ(instance.destroy(), CallOutcome::succeeded);
    EXPECT_TRUE(gone_within(child, std::chrono::seconds(5))) << child;
}

TEST(Isolation, ACrashAsTheGraceEndsIsACrash) {
    // The module host crashes after the deadline; whoever records the crash is held (a slow log
    // sink) until after the grace has ended, so the call is given up on before it can return. The
    // crash comes 300 ms into the call, and the grace ends at 1.6 s: the runtime has 1.3 s to
    // notice that the module host ended (macOS's crash reporting and a sanitizer can delay a
    // crashing process's end), and the sink holds the crash's record until 4 s after it was
    // noticed, well after the grace.
    const praenora::test::OwnSignals own_signals;
    CapturedLogs logs;
    ChildSettings settings = child_settings(logs);
    settings.logger =
        std::make_shared<spdlog::logger>("module.s", std::make_shared<SlowCrashSink>());
    const auto module =
        LoadedModule::load_in_child(load_manifest(fixture_manifest("fx-crash")), settings);
    InstanceOptions options = instance_options(logs);
    options.invoke_timeout = milliseconds(100);
    options.cancel_grace = milliseconds(1500);
    ModuleInstance instance(module, std::move(options));
    ASSERT_EQ(instance
                  .create(create_document(
                      "fx-crash", {{"when", "invoke"}, {"how", "segv"}, {"crash_after_ms", 300}}))
                  .outcome,
              CallOutcome::succeeded);
    ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
    const CallResult result = instance.invoke(invocation_document(0), 0);
    EXPECT_EQ(result.outcome, CallOutcome::crashed)
        << praenora::detail::to_string(result.outcome) << " " << result.details_json;
    EXPECT_EQ(instance.state(), StepState::crashed);
    if (result.outcome == CallOutcome::crashed) {
        expect_crash_details(json::parse(result.details_json), "invoke", "segv");
    }
}

TEST(Isolation, ASecondInstanceDoesNotTakeTheModuleHostOfTheFirst) {
    CapturedLogs logs;
    const auto module = load_in_child("fx-lifecycle", logs);
    ModuleInstance first(module, instance_options(logs, "a"));
    ASSERT_EQ(first.create(create_document("fx-lifecycle", json::object(), "a")).outcome,
              CallOutcome::succeeded);
    ASSERT_EQ(first.initialize(initialize_document()).outcome, CallOutcome::succeeded);
    ModuleInstance second(module, instance_options(logs, "b"));
    const std::uint64_t sent = child_of(*module).requests_sent();
    const CallResult refused = second.create(create_document("fx-lifecycle", json::object(), "b"));
    EXPECT_EQ(refused.outcome, CallOutcome::failed);
    EXPECT_EQ(refused.status, PRAENORA_STATUS_INVALID_STATE);
    EXPECT_EQ(child_of(*module).requests_sent(), sent); // refused here, never sent
    ASSERT_EQ(first.invoke(invocation_document(0), 0).outcome, CallOutcome::succeeded);
    // The first instance's records still go to it.
    std::size_t invokes = 0;
    for (const json& event : logs.events()) {
        if (event.value("body", std::string()) == "lifecycle:invoke") {
            ++invokes;
            EXPECT_EQ(event.at("attributes").at("praenora.step"), "a") << event;
        }
    }
    EXPECT_EQ(invokes, 1u);
}

TEST(Isolation, ACallThatNeverReturnsKillsItsModuleHost) {
    {
        // A first module host, gone again: the threads a process keeps once it has started
        // children and threads (Linux's spawning thread, a sanitizer runtime's own) exist from
        // now on, and are not counted below.
        CapturedLogs logs;
        static_cast<void>(load_in_child("fx-echo", logs));
    }
    const auto threads_before = praenora::test::thread_count();
    std::weak_ptr<spdlog::logger> instance_logger;
    std::int64_t pid = 0;
    {
        CapturedLogs logs;
        const auto module = load_in_child("fx-hang", logs);
        pid = module_host_pid(*module);
        InstanceOptions options = instance_options(logs);
        options.invoke_timeout = milliseconds(100);
        options.cancel_grace = milliseconds(100);
        instance_logger = options.logger;
        {
            ModuleInstance instance(module, std::move(options));
            ASSERT_EQ(instance
                          .create(create_document("fx-hang",
                                                  {{"hang", "invoke"}, {"without_gate", "block"}}))
                          .outcome,
                      CallOutcome::succeeded);
            ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
            const auto start = Clock::now();
            const CallResult result = instance.invoke(invocation_document(0), 0);
            EXPECT_EQ(result.outcome, CallOutcome::unresponsive);
            EXPECT_EQ(result.status, PRAENORA_STATUS_TIMEOUT);
            EXPECT_EQ(json::parse(result.details_json), (json{{"terminated", true}}));
            EXPECT_LT(Clock::now() - start, std::chrono::seconds(5));
            EXPECT_EQ(instance.state(), StepState::unresponsive);
            EXPECT_TRUE(process_gone(pid)) << pid;
            // Nothing is pinned, no lease is left abandoned.
            EXPECT_FALSE(module->pinned());
            EXPECT_FALSE(module->gate().refusal(LibraryGate::Access::exclusive).has_value());
            EXPECT_FALSE(module->gate().refusal(LibraryGate::Access::shared).has_value());
            const json terminated = event_named(logs, "module_host.terminated");
            ASSERT_FALSE(terminated.is_null());
            EXPECT_EQ(terminated.at("severity_text"), "WARN");
            EXPECT_EQ(event_named(logs, "instance.unresponsive").at("severity_text"), "ERROR");
            EXPECT_EQ(instance.destroy(), CallOutcome::unresponsive);
        }

        // The same library loads and runs again at once: in this process, and in a new module
        // host.
        EXPECT_NO_THROW(static_cast<void>(load_fixture_module("fx-hang")));
        ModuleInstance again(load_in_child("fx-hang", logs), instance_options(logs));
        ASSERT_EQ(again.create(create_document("fx-hang", {{"hang", "none"}})).outcome,
                  CallOutcome::succeeded);
        ASSERT_EQ(again.initialize(initialize_document()).outcome, CallOutcome::succeeded);
        EXPECT_EQ(again.invoke(invocation_document(0), 0).outcome, CallOutcome::succeeded);
    }
    // No thread is left behind, and the instance's host context is not kept.
    EXPECT_TRUE(praenora::test::threads_back_to(threads_before));
    EXPECT_TRUE(instance_logger.expired());
}

TEST(Isolation, AStopEndsACallAsInTheRuntimesProcess) {
    std::vector<Seen> results;
    for (const bool child : {false, true}) {
        SCOPED_TRACE(mode_name(child));
        CapturedLogs logs;
        ModuleInstance instance(load(child, "fx-slow", logs), instance_options(logs));
        ASSERT_EQ(instance.create(create_document("fx-slow", {{"sleep_ms", 20000}})).outcome,
                  CallOutcome::succeeded);
        ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
        std::thread canceller = cancel_when_busy(instance, milliseconds(50));
        const auto start = Clock::now();
        const CallResult result = instance.invoke(invocation_document(0), 0);
        canceller.join();
        EXPECT_LT(Clock::now() - start, std::chrono::seconds(5));
        EXPECT_EQ(result.outcome, CallOutcome::cancelled);
        EXPECT_EQ(instance.state(), StepState::ready);
        results.push_back(Seen{result.outcome, result.status, result.output, std::string()});
        EXPECT_EQ(json::parse(result.error_json).at("code"), PRAENORA_STATUS_CANCELLED);
        // The next call starts without the stop.
        EXPECT_EQ(instance.finalize().outcome, CallOutcome::succeeded);
    }
    EXPECT_EQ(results[1], results[0]);
}

TEST(Isolation, AStopFlagSetBeforeTheCallStopsItAsInTheRuntimesProcess) {
    std::vector<Seen> results;
    for (const bool child : {false, true}) {
        SCOPED_TRACE(mode_name(child));
        CapturedLogs logs;
        std::atomic<bool> stop{false};
        InstanceOptions options = instance_options(logs);
        options.stop_flag = &stop;
        ModuleInstance instance(load(child, "fx-slow", logs), std::move(options));
        ASSERT_EQ(instance.create(create_document("fx-slow", {{"sleep_ms", 20000}})).outcome,
                  CallOutcome::succeeded);
        ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
        stop.store(true);
        const auto start = Clock::now();
        const CallResult result = instance.invoke(invocation_document(0), 0);
        EXPECT_LT(Clock::now() - start, std::chrono::seconds(5));
        EXPECT_TRUE(result.called);
        results.push_back(seen(result));
        stop.store(false);
    }
    EXPECT_EQ(results[0].outcome, CallOutcome::cancelled);
    EXPECT_EQ(results[1].outcome, results[0].outcome);
    EXPECT_EQ(results[1].status, results[0].status);
}

TEST(Isolation, ADeadlineEndsACallAsInTheRuntimesProcess) {
    std::vector<Seen> results;
    for (const bool child : {false, true}) {
        SCOPED_TRACE(mode_name(child));
        CapturedLogs logs;
        InstanceOptions options = instance_options(logs);
        options.invoke_timeout = milliseconds(100);
        const auto module = load(child, "fx-slow", logs);
        ModuleInstance instance(module, std::move(options));
        ASSERT_EQ(instance.create(create_document("fx-slow", {{"sleep_ms", 20000}})).outcome,
                  CallOutcome::succeeded);
        ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
        const auto start = Clock::now();
        const CallResult result = instance.invoke(invocation_document(0), 0);
        EXPECT_LT(Clock::now() - start, std::chrono::seconds(5));
        EXPECT_EQ(result.outcome, CallOutcome::timed_out);
        EXPECT_EQ(result.status, PRAENORA_STATUS_CANCELLED);
        EXPECT_TRUE(result.details_json.empty());
        results.push_back(Seen{result.outcome, result.status, result.output, std::string()});
        // A call that returned within the grace leaves a usable instance (and module host).
        EXPECT_EQ(instance.state(), StepState::ready);
        if (child) {
            EXPECT_TRUE(process_running(module_host_pid(*module)));
        }
        EXPECT_EQ(instance.diagnostics(), std::optional<std::string>(R"({"invocations":1})"));
        EXPECT_EQ(instance.finalize().outcome, CallOutcome::succeeded);
    }
    EXPECT_EQ(results[1], results[0]);
}

TEST(Isolation, SinkLimitsRefuseAtTheSameByteAsInTheRuntimesProcess) {
    std::vector<std::vector<Seen>> runs;
    for (const bool child : {false, true}) {
        SCOPED_TRACE(mode_name(child));
        CapturedLogs logs;
        InstanceOptions options = instance_options(logs);
        options.max_output_bytes = 5000;
        ModuleInstance instance(load(child, "fx-echo", logs), std::move(options));
        ASSERT_EQ(instance.create(create_document("fx-echo")).outcome, CallOutcome::succeeded);
        ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
        std::vector<Seen> run;
        const json fits = std::string(4000, 'f');
        const json too_long = std::string(12000, 'x');
        run.push_back(seen(instance.invoke(invocation_document(0, &fits), 0)));
        run.push_back(seen(instance.invoke(invocation_document(1, &too_long), 1)));
        runs.push_back(run);
    }
    EXPECT_EQ(runs[0][0].outcome, CallOutcome::succeeded);
    EXPECT_EQ(runs[0][1].status, PRAENORA_STATUS_RESOURCE_EXHAUSTED);
    EXPECT_EQ(runs[1], runs[0]);
}

TEST(Isolation, EmptyAndNonAsciiDocumentsAreAsInTheRuntimesProcess) {
    std::vector<std::vector<Seen>> runs;
    for (const bool child : {false, true}) {
        SCOPED_TRACE(mode_name(child));
        CapturedLogs logs;
        ModuleInstance instance(load(child, "fx-echo", logs), instance_options(logs));
        std::vector<Seen> run;
        run.push_back(seen(instance.create(std::string())));
        run.push_back(seen(instance.initialize(std::string())));
        run.push_back(seen(instance.invoke(std::string(), 0)));
        const json text = "\xE2\x80\x9C"
                          "caf\xC3\xA9 \xF0\x9F\x8E\x89\xE2\x80\x9D";
        run.push_back(seen(instance.invoke(invocation_document(1, &text), 1)));
        const json empty = "";
        run.push_back(seen(instance.invoke(invocation_document(2, &empty), 2)));
        runs.push_back(run);
    }
    EXPECT_EQ(runs[0][0].outcome, CallOutcome::succeeded);
    EXPECT_EQ(runs[0][2].outcome, CallOutcome::failed);
    EXPECT_EQ(runs[1], runs[0]);
}

TEST(Isolation, AModuleUnderANonAsciiPathLoadsAndRuns) {
    const ScratchDir scratch;
    const fs::path manifest_path =
        copy_fixture("fx-echo", scratch.path() / praenora::test::utf8_path("m\xC3\xB6"
                                                                           "dule dir"));
    const ModuleManifest manifest = load_manifest(manifest_path);
    CapturedLogs logs;
    const auto module = LoadedModule::load_in_child(manifest, child_settings(logs));
    EXPECT_EQ(module->library_sha256(), LoadedModule::load(manifest)->library_sha256());
    ModuleInstance instance(module, instance_options(logs));
    ASSERT_EQ(instance.create(create_document("fx-echo")).outcome, CallOutcome::succeeded);
    ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
    const json data = {{"k", 1}};
    const CallResult result = instance.invoke(invocation_document(0, &data), 0);
    ASSERT_EQ(result.outcome, CallOutcome::succeeded) << result.error_json;
    EXPECT_EQ(json::parse(result.output).at("outputs").at("out").at("data"), data);
}

TEST(Isolation, LookupsAreRecordedAsInTheRuntimesProcess) {
    std::vector<std::vector<std::string>> recorded;
    std::vector<std::size_t> logged;
    for (const bool child : {false, true}) {
        SCOPED_TRACE(mode_name(child));
        CapturedLogs logs;
        InstanceOptions options = instance_options(logs);
        options.record_lookups = true;
        ModuleInstance instance(load(child, "fx-service-optional", logs), std::move(options));
        const json parameters = {
            {"lookups", {"test.other@1"}}, {"lookup_in", {"create", "invoke"}}, {"repeat", 2}};
        ASSERT_EQ(instance.create(create_document("fx-service-optional", parameters)).outcome,
                  CallOutcome::succeeded);
        ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
        const CallResult invoked = instance.invoke(invocation_document(4), 4);
        ASSERT_EQ(invoked.outcome, CallOutcome::succeeded) << invoked.error_json;
        std::vector<std::string> lookups;
        for (const auto& lookup : instance.take_lookups()) {
            lookups.push_back(
                lookup.phase + " " + (lookup.tick ? std::to_string(*lookup.tick) : "-") + " " +
                lookup.service + "@" + std::to_string(lookup.major) + " " +
                (lookup.provided ? "provided" : "absent") + " x" + std::to_string(lookup.count));
        }
        recorded.push_back(lookups);
        std::size_t events = 0;
        for (const json& event : logs.events()) {
            events +=
                event.at("attributes").value("praenora.event", std::string()) == "service.lookup";
        }
        logged.push_back(events);
    }
    EXPECT_FALSE(recorded[0].empty());
    EXPECT_EQ(recorded[1], recorded[0]);
    EXPECT_EQ(logged[1], logged[0]);
}

TEST(Isolation, SelfTestStaysInTheRuntimesProcess) {
    CapturedLogs logs;
    const auto module = load_in_child("fx-self-test", logs);
    EXPECT_TRUE(module->code().has_self_test());
    ModuleInstance instance(module, instance_options(logs));
    try {
        static_cast<void>(instance.self_test());
        FAIL() << "self_test() ran in a module host";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::UNSUPPORTED) << error.what();
    }
    EXPECT_EQ(instance.state(), StepState::loaded);
}

TEST(Isolation, ADestroyedInstancesModuleHostIsKilledWhenItDoesNotExit) {
    CapturedLogs logs;
    const auto module = load_in_child("fx-crash", logs);
    const std::int64_t pid = module_host_pid(*module);
    InstanceOptions options = instance_options(logs);
    // The wait for the exit; create, in a module host, must return within it too.
    options.lifecycle_timeout = milliseconds(1000);
    ModuleInstance instance(module, std::move(options));
    ASSERT_EQ(instance.create(create_document("fx-crash", {{"exit_blocked", true}})).outcome,
              CallOutcome::succeeded);
    const auto start = Clock::now();
    EXPECT_EQ(instance.destroy(), CallOutcome::succeeded);
    EXPECT_LT(Clock::now() - start, std::chrono::seconds(5));
    EXPECT_EQ(instance.state(), StepState::destroyed);
    EXPECT_TRUE(process_gone(pid)) << pid;
    const json terminated = event_named(logs, "module_host.terminated");
    ASSERT_FALSE(terminated.is_null());
    EXPECT_NE(terminated.at("body").get<std::string>().find("did not exit within 1000 ms"),
              std::string::npos)
        << terminated;
}

// A module host is a background process group of the runtime's terminal, when it has one: the
// terminal would stop it with SIGTTOU when it writes to the terminal (under `stty tostop`) or
// changes the terminal's settings, and with SIGTTIN when it reads from it. A module host ignores
// both, so its calls go on as they do in the runtime's process. The signals are sent here by hand.
TEST(Isolation, TheTerminalsStopSignalsDoNotStopAModuleHost) {
    CapturedLogs logs;
    const auto module = load_in_child("fx-echo", logs);
    const std::int64_t pid = module_host_pid(*module);
    ASSERT_EQ(::kill(static_cast<pid_t>(pid), SIGTTOU), 0);
    ASSERT_EQ(::kill(static_cast<pid_t>(pid), SIGTTIN), 0);
    InstanceOptions options = instance_options(logs);
    options.lifecycle_timeout = std::chrono::seconds(5);
    options.invoke_timeout = std::chrono::seconds(5);
    options.cancel_grace = std::chrono::seconds(1);
    ModuleInstance instance(module, std::move(options));
    const CallResult created = instance.create(create_document("fx-echo"));
    EXPECT_EQ(created.outcome, CallOutcome::succeeded)
        << praenora::detail::to_string(created.outcome) << " " << created.details_json;
    ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
    const json data = {{"k", 1}};
    const CallResult invoked = instance.invoke(invocation_document(0, &data), 0);
    ASSERT_EQ(invoked.outcome, CallOutcome::succeeded) << invoked.error_json;
    EXPECT_EQ(json::parse(invoked.output).at("outputs").at("out").at("data"), data);
    EXPECT_TRUE(process_running(pid)) << pid;
}

// A load in a module host runs on a thread with the stack of a program's main thread (the soft
// limit of RLIMIT_STACK, or 8 MiB), as a load in the runtime's process runs on its caller's thread,
// here this test's main thread: a library whose static initializer uses 2 MiB of stack, more than a
// thread's default holds (512 KiB on macOS), loads and runs in either place.
TEST(Isolation, ALoadHasTheStackOfAMainThread) {
    EXPECT_NO_THROW(static_cast<void>(load_fixture_module("fx-deep-stack-at-load")));
    const praenora::test::OwnSignals own_signals;
    CapturedLogs logs;
    std::shared_ptr<LoadedModule> module;
    try {
        module = load_in_child("fx-deep-stack-at-load", logs);
    } catch (const Error& error) {
        FAIL() << error.what() << " " << error.details_json();
    }
    ModuleInstance instance(module, instance_options(logs));
    ASSERT_EQ(instance.create(create_document("fx-deep-stack-at-load")).outcome,
              CallOutcome::succeeded);
    ASSERT_EQ(instance.initialize(initialize_document()).outcome, CallOutcome::succeeded);
    const CallResult invoked = instance.invoke(invocation_document(0), 0);
    ASSERT_EQ(invoked.outcome, CallOutcome::succeeded) << invoked.error_json;
    EXPECT_GT(json::parse(invoked.output).at("outputs").at("out").at("data").at("stack"), 0);
}

TEST(Isolation, LoadErrorLibraryNotFoundIsAsInTheRuntimesProcess) {
    const ScratchDir scratch;
    const ModuleManifest manifest = load_manifest(copy_fixture("fx-echo", scratch.path() / "nf"));
    fs::remove(manifest.library_path);
    expect_same_load_failure(manifest, ErrorCode::LIBRARY_NOT_FOUND);
}

TEST(Isolation, LoadErrorLibraryDigestMismatchIsAsInTheRuntimesProcess) {
    const ScratchDir scratch;
    const fs::path manifest_path = copy_fixture("fx-echo", scratch.path() / "digest");
    const std::string digest = load_fixture_module("fx-echo")->library_sha256();
    std::string wrong = digest;
    wrong[0] = wrong[0] == '0' ? '1' : '0';
    // The digest the runtime expects (validation's), checked before the library opens.
    expect_same_load_failure(load_manifest(manifest_path), ErrorCode::LIBRARY_DIGEST_MISMATCH,
                             wrong);
    // The digest the manifest pins.
    edit_manifest(manifest_path,
                  [&wrong](json& document) { document["library"]["sha256"] = wrong; });
    expect_same_load_failure(load_manifest(manifest_path), ErrorCode::LIBRARY_DIGEST_MISMATCH);
}

TEST(Isolation, LoadErrorLibraryLoadFailedIsAsInTheRuntimesProcess) {
    const ScratchDir scratch;
    const fs::path manifest_path = copy_fixture("fx-echo", scratch.path() / "text");
    praenora::test::write_text_file(scratch.path() / "text" / "not-a-library.bin",
                                    "This is a text file, not a module library.\n");
    edit_manifest(manifest_path,
                  [](json& document) { document["library"]["path"] = "not-a-library.bin"; });
    expect_same_load_failure(load_manifest(manifest_path), ErrorCode::LIBRARY_LOAD_FAILED);
}

TEST(Isolation, LoadErrorEntryPointMissingIsAsInTheRuntimesProcess) {
    expect_same_load_failure(load_manifest(fixture_manifest("fx-no-entry")),
                             ErrorCode::ENTRY_POINT_MISSING);
}

TEST(Isolation, LoadErrorApiTableInvalidIsAsInTheRuntimesProcess) {
    for (const char* name :
         {"fx-null-table", "fx-short-table", "fx-missing-fn", "fx-reserved-field"}) {
        expect_same_load_failure(load_manifest(fixture_manifest(name)),
                                 ErrorCode::API_TABLE_INVALID);
    }
}

TEST(Isolation, LoadErrorAbiIncompatibleIsAsInTheRuntimesProcess) {
    expect_same_load_failure(load_manifest(fixture_manifest("fx-abi-major")),
                             ErrorCode::ABI_INCOMPATIBLE);
}

TEST(Isolation, LoadErrorModuleIdentityMismatchIsAsInTheRuntimesProcess) {
    for (const char* name :
         {"fx-identity-mismatch", "fx-describe-not-json", "fx-describe-invalid"}) {
        expect_same_load_failure(load_manifest(fixture_manifest(name)),
                                 ErrorCode::MODULE_IDENTITY_MISMATCH);
    }
}

TEST(Isolation, AMissingModuleHostIsUnavailable) {
    const ScratchDir empty;
    const praenora::test::ModuleHostDirectory hook(empty.path());
    CapturedLogs logs;
    const ModuleManifest manifest = load_manifest(fixture_manifest("fx-echo"));
    try {
        static_cast<void>(LoadedModule::load_in_child(manifest, child_settings(logs)));
        FAIL() << "the load did not fail";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::MODULE_HOST_UNAVAILABLE) << error.what();
        const json details = json::parse(error.details_json());
        EXPECT_EQ(details.at("reason"), "missing");
        EXPECT_EQ(error.location().file, praenora::detail::path_to_utf8(manifest.manifest_path));
        EXPECT_NE(std::string(error.what()).find("praenora-module-host"), std::string::npos)
            << error.what();
    }
    EXPECT_TRUE(event_named(logs, "module_host.started").is_null());
}

TEST(Isolation, AModuleHostThatIsNotExecutableIsUnavailable) {
    const ScratchDir directory;
    praenora::test::write_text_file(directory.path() / "praenora-module-host", "not a program\n");
    const praenora::test::ModuleHostDirectory hook(directory.path());
    CapturedLogs logs;
    const ModuleManifest manifest = load_manifest(fixture_manifest("fx-echo"));
    try {
        static_cast<void>(LoadedModule::load_in_child(manifest, child_settings(logs)));
        FAIL() << "the load did not fail";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::MODULE_HOST_UNAVAILABLE) << error.what();
        const json details = json::parse(error.details_json());
        EXPECT_EQ(details.at("reason"), "spawn");
        EXPECT_FALSE(details.value("system", std::string()).empty()) << details;
        EXPECT_NE(std::string(error.what()).find("is not executable"), std::string::npos)
            << error.what();
    }
}

TEST(Isolation, AModuleHostThatDoesNotAnswerAsThisBuildIsUnavailable) {
    const ScratchDir directory;
    fs::create_symlink(praenora::test::utf8_path(PRAENORA_TEST_FAKE_MODULE_HOST),
                       directory.path() / "praenora-module-host");
    const praenora::test::ModuleHostDirectory hook(directory.path());
    const ModuleManifest manifest = load_manifest(fixture_manifest("fx-echo"));
    struct Case {
        const char* mode;
        const char* reason;
    };
    for (const Case& c :
         {Case{"build", "build"}, Case{"protocol", "build"}, Case{"garbage", "handshake"},
          Case{"hello", "handshake"}, Case{"exit", "handshake"}, Case{"silent", "handshake"}}) {
        SCOPED_TRACE(c.mode);
        const praenora::test::ScopedEnvironment mode("PRAENORA_TEST_FAKE_MODULE_HOST", c.mode);
        CapturedLogs logs;
        ChildSettings settings = child_settings(logs);
        // Only the module host that never answers waits for the timeout.
        const bool silent = std::string(c.mode) == "silent";
        settings.start_timeout = silent ? milliseconds(300) : milliseconds(20000);
        const auto start = Clock::now();
        try {
            static_cast<void>(LoadedModule::load_in_child(manifest, settings));
            FAIL() << "the load did not fail";
        } catch (const Error& error) {
            EXPECT_EQ(error.code(), ErrorCode::MODULE_HOST_UNAVAILABLE) << error.what();
            EXPECT_EQ(json::parse(error.details_json()).at("reason"), c.reason) << error.what();
            EXPECT_EQ(error.location().file,
                      praenora::detail::path_to_utf8(manifest.manifest_path));
        }
        if (silent) {
            EXPECT_LT(Clock::now() - start, std::chrono::seconds(5));
        }
    }
}

// A raised stop flag (a run's stop) ends the start of a module host as it ends a call: the module
// host is killed and waited for, and the load fails with CANCELLED, located in the manifest. No
// timeout of its own: the handshake's and none for the load, as in the runtime's process.
TEST(Isolation, AStopEndsTheHandshake) {
    const ScratchDir directory;
    fs::create_symlink(praenora::test::utf8_path(PRAENORA_TEST_FAKE_MODULE_HOST),
                       directory.path() / "praenora-module-host");
    const praenora::test::ModuleHostDirectory hook(directory.path());
    const praenora::test::ScopedEnvironment mode("PRAENORA_TEST_FAKE_MODULE_HOST", "silent");
    const ModuleManifest manifest = load_manifest(fixture_manifest("fx-echo"));
    CapturedLogs logs;
    std::atomic<bool> stop{false};
    ChildSettings settings = child_settings(logs);
    settings.start_timeout = std::chrono::seconds(60);
    settings.stop_flag = &stop;
    std::thread stopper([&stop] {
        std::this_thread::sleep_for(milliseconds(200));
        stop.store(true);
    });
    const auto start = Clock::now();
    try {
        static_cast<void>(LoadedModule::load_in_child(manifest, settings));
        ADD_FAILURE() << "the load did not fail";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::CANCELLED) << error.what();
        EXPECT_EQ(error.location().file, praenora::detail::path_to_utf8(manifest.manifest_path));
        EXPECT_NE(std::string(error.what()).find("module fx-echo@0.1.0: "), std::string::npos)
            << error.what();
    }
    stopper.join();
    EXPECT_LT(Clock::now() - start, std::chrono::seconds(5));
    // Killed as a stop during the load kills it, and said so; it was never announced as started.
    const json terminated = event_named(logs, "module_host.terminated");
    ASSERT_FALSE(terminated.is_null());
    EXPECT_EQ(terminated.at("attributes").at("praenora.module"), "fx-echo@0.1.0");
    EXPECT_GT(terminated.at("attributes").at("praenora.pid").get<std::int64_t>(), 0);
    EXPECT_TRUE(event_named(logs, "module_host.started").is_null());
    EXPECT_TRUE(event_named(logs, "module_host.crashed").is_null());
}

TEST(Isolation, AStopEndsTheLoad) {
    CapturedLogs logs;
    const ModuleManifest manifest = load_manifest(fixture_manifest("fx-hang-at-load"));
    std::atomic<bool> stop{false};
    ChildSettings settings = child_settings(logs);
    settings.stop_flag = &stop;
    // The stop comes once the module host has started and is loading the library.
    std::thread stopper([&stop, &logs] {
        const auto deadline = Clock::now() + std::chrono::seconds(30);
        while (event_named(logs, "module_host.started").is_null() && Clock::now() < deadline) {
            std::this_thread::sleep_for(milliseconds(10));
        }
        std::this_thread::sleep_for(milliseconds(200));
        stop.store(true);
    });
    try {
        static_cast<void>(LoadedModule::load_in_child(manifest, settings));
        ADD_FAILURE() << "the load did not fail";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::CANCELLED) << error.what();
        EXPECT_EQ(error.location().file, praenora::detail::path_to_utf8(manifest.manifest_path));
        EXPECT_NE(std::string(error.what()).find("module fx-hang-at-load@0.1.0: "),
                  std::string::npos)
            << error.what();
    }
    stopper.join();
    const std::int64_t pid = started_pid(logs);
    EXPECT_TRUE(process_gone(pid)) << pid;
    EXPECT_FALSE(event_named(logs, "module_host.terminated").is_null());
    EXPECT_TRUE(event_named(logs, "module_host.crashed").is_null());
}

// A flag raised before the start starts no module host at all.
TEST(Isolation, AStopBeforeTheStartStartsNoModuleHost) {
    CapturedLogs logs;
    std::atomic<bool> stop{true};
    ChildSettings settings = child_settings(logs);
    settings.stop_flag = &stop;
    try {
        static_cast<void>(
            LoadedModule::load_in_child(load_manifest(fixture_manifest("fx-echo")), settings));
        ADD_FAILURE() << "the load did not fail";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::CANCELLED) << error.what();
    }
    EXPECT_TRUE(event_named(logs, "module_host.started").is_null());
}

TEST(Isolation, AModuleHostWhoseRuntimeDiesEnds) {
    // The probe starts a module host and writes its pid to its descriptor 3; it is then killed
    // with its process group, which does not hold the module host.
    auto probe = praenora::detail::ChildProcess::start(PRAENORA_TEST_PARENT_PROBE, {});
    std::string line;
    char c = 0;
    while (::recv(probe.channel, &c, 1, 0) == 1 && c != '\n') {
        line.push_back(c);
    }
    ASSERT_FALSE(line.empty()) << "the probe wrote no module host pid";
    const std::int64_t pid = std::stoll(line);
    EXPECT_TRUE(process_running(pid));
    probe.process->terminate();
    probe.process->wait();
    ::close(probe.channel);
    EXPECT_TRUE(gone_within(pid, std::chrono::seconds(5))) << pid;
}

#else // _WIN32

TEST(Isolation, IsNotAvailableOnWindows) {
    praenora::test::CapturedLogs logs;
    try {
        static_cast<void>(LoadedModule::load_in_child(load_manifest(fixture_manifest("fx-echo")),
                                                      ChildSettings{}));
        FAIL() << "a module host started on Windows";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::MODULE_HOST_UNAVAILABLE) << error.what();
        EXPECT_EQ(json::parse(error.details_json()).at("reason"), "spawn");
    }
}

#endif // _WIN32
