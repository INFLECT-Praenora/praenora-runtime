// Steps whose module instances run in module hosts (process isolation,
// docs/specs/process-isolation.md), through the public API only (the shared library), with the
// fixture modules and the test catalog: the outputs, message trace and run-record reproducibility
// section of the runtime's process; a module host of its own for each step; a crash mid-run that
// fails its step while the other steps end normally; a crash at load; a call that never returns,
// whose module host is killed and whose library runs again at once; a finalize or destroy of the
// teardown that never returns, killed the same way and its step's last error; a stop during a
// call and during a load, which end the run as stopped; a Ctrl+C that reaches the runtime only;
// `validate --load` in a module host, with the codes and locations of the runtime's process; and
// the run record's isolation, the override, and the status's crashed state. Each test that starts
// module hosts checks that none is left.
#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/runtime.hpp>
#include <praenora/runtime/status.hpp>
#include <praenora/runtime/workflow.hpp>

#include "module_fixtures.hpp"
#include "provider_fixtures.hpp"
#include "sandbox_support.hpp"
#include "scoped_environment.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Diagnostic;
using praenora::Error;
using praenora::ErrorCode;
using praenora::Health;
using praenora::IsolationMode;
using praenora::LogFormat;
using praenora::LogLevel;
using praenora::Runtime;
using praenora::RuntimeOptions;
using praenora::Severity;
using praenora::StatusSnapshot;
using praenora::StepState;
using praenora::ValidateOptions;
using praenora::ValidationReport;
using praenora::Workflow;
using praenora::WorkflowOptions;
using praenora::WorkflowState;
using praenora::test::OwnSignals;
using praenora::test::ScratchDir;
using praenora::test::write_text_file;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

#ifndef _WIN32

namespace {

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::string read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The fixture modules and the test catalog, without the environment; the runtime and its runs log
// at level debug, as JSON lines, to `log_file` only.
RuntimeOptions options_logging_to(const fs::path& log_file) {
    RuntimeOptions options;
    options.module_paths = {PRAENORA_TEST_MODULES_DIR};
    options.catalog_paths = {PRAENORA_TEST_CATALOG_DIR};
    options.use_environment = false;
    options.logging.to_stderr = false;
    options.logging.level = LogLevel::debug;
    options.logging.format = LogFormat::json;
    options.logging.file = utf8(log_file);
    return options;
}

// A step block binding `id` to fixture module `module` with `parameters`, and `extra` lines.
std::string step(const std::string& id, const std::string& module,
                 const std::string& parameters = "{}", const std::string& extra = "") {
    return "  " + id + ":\n    module: { name: " + module +
           ", version: \"0.1.0\" }\n    parameters: " + parameters + "\n" + extra;
}

// Writes `<dir>/source.json` and `<dir>/workflow.yaml`, a configuration of the test workflow
// `workflow` with `run` and `steps` (YAML blocks), and returns the configuration's path.
std::string write_config(const fs::path& dir, const std::string& workflow, const std::string& run,
                         const std::string& steps, const std::string& extra = "") {
    write_text_file(dir / "source.json", "{\"value\": \"d\xC3\xA9j\xC3\xA0 vu\", \"n\": [1, 2]}");
    const fs::path file = dir / "workflow.yaml";
    write_text_file(file, "kind: workflow-config\n"
                          "format_version: \"0.1.0\"\n"
                          "workflow: { name: " +
                              workflow + ", version: \"0.1.0\" }\n" + run +
                              "inputs:\n"
                              "  source: { source: json-document, path: source.json }\n"
                              "steps:\n" +
                              steps + extra);
    return utf8(file);
}

// The log records of `file` (JSON lines). A last line that does not parse is one the runtime is
// still writing (another thread reads the file during a run): it is left out.
std::vector<json> log_records(const fs::path& file) {
    std::vector<json> records;
    std::istringstream lines(read_text(file));
    for (std::string line; std::getline(lines, line);) {
        json record = json::parse(line, nullptr, false);
        if (record.is_discarded()) {
            if (lines.peek() == std::char_traits<char>::eof()) {
                break;
            }
            throw std::runtime_error("a log record is not JSON: " + line);
        }
        records.push_back(std::move(record));
    }
    return records;
}

std::string event_of(const json& record) {
    return record.at("attributes").value("event", std::string());
}

std::string logger_of(const json& record) {
    return record.at("attributes").value("logger", std::string());
}

// The process ids of the module hosts that the records of `file` say were started, by logger.
std::vector<std::pair<std::string, std::int64_t>> started_module_hosts(const fs::path& file) {
    std::vector<std::pair<std::string, std::int64_t>> started;
    for (const json& record : log_records(file)) {
        if (event_of(record) == "module_host.started") {
            started.emplace_back(logger_of(record),
                                 record.at("attributes").at("pid").get<std::int64_t>());
        }
    }
    return started;
}

// Whether a record of `file` has event `event` and logger `logger`.
bool has_event(const fs::path& file, const std::string& logger, const std::string& event) {
    const auto records = log_records(file);
    return std::any_of(records.begin(), records.end(), [&](const json& record) {
        return logger_of(record) == logger && event_of(record) == event;
    });
}

// The bodies that the module of step `step` logged, in order (records of logger module.<step>
// without an event: those that come from the module itself).
std::vector<std::string> module_messages(const fs::path& file, const std::string& step) {
    std::vector<std::string> messages;
    for (const json& record : log_records(file)) {
        if (logger_of(record) == "module." + step && event_of(record).empty()) {
            messages.push_back(record.at("body").get<std::string>());
        }
    }
    return messages;
}

// Waits up to `limit` until no process `pid` exists.
bool gone_within(std::int64_t pid, std::chrono::milliseconds limit) {
    const auto deadline = Clock::now() + limit;
    while (!(::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH)) {
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(milliseconds(10));
    }
    return true;
}

// Checks that every module host the records of `file` name is gone.
void expect_module_hosts_gone(const fs::path& file) {
    for (const auto& [logger, pid] : started_module_hosts(file)) {
        EXPECT_TRUE(gone_within(pid, std::chrono::seconds(5))) << logger << " pid " << pid;
    }
}

template <class Call> Error error_of(Call&& call) {
    try {
        call();
    } catch (const Error& error) {
        return error;
    }
    ADD_FAILURE() << "no praenora::Error was thrown";
    return Error(ErrorCode::OK, "nothing thrown");
}

const praenora::StepStatus& step_status(const StatusSnapshot& status, const std::string& id) {
    for (const auto& step : status.steps) {
        if (step.step == id) {
            return step;
        }
    }
    throw std::runtime_error("no step " + id + " in the status");
}

// The files of a run's output directory that hold results: every output file and the trace.
std::vector<std::string> result_files(const fs::path& out) {
    std::vector<std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(out)) {
        if (entry.is_regular_file() && entry.path().filename() != "run-record.json") {
            files.push_back(utf8(entry.path().lexically_relative(out)));
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

json run_record(const fs::path& out) { return json::parse(read_text(out / "run-record.json")); }

} // namespace

// The same configuration gives the same output files, message trace (service lookups included)
// and reproducibility section, byte for byte, with its steps in module hosts as in the runtime's
// process; the run record says where each step ran, and lists the override.
TEST(IsolationRun, GivesTheResultsOfTheRuntimesProcess) {
    ScratchDir dir;
    const std::string config = write_config(
        dir.path(), "test.chain", "run: { ticks: 3, seed: 11 }\n",
        step("a", "fx-echo") + step("b", "fx-lifecycle"), "outputs: { trace_messages: true }\n");
    std::map<IsolationMode, fs::path> outs;
    for (const IsolationMode mode : {IsolationMode::in_process, IsolationMode::process}) {
        const std::string name = mode == IsolationMode::process ? "process" : "in-process";
        SCOPED_TRACE(name);
        const fs::path log = dir.path() / (name + ".log");
        const Runtime runtime(options_logging_to(log));
        WorkflowOptions options;
        options.output_dir = utf8(dir.path() / name);
        options.isolation = mode;
        {
            const auto workflow = runtime.open(config, options);
            EXPECT_EQ(workflow->run().final_state, WorkflowState::completed);
        }
        outs[mode] = dir.path() / name;
        EXPECT_EQ(started_module_hosts(log).size(), mode == IsolationMode::process ? 2u : 0u);
        expect_module_hosts_gone(log);
        // The module's own log records are the same too.
        EXPECT_EQ(
            module_messages(log, "b"),
            (std::vector<std::string>{"lifecycle:create", "lifecycle:initialize",
                                      "lifecycle:invoke", "lifecycle:invoke", "lifecycle:invoke",
                                      "lifecycle:finalize", "lifecycle:destroy"}));
    }
    const fs::path& in_process = outs.at(IsolationMode::in_process);
    const fs::path& process = outs.at(IsolationMode::process);
    const auto files = result_files(in_process);
    EXPECT_EQ(files, result_files(process));
    EXPECT_EQ(files, (std::vector<std::string>{"outputs/result.jsonl", "trace/messages.jsonl"}));
    for (const std::string& file : files) {
        EXPECT_EQ(read_text(in_process / file), read_text(process / file)) << file;
    }
    const json first = run_record(in_process);
    const json second = run_record(process);
    EXPECT_EQ(first.at("fingerprint"), second.at("fingerprint"));
    EXPECT_EQ(first.at("reproducibility").dump(), second.at("reproducibility").dump());
    EXPECT_EQ(first.at("execution").at("outputs"), second.at("execution").at("outputs"));
    EXPECT_EQ(first.at("execution").at("trace"), second.at("execution").at("trace"));
    EXPECT_EQ(first.at("execution").at("isolation"),
              json::parse(R"({"a":{"mode":"in-process","share_compiled_models":false,)"
                          R"("sandbox":"none"},)"
                          R"("b":{"mode":"in-process","share_compiled_models":false,)"
                          R"("sandbox":"none"}})"));
    EXPECT_EQ(second.at("execution").at("isolation"),
              json::parse(R"({"a":{"mode":"process","share_compiled_models":false,)"
                          R"("sandbox":"none"},)"
                          R"("b":{"mode":"process","share_compiled_models":false,)"
                          R"("sandbox":"none"}})"));
    EXPECT_EQ(first.at("execution").at("overrides"), json({{"isolation", "in-process"}}));
    EXPECT_EQ(second.at("execution").at("overrides"), json({{"isolation", "process"}}));
}

// Each step in process mode has a module host of its own, also when two steps use one module; the
// configuration sets the mode per step or for the run.
TEST(IsolationRun, EachStepHasAModuleHostOfItsOwn) {
    ScratchDir dir;
    const fs::path log = dir.path() / "run.log";
    const Runtime runtime(options_logging_to(log));
    const std::string config =
        write_config(dir.path(), "test.chain", "run: { ticks: 2 }\n",
                     step("a", "fx-echo", "{}", "    isolation: { mode: process }\n") +
                         step("b", "fx-echo", "{}", "    isolation: { mode: process }\n"));
    const auto workflow = runtime.open(config, {});
    EXPECT_EQ(workflow->run().final_state, WorkflowState::completed);
    const json record = json::parse(workflow->run_record_json());
    EXPECT_EQ(record.at("execution").at("isolation"),
              json::parse(R"({"a":{"mode":"process","share_compiled_models":false,)"
                          R"("sandbox":"none"},)"
                          R"("b":{"mode":"process","share_compiled_models":false,)"
                          R"("sandbox":"none"}})"));
    EXPECT_EQ(record.at("execution").at("overrides"), json::object());
    const auto started = started_module_hosts(log);
    ASSERT_EQ(started.size(), 2u);
    EXPECT_EQ(started[0].first, "module.a");
    EXPECT_EQ(started[1].first, "module.b");
    EXPECT_NE(started[0].second, started[1].second);
    EXPECT_TRUE(has_event(log, "module.a", "module_host.exited"));
    EXPECT_TRUE(has_event(log, "module.b", "module_host.exited"));
    expect_module_hosts_gone(log);
}

namespace {

// Points this process's descriptor 1 at `file` (made, or emptied) while it lives, and back at what
// it was then: a module host started meanwhile has the file as its standard output, as it has the
// runtime's (a module host takes descriptors 1 and 2 as they are when it starts). Whatever this
// process writes to its standard output meanwhile goes there too.
class StandardOutputTo {
public:
    explicit StandardOutputTo(const fs::path& file) {
        static_cast<void>(std::fflush(stdout));
        saved_ = ::fcntl(1, F_DUPFD_CLOEXEC, 0);
        const int descriptor = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (saved_ >= 0 && descriptor >= 0) {
            redirected_ = ::dup2(descriptor, 1) == 1;
        }
        if (descriptor >= 0) {
            static_cast<void>(::close(descriptor));
        }
    }
    ~StandardOutputTo() {
        static_cast<void>(std::fflush(stdout));
        if (redirected_) {
            static_cast<void>(::dup2(saved_, 1));
        }
        if (saved_ >= 0) {
            static_cast<void>(::close(saved_));
        }
    }
    StandardOutputTo(const StandardOutputTo&) = delete;
    StandardOutputTo& operator=(const StandardOutputTo&) = delete;

    bool redirected() const noexcept { return redirected_; }

private:
    int saved_ = -1;
    bool redirected_ = false;
};

} // namespace

// Without a lifecycle timeout (`timeouts.lifecycle: none`), a step's module host is asked to exit
// after destroy, and the runtime waits for its exit without limit: the module host exits on its
// own (module_host.exited, never module_host.terminated), and flushes its standard streams as it
// does, so what the module left in the buffer of its standard output (a file here) reaches it.
TEST(IsolationRun, WithoutALifecycleTimeoutAModuleHostExitsWhenAsked) {
    ScratchDir dir;
    const fs::path log = dir.path() / "run.log";
    const fs::path printed = dir.path() / "stdout.txt";
    const Runtime runtime(options_logging_to(log));
    const std::string config = write_config(
        dir.path(), "test.single",
        "run:\n  ticks: 2\n  isolation: { mode: process }\n  timeouts: { lifecycle: none }\n",
        step("a", "fx-crash", "{ print: \"fx-crash: buffered\\n\" }"));
    {
        const StandardOutputTo redirect(printed);
        ASSERT_TRUE(redirect.redirected());
        const auto workflow = runtime.open(config, {});
        EXPECT_EQ(workflow->run().final_state, WorkflowState::completed);
    }
    EXPECT_EQ(read_text(printed), "fx-crash: buffered\nfx-crash: buffered\n");
    EXPECT_TRUE(has_event(log, "module.a", "module_host.exited"));
    EXPECT_FALSE(has_event(log, "module.a", "module_host.terminated"));
    expect_module_hosts_gone(log);
}

// A module host that crashes in a later tick fails its step (MODULE_CRASHED, with how it ended)
// and the run; the other step is finalized and destroyed as usual, and the crashed step's state
// is `crashed`.
TEST(IsolationRun, ACrashMidRunFailsItsStepWhileTheOthersEndNormally) {
    const OwnSignals own_signals;
    ScratchDir dir;
    const fs::path log = dir.path() / "run.log";
    const Runtime runtime(options_logging_to(log));
    const std::string config =
        write_config(dir.path(), "test.chain", "run: { ticks: 5, isolation: { mode: process } }\n",
                     step("a", "fx-lifecycle") +
                         step("b", "fx-crash", "{ when: invoke, how: segv, skip_invocations: 2 }"));
    WorkflowOptions options;
    options.output_dir = utf8(dir.path() / "out");
    const auto workflow = runtime.open(config, options);
    const Error error = error_of([&] { workflow->run(); });
    EXPECT_EQ(error.code(), ErrorCode::MODULE_CRASHED) << error.what();
    EXPECT_TRUE(contains(error.what(), "step 'b'")) << error.what();
    EXPECT_TRUE(contains(error.what(), "SIGSEGV")) << error.what();
    const json details = json::parse(error.details_json());
    EXPECT_EQ(details.at("step"), "b");
    EXPECT_EQ(details.at("phase"), "invoke");
    EXPECT_EQ(details.at("tick"), 2);
    EXPECT_EQ(details.at("signal_name"), "SIGSEGV");
    EXPECT_FALSE(details.contains("diagnostics")); // never asked of a crashed instance

    const StatusSnapshot status = workflow->status();
    EXPECT_EQ(status.state, WorkflowState::failed);
    EXPECT_EQ(status.ticks_completed, 2u);
    EXPECT_EQ(status.health, Health::unhealthy);
    EXPECT_EQ(step_status(status, "b").state, StepState::crashed);
    EXPECT_EQ(step_status(status, "a").state, StepState::destroyed);
    EXPECT_EQ(step_status(status, "b").failed, 1u);
    ASSERT_TRUE(step_status(status, "b").last_error.has_value());
    EXPECT_EQ(step_status(status, "b").last_error->code, ErrorCode::MODULE_CRASHED);
    // Step a ended normally, in its own module host.
    const auto messages = module_messages(log, "a");
    ASSERT_GE(messages.size(), 2u);
    EXPECT_EQ(messages[messages.size() - 2], "lifecycle:finalize");
    EXPECT_EQ(messages.back(), "lifecycle:destroy");
    EXPECT_TRUE(has_event(log, "module.a", "module_host.exited"));
    EXPECT_TRUE(has_event(log, "module.b", "module_host.crashed"));
    // The run record has the error, with how the module host ended; the status file too.
    const json record = run_record(dir.path() / "out");
    EXPECT_EQ(record.at("execution").at("status"), "failed");
    EXPECT_EQ(record.at("execution").at("error").at("name"), "MODULE_CRASHED");
    EXPECT_EQ(record.at("execution").at("error").at("details").at("signal_name"), "SIGSEGV");
    const json final_status = record.at("execution").at("final_status");
    EXPECT_EQ(final_status.at("steps").at(1).at("step"), "b");
    EXPECT_EQ(final_status.at("steps").at(1).at("state"), "crashed");
    expect_module_hosts_gone(log);
}

namespace {

// A crash of step b's module host (fx-crash) in the call `when`, in the way `how`, during a run
// of test.chain whose step a (fx-lifecycle) is in a module host too, and what the run comes to.
struct CrashCase {
    const char* name;    // the GoogleTest name of the case
    const char* when;    // create, initialize, invoke, finalize, destroy or diagnostics
    const char* how;     // segv, abort or exit
    WorkflowState state; // how the run ends
    std::optional<ErrorCode> run_error; // the error of a failed run
    // Whether the error, or the step's last error, holds how the module host ended (the phase and
    // the signal or exit code); not for diagnostics, where the run's error is the invoke's
    // (STEP_FAILED) and only the module_host.crashed record names the crash.
    bool crash_details;
};

std::ostream& operator<<(std::ostream& out, const CrashCase& crash) { return out << crash.name; }

const CrashCase kCrashCases[] = {
    {"CreateSegv", "create", "segv", WorkflowState::failed, ErrorCode::MODULE_CRASHED, true},
    {"InitializeAbort", "initialize", "abort", WorkflowState::failed, ErrorCode::MODULE_CRASHED,
     true},
    {"InvokeSegv", "invoke", "segv", WorkflowState::failed, ErrorCode::MODULE_CRASHED, true},
    {"InvokeAbort", "invoke", "abort", WorkflowState::failed, ErrorCode::MODULE_CRASHED, true},
    {"InvokeExit", "invoke", "exit", WorkflowState::failed, ErrorCode::MODULE_CRASHED, true},
    // A crash in the teardown leaves the run's ending as it was; the step's last error says it,
    // and a warning (teardown.finalize_failed, teardown.destroy_failed).
    {"FinalizeExit", "finalize", "exit", WorkflowState::completed, std::nullopt, true},
    {"DestroySegv", "destroy", "segv", WorkflowState::completed, std::nullopt, true},
    // A run asks a failing step for its diagnostics: fx-crash's invoke fails (STEP_FAILED), and
    // its module host crashes in the diagnostics call; the run's error stays the invoke's.
    {"DiagnosticsAbort", "diagnostics", "abort", WorkflowState::failed, ErrorCode::STEP_FAILED,
     false},
};

// How the module host ended, as `how` makes it end: {"signal_name"} or {"exit_code"}.
void expect_ended_by(const json& details, const std::string& how, const std::string& phase) {
    EXPECT_EQ(details.value("phase", std::string()), phase) << details.dump();
    if (how == "exit") {
        EXPECT_EQ(details.value("exit_code", -1), 0) << details.dump();
    } else {
        EXPECT_EQ(details.value("signal_name", std::string()),
                  how == "segv" ? "SIGSEGV" : "SIGABRT")
            << details.dump();
    }
}

// The bodies of the records of `file` with logger `logger` and event `event`, in order.
std::vector<std::string> event_bodies(const fs::path& file, const std::string& logger,
                                      const std::string& event) {
    std::vector<std::string> bodies;
    for (const json& record : log_records(file)) {
        if (logger_of(record) == logger && event_of(record) == event) {
            bodies.push_back(record.at("body").get<std::string>());
        }
    }
    return bodies;
}

class IsolationRunCrash : public testing::TestWithParam<CrashCase> {};

} // namespace

// A crash in any call of a step in a module host ends only that step: the step is `crashed` and
// never called again, the other step is finalized and destroyed as usual, the run's health is
// unhealthy, and no module host is left. A crash during a call of the run fails it with
// MODULE_CRASHED; one in the teardown leaves the run's ending as it was.
TEST_P(IsolationRunCrash, EndsOnlyItsStep) {
    const CrashCase& crash = GetParam();
    const OwnSignals own_signals;
    ScratchDir dir;
    const fs::path log = dir.path() / "run.log";
    const Runtime runtime(options_logging_to(log));
    const std::string when = crash.when;
    const std::string parameters =
        "{ when: " + when + ", how: " + crash.how +
        (when == "diagnostics" ? std::string(", fail_invoke: true") : std::string()) + " }";
    const std::string config =
        write_config(dir.path(), "test.chain", "run: { ticks: 2, isolation: { mode: process } }\n",
                     step("a", "fx-lifecycle") + step("b", "fx-crash", parameters));
    const auto workflow = runtime.open(config, {});
    std::optional<Error> error;
    try {
        workflow->run();
    } catch (const Error& e) {
        error = e;
    }
    const StatusSnapshot status = workflow->status();
    EXPECT_EQ(status.state, crash.state);
    EXPECT_EQ(error.has_value(), crash.run_error.has_value()) << (error ? error->what() : "");
    if (error && crash.run_error) {
        EXPECT_EQ(error->code(), *crash.run_error) << error->what();
        const json details = json::parse(error->details_json());
        EXPECT_EQ(details.value("step", std::string()), "b");
        EXPECT_FALSE(details.contains("diagnostics")); // never from a crashed instance
        if (crash.crash_details) {
            expect_ended_by(details, crash.how, when);
        }
    }
    const praenora::StepStatus& b = step_status(status, "b");
    EXPECT_EQ(b.state, StepState::crashed);
    if (crash.crash_details && !crash.run_error) {
        // A crash in the teardown: the step's last error, and the teardown's warning.
        ASSERT_TRUE(b.last_error.has_value());
        EXPECT_EQ(b.last_error->code, ErrorCode::MODULE_CRASHED);
        expect_ended_by(json::parse(b.last_error->details_json), crash.how, when);
        const auto warnings = event_bodies(log, "praenora.engine", "teardown." + when + "_failed");
        ASSERT_EQ(warnings.size(), 1u);
        EXPECT_TRUE(contains(warnings.front(), when + " crashed")) << warnings.front();
    }
    EXPECT_EQ(status.health, Health::unhealthy);
    EXPECT_EQ(step_status(status, "a").state, StepState::destroyed);
    const auto messages = module_messages(log, "a");
    ASSERT_GE(messages.size(), 2u);
    EXPECT_EQ(messages[messages.size() - 2], "lifecycle:finalize");
    EXPECT_EQ(messages.back(), "lifecycle:destroy");
    // The record of the crash names the call it ended, whatever else records it.
    const auto crashes = event_bodies(log, "module.b", "module_host.crashed");
    ASSERT_EQ(crashes.size(), 1u);
    EXPECT_TRUE(contains(crashes.front(), "crashed during " + when)) << crashes.front();
    expect_module_hosts_gone(log);
}

INSTANTIATE_TEST_SUITE_P(EveryCall, IsolationRunCrash, testing::ValuesIn(kCrashCases),
                         [](const testing::TestParamInfo<CrashCase>& info) {
                             return std::string(info.param.name);
                         });

// A library that changes after validation is refused when a run loads it in its module host, as
// in the runtime's process: LIBRARY_DIGEST_MISMATCH at /library/path, with the same message,
// before any of its code runs.
TEST(IsolationRun, ALibraryThatChangedAfterValidationIsRefused) {
    ScratchDir dir;
    // fx-echo, copied: its manifest names its library relative to itself.
    const fs::path modules = dir.path() / "modules";
    const fs::path fixtures = praenora::test::utf8_path(PRAENORA_TEST_MODULES_DIR);
    fs::create_directories(modules);
    fs::copy(fixtures / "fx-echo", modules / "fx-echo");
    // The library file that a module's manifest names (library.path, relative to the manifest).
    const auto library_of = [](const fs::path& module_dir, const std::string& name) {
        const json manifest = json::parse(read_text(module_dir / (name + ".pmm.json")));
        return module_dir /
               praenora::test::utf8_path(manifest.at("library").at("path").get<std::string>());
    };
    const fs::path library = library_of(modules / "fx-echo", "fx-echo");
    const fs::path original = library_of(fixtures / "fx-echo", "fx-echo");
    const fs::path other = library_of(fixtures / "fx-noop", "fx-noop");
    RuntimeOptions options = options_logging_to(dir.path() / "run.log");
    options.module_paths = {utf8(modules)};
    const Runtime runtime(options);
    const std::string config = write_config(dir.path() / "config", "test.single",
                                            "run: { ticks: 1 }\n", step("a", "fx-echo"));
    std::map<IsolationMode, Error> errors;
    for (const IsolationMode mode : {IsolationMode::in_process, IsolationMode::process}) {
        fs::copy_file(original, library, fs::copy_options::overwrite_existing);
        WorkflowOptions run;
        run.isolation = mode;
        const auto workflow = runtime.open(config, run);
        fs::copy_file(other, library, fs::copy_options::overwrite_existing);
        errors.emplace(mode, error_of([&] { workflow->run(); }));
        EXPECT_EQ(step_status(workflow->status(), "a").state, StepState::failed);
    }
    const Error& in_process = errors.at(IsolationMode::in_process);
    const Error& process = errors.at(IsolationMode::process);
    EXPECT_EQ(process.code(), ErrorCode::LIBRARY_DIGEST_MISMATCH) << process.what();
    EXPECT_EQ(std::string(process.what()), std::string(in_process.what()));
    EXPECT_TRUE(contains(process.what(), "changed after validation")) << process.what();
    EXPECT_EQ(process.location().pointer, "/library/path");
    EXPECT_EQ(process.location().file, in_process.location().file);
    EXPECT_EQ(process.location().file, utf8(modules / "fx-echo" / "fx-echo.pmm.json"));
    const json details = json::parse(process.details_json());
    EXPECT_EQ(details.value("step", std::string()), "a");
    EXPECT_EQ(details.value("phase", std::string()), "load");
    expect_module_hosts_gone(dir.path() / "run.log");
}

// A module host that crashes while it loads the module (a static initializer) fails the run at
// the step's load, and the step is `crashed`.
TEST(IsolationRun, ACrashAtLoadFailsTheRun) {
    const OwnSignals own_signals;
    ScratchDir dir;
    const fs::path log = dir.path() / "run.log";
    const Runtime runtime(options_logging_to(log));
    const std::string config =
        write_config(dir.path(), "test.chain", "run: { ticks: 1, isolation: { mode: process } }\n",
                     step("a", "fx-lifecycle") + step("b", "fx-crash-at-load"));
    const auto workflow = runtime.open(config, {});
    const Error error = error_of([&] { workflow->run(); });
    EXPECT_EQ(error.code(), ErrorCode::MODULE_CRASHED) << error.what();
    const json details = json::parse(error.details_json());
    EXPECT_EQ(details.at("step"), "b");
    EXPECT_EQ(details.at("phase"), "load");
    EXPECT_EQ(details.at("signal_name"), "SIGSEGV");
    EXPECT_TRUE(contains(error.location().file, "fx-crash-at-load.pmm.json"))
        << error.location().file;
    const StatusSnapshot status = workflow->status();
    EXPECT_EQ(status.state, WorkflowState::failed);
    EXPECT_EQ(step_status(status, "b").state, StepState::crashed);
    EXPECT_EQ(step_status(status, "a").state, StepState::destroyed);
    EXPECT_EQ(status.health, Health::unhealthy);
    expect_module_hosts_gone(log);
}

// A call that never returns and never polls is killed with its module host when its deadline and
// grace pass (MODULE_UNRESPONSIVE, terminated); nothing of its library is held on to, so another
// run in the same process uses it at once, in a module host or in the runtime's process.
TEST(IsolationRun, ACallThatNeverReturnsIsKilledAndItsLibraryRunsAgainAtOnce) {
    ScratchDir dir;
    const fs::path log = dir.path() / "run.log";
    const Runtime runtime(options_logging_to(log));
    const std::string hung =
        write_config(dir.path() / "hung", "test.single",
                     "run:\n  ticks: 1\n  timeouts: { invoke: 100ms, cancel_grace: 200ms }\n"
                     "  isolation: { mode: process }\n",
                     step("a", "fx-hang", "{ hang: invoke, without_gate: block }"));
    const auto first = runtime.open(hung, {});
    const auto started = Clock::now();
    const Error error = error_of([&] { first->run(); });
    EXPECT_LT(Clock::now() - started, std::chrono::seconds(10));
    EXPECT_EQ(error.code(), ErrorCode::MODULE_UNRESPONSIVE) << error.what();
    EXPECT_TRUE(contains(error.what(), "its module host was terminated")) << error.what();
    const json details = json::parse(error.details_json());
    EXPECT_EQ(details.at("terminated"), true);
    EXPECT_EQ(details.at("phase"), "invoke");
    EXPECT_FALSE(details.contains("diagnostics"));
    EXPECT_EQ(step_status(first->status(), "a").state, StepState::unresponsive);
    EXPECT_TRUE(has_event(log, "module.a", "module_host.terminated"));
    expect_module_hosts_gone(log);

    // The same library, at once, in a module host and in the runtime's process.
    for (const char* mode : {"process", "in-process"}) {
        SCOPED_TRACE(mode);
        const std::string config =
            write_config(dir.path() / mode, "test.single",
                         std::string("run: { ticks: 2, isolation: { mode: ") + mode + " } }\n",
                         step("a", "fx-hang", "{ hang: none }"));
        const auto again = runtime.open(config, {});
        const auto start = Clock::now();
        EXPECT_EQ(again->run().final_state, WorkflowState::completed);
        EXPECT_LT(Clock::now() - start, std::chrono::seconds(10));
    }
    first->close();
    expect_module_hosts_gone(log);
}

// A finalize or destroy of the teardown that never returns and never polls is killed with its
// module host when its deadline and grace pass, as any call is. The run's ending stays as it was
// (completed), but the step is unresponsive and its last error says why: MODULE_UNRESPONSIVE,
// naming the call and that the module host was terminated, in the status and the run record,
// with a warning.
TEST(IsolationRun, ATeardownCallThatNeverReturnsIsKilledAndIsItsStepsLastError) {
    for (const std::string call : {"finalize", "destroy"}) {
        SCOPED_TRACE(call);
        ScratchDir dir;
        const fs::path log = dir.path() / "run.log";
        const Runtime runtime(options_logging_to(log));
        // The lifecycle timeout bounds create and initialize too, which return at once.
        const std::string config =
            write_config(dir.path(), "test.single",
                         "run:\n  ticks: 2\n  timeouts: { lifecycle: 1s, cancel_grace: 200ms }\n"
                         "  isolation: { mode: process }\n",
                         step("a", "fx-hang", "{ hang: " + call + ", without_gate: block }"));
        WorkflowOptions options;
        options.output_dir = utf8(dir.path() / "out");
        const auto workflow = runtime.open(config, options);
        const auto started = Clock::now();
        const praenora::RunSummary summary = workflow->run();
        EXPECT_LT(Clock::now() - started, std::chrono::seconds(10));
        EXPECT_EQ(summary.final_state, WorkflowState::completed);
        EXPECT_FALSE(summary.error.has_value()) << summary.error->message;

        const StatusSnapshot status = workflow->status();
        EXPECT_FALSE(status.last_error.has_value()); // the run did not fail
        EXPECT_EQ(status.health, Health::unhealthy);
        const praenora::StepStatus& a = step_status(status, "a");
        EXPECT_EQ(a.state, StepState::unresponsive);
        EXPECT_EQ(a.succeeded, 2u);
        ASSERT_TRUE(a.last_error.has_value());
        EXPECT_EQ(a.last_error->code, ErrorCode::MODULE_UNRESPONSIVE);
        const std::string& message = a.last_error->message;
        EXPECT_TRUE(contains(message, "step 'a' (module fx-hang@0.1.0): " + call +
                                          " did not return within its timeout of 1s and the "
                                          "cancellation grace of 200ms"))
            << message;
        EXPECT_TRUE(contains(message, "its module host was terminated")) << message;
        const json details = {{"phase", call}, {"terminated", true}};
        EXPECT_EQ(json::parse(a.last_error->details_json), details);
        EXPECT_TRUE(a.last_error->cause_json.empty()); // the call never returned

        const auto warnings = event_bodies(log, "praenora.engine", "teardown." + call + "_failed");
        ASSERT_EQ(warnings.size(), 1u);
        EXPECT_EQ(warnings.front(), message);
        if (call == "destroy") { // its finalize went as usual
            EXPECT_TRUE(event_bodies(log, "praenora.engine", "teardown.finalize_failed").empty());
        }
        EXPECT_TRUE(has_event(log, "module.a", "instance.unresponsive"));
        EXPECT_TRUE(has_event(log, "module.a", "module_host.terminated"));
        EXPECT_FALSE(has_event(log, "module.a", "module_host.crashed"));

        // The run record's final status names it too.
        const json record = run_record(dir.path() / "out");
        EXPECT_EQ(record.at("execution").at("status"), "completed");
        const json& recorded = record.at("execution").at("final_status").at("steps").at(0);
        EXPECT_EQ(recorded.at("step"), "a");
        EXPECT_EQ(recorded.at("state"), "unresponsive");
        EXPECT_EQ(recorded.at("last_error").at("name"), "MODULE_UNRESPONSIVE");
        EXPECT_EQ(recorded.at("last_error").at("message"), message);
        EXPECT_EQ(recorded.at("last_error").at("details"), details);
        workflow->close();
        expect_module_hosts_gone(log);
    }
}

namespace {

// A configuration of test.single whose step a (fx-slow, in a module host) sleeps 30 s in each
// invocation and stops as soon as its call is cancelled.
std::string slow_config(const fs::path& dir) {
    return write_config(dir, "test.single", "run: { ticks: 100, isolation: { mode: process } }\n",
                        step("a", "fx-slow", "{ sleep_ms: 30000, cooperative: true }"));
}

// Waits until the run of `workflow` is running (its first invocation has started), lets the call
// run a little, then calls `stop` and returns when it did.
template <class Stop> Clock::time_point stop_during_the_call(Workflow& workflow, Stop&& stop) {
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (workflow.state() != WorkflowState::running && Clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(10));
    }
    std::this_thread::sleep_for(milliseconds(100));
    const auto stopped_at = Clock::now();
    stop();
    return stopped_at;
}

// The stop reached the call in its module host: the run ended long before the call's 30 s were
// over, and the call itself was cancelled (one cancelled invocation, none that succeeded).
void expect_the_call_cancelled(const Workflow& workflow, Clock::time_point stopped_at) {
    EXPECT_LT(Clock::now() - stopped_at, std::chrono::seconds(5));
    const StatusSnapshot status = workflow.status();
    const praenora::StepStatus& step = step_status(status, "a");
    EXPECT_EQ(step.cancelled, 1u);
    EXPECT_EQ(step.succeeded, 0u);
    EXPECT_EQ(step.state, StepState::destroyed);
}

} // namespace

// A stop during a call in a module host reaches the call (its cancellation frame), and the run ends
// as stopped, never as a crash.
TEST(IsolationRun, AStopDuringACallEndsTheRunStopped) {
    ScratchDir dir;
    const fs::path log = dir.path() / "run.log";
    const Runtime runtime(options_logging_to(log));
    const auto workflow = runtime.open(slow_config(dir.path()), {});
    Clock::time_point stopped_at;
    std::thread stopper([&workflow, &stopped_at] {
        stopped_at = stop_during_the_call(*workflow, [&workflow] { workflow->request_stop(); });
    });
    const auto summary = workflow->run();
    stopper.join();
    EXPECT_EQ(summary.final_state, WorkflowState::stopped);
    ASSERT_TRUE(summary.error.has_value());
    EXPECT_EQ(summary.error->code, ErrorCode::RUN_STOPPED);
    EXPECT_EQ(summary.ticks_completed, 0u);
    expect_the_call_cancelled(*workflow, stopped_at);
    EXPECT_FALSE(has_event(log, "module.a", "module_host.crashed"));
    EXPECT_TRUE(has_event(log, "module.a", "module_host.exited"));
    expect_module_hosts_gone(log);
}

// A stop while a module host loads the module (whose library never finishes loading) kills that
// module host, and the run ends as stopped; nothing else needs to end it.
TEST(IsolationRun, AStopDuringTheLoadEndsTheRunStopped) {
    ScratchDir dir;
    const fs::path log = dir.path() / "run.log";
    const Runtime runtime(options_logging_to(log));
    const std::string config =
        write_config(dir.path(), "test.chain", "run: { ticks: 1, isolation: { mode: process } }\n",
                     step("a", "fx-lifecycle") + step("b", "fx-hang-at-load"));
    const auto workflow = runtime.open(config, {});
    std::thread stopper([&workflow, &log] {
        const auto deadline = Clock::now() + std::chrono::seconds(30);
        while (Clock::now() < deadline && started_module_hosts(log).size() < 2) {
            std::this_thread::sleep_for(milliseconds(10));
        }
        std::this_thread::sleep_for(milliseconds(200));
        workflow->request_stop();
    });
    const auto summary = workflow->run();
    stopper.join();
    EXPECT_EQ(summary.final_state, WorkflowState::stopped);
    ASSERT_TRUE(summary.error.has_value());
    EXPECT_EQ(summary.error->code, ErrorCode::RUN_STOPPED);
    const StatusSnapshot status = workflow->status();
    EXPECT_EQ(step_status(status, "a").state, StepState::destroyed);
    EXPECT_NE(step_status(status, "b").state, StepState::crashed);
    EXPECT_FALSE(step_status(status, "b").last_error.has_value());
    EXPECT_TRUE(has_event(log, "module.b", "module_host.terminated"));
    EXPECT_FALSE(has_event(log, "module.b", "module_host.crashed"));
    expect_module_hosts_gone(log);
}

namespace {

std::atomic<Workflow*> g_interrupted{nullptr};

extern "C" void stop_on_interrupt(int) {
    if (Workflow* workflow = g_interrupted.load()) {
        workflow->request_stop();
    }
}

} // namespace

// Ctrl+C in a terminal signals the foreground process group: the runtime's. A module host has a
// process group of its own, so the signal never reaches it: the runtime stops the call in flight
// and the run ends as stopped, not as a crash. The test process becomes the leader of a process
// group of its own and signals that group, as a terminal would; it returns to its own group at the
// end, so a run of the test executable from a terminal still gets the terminal's Ctrl+C.
TEST(IsolationRun, ACtrlCReachesTheRuntimeOnly) {
    // Back to the process group the test started in, best effort, however the test ends.
    struct Restore {
        pid_t group = ::getpgrp();
        ~Restore() {
            if (::getpgrp() != group) {
                static_cast<void>(::setpgid(0, group));
            }
        }
    } restore;
    if (::getpgrp() != ::getpid() && ::setpgid(0, 0) != 0) {
        GTEST_SKIP() << "this process cannot lead a process group of its own: "
                     << std::strerror(errno);
    }
    ScratchDir dir;
    const fs::path log = dir.path() / "run.log";
    const Runtime runtime(options_logging_to(log));
    const auto workflow = runtime.open(slow_config(dir.path()), {});
    g_interrupted.store(workflow.get());
    struct sigaction action{};
    struct sigaction previous{};
    action.sa_handler = stop_on_interrupt;
    sigemptyset(&action.sa_mask);
    ASSERT_EQ(::sigaction(SIGINT, &action, &previous), 0);
    Clock::time_point stopped_at;
    std::thread interrupter([&workflow, &stopped_at] {
        // The whole process group, as a terminal's Ctrl+C.
        stopped_at = stop_during_the_call(*workflow, [] { ::kill(0, SIGINT); });
    });
    const auto summary = workflow->run();
    interrupter.join();
    ::sigaction(SIGINT, &previous, nullptr);
    g_interrupted.store(nullptr);
    EXPECT_EQ(summary.final_state, WorkflowState::stopped);
    ASSERT_TRUE(summary.error.has_value());
    EXPECT_EQ(summary.error->code, ErrorCode::RUN_STOPPED);
    expect_the_call_cancelled(*workflow, stopped_at);
    EXPECT_FALSE(has_event(log, "module.a", "module_host.crashed"));
    EXPECT_TRUE(has_event(log, "module.a", "module_host.exited"));
    expect_module_hosts_gone(log);
}

// validate --load loads the module of a step in process mode in a module host, then ends it: its
// load errors are those of the runtime's process (code, message, location and details), and a crash
// at load is reported as MODULE_CRASHED instead of ending the runtime.
TEST(IsolationValidation, LoadChecksRunInAModuleHost) {
    ScratchDir dir;
    const fs::path log = dir.path() / "validate.log";
    const Runtime runtime(options_logging_to(log));
    ValidateOptions load;
    load.load = true;
    for (const char* module : {"fx-identity-mismatch", "fx-no-entry", "fx-describe-invalid"}) {
        SCOPED_TRACE(module);
        const auto report_for = [&](const char* mode) {
            const std::string config =
                write_config(dir.path() / module / mode, "test.single",
                             std::string("run: { ticks: 1, isolation: { mode: ") + mode + " } }\n",
                             step("a", module));
            return runtime.validate(config, load);
        };
        const ValidationReport in_process = report_for("in-process");
        const ValidationReport process = report_for("process");
        ASSERT_EQ(in_process.diagnostics.size(), 1u) << in_process.to_text();
        ASSERT_EQ(process.diagnostics.size(), 1u) << process.to_text();
        const Diagnostic& expected = in_process.diagnostics.front();
        const Diagnostic& actual = process.diagnostics.front();
        EXPECT_EQ(actual.code, expected.code);
        EXPECT_EQ(actual.message, expected.message);
        EXPECT_EQ(actual.location.file, expected.location.file);
        EXPECT_EQ(actual.location.pointer, expected.location.pointer);
        EXPECT_EQ(actual.step, "a");
        EXPECT_EQ(actual.module, expected.module);
        EXPECT_EQ(actual.details_json, expected.details_json);
    }
    {
        const OwnSignals own_signals;
        const std::string config = write_config(dir.path() / "crash", "test.single",
                                                "run: { ticks: 1, isolation: { mode: process } }\n",
                                                step("a", "fx-crash-at-load"));
        const ValidationReport report = runtime.validate(config, load);
        ASSERT_EQ(report.diagnostics.size(), 1u) << report.to_text();
        EXPECT_EQ(report.diagnostics.front().code, ErrorCode::MODULE_CRASHED);
        EXPECT_EQ(report.diagnostics.front().step, "a");
        EXPECT_EQ(json::parse(report.diagnostics.front().details_json).at("phase"), "load");
    }
    {
        const std::string config =
            write_config(dir.path() / "fine", "test.single",
                         "run: { ticks: 1, isolation: { mode: process } }\n", step("a", "fx-echo"));
        const ValidationReport report = runtime.validate(config, load);
        EXPECT_TRUE(report.diagnostics.empty()) << report.to_text();
    }
    EXPECT_FALSE(started_module_hosts(log).empty());
    expect_module_hosts_gone(log);
}

// Service providers in module hosts: a step in process mode has the providers of its module's
// services loaded and started in its module host, before its module; the runtime's process loads
// only those of its own steps (the white-box test Isolation.TheRuntimesProcessLoadsOnlyTheProviders
// OfItsSteps checks that). The fixture providers test-simulation and test-conversion are on the
// caller's provider paths.
namespace {

// options_logging_to(), with the fixture providers test-simulation and test-conversion.
RuntimeOptions provider_options_logging_to(const fs::path& log_file) {
    RuntimeOptions options = options_logging_to(log_file);
    options.provider_paths = {PRAENORA_TEST_PROVIDERS_DIR "/test-simulation",
                              PRAENORA_TEST_PROVIDERS_DIR "/test-conversion"};
    return options;
}

// The records of `file` that providers and modules wrote (loggers provider.<name> and
// module.<step>), without their time and without the module hosts' own events, which a step in the
// runtime's process has not: the records that must not depend on where a step runs. A provider's
// records of its start, up to and including its provider.loaded event, are left out too: a
// provider starts once in a process, so the runtime's process has them only when no earlier test
// of this process started the provider, and a module host always (the tests check them there).
std::vector<json> provider_and_module_records(const fs::path& file) {
    const std::vector<json> all = log_records(file);
    // Where each provider's start ends: the position of its provider.loaded event.
    std::map<std::string, std::size_t> started;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (event_of(all[i]) == "provider.loaded") {
            started.emplace(logger_of(all[i]), i);
        }
    }
    std::vector<json> records;
    for (std::size_t i = 0; i < all.size(); ++i) {
        const std::string logger = logger_of(all[i]);
        const auto start = started.find(logger);
        if ((logger.rfind("provider.", 0) != 0 && logger.rfind("module.", 0) != 0) ||
            event_of(all[i]).rfind("module_host.", 0) == 0 ||
            (start != started.end() && i <= start->second)) {
            continue;
        }
        json record = all[i];
        record.erase("timestamp");
        records.push_back(std::move(record));
    }
    return records;
}

// Runs `config` with `options`, every step in `mode` (the run's isolation override) and its outputs
// in `out`; the run must complete.
void run_completed(const RuntimeOptions& options, const std::string& config, IsolationMode mode,
                   const fs::path& out) {
    const Runtime runtime(options);
    WorkflowOptions overrides;
    overrides.output_dir = utf8(out);
    overrides.isolation = mode;
    const auto workflow = runtime.open(config, overrides);
    const praenora::RunSummary summary = workflow->run();
    EXPECT_EQ(summary.final_state, WorkflowState::completed)
        << (summary.error ? summary.error->message : std::string());
}

// A module that uses provider services, its parameters, and the providers that serve it.
struct ProviderCase {
    const char* name; // the GoogleTest name of the case
    const char* module;
    const char* parameters;
    std::vector<std::string> providers;
};

std::ostream& operator<<(std::ostream& out, const ProviderCase& c) { return out << c.name; }

const ProviderCase kProviderCases[] = {
    {"SimulatingModule", "simulating-module", "{ kernel: true }", {"test-simulation"}},
    {"ConvertingModule",
     "converting-module",
     "{ simulate: { time_domain: { start: 0, end: 2 }, output: { save_at: explicit_times, "
     "times: [0, 1, 2], quantities: [total] } } }",
     {"test-conversion", "test-simulation"}},
};

class IsolationRunProviders : public testing::TestWithParam<ProviderCase> {};

// Runs `config` (with a step a, traced) in the runtime's process and then in module hosts, with
// the options `options_for` gives for each run's log file, and checks that the module hosts give
// the results of the runtime's process: the same output files and message trace (its service
// lookups, each provided, included), byte for byte, the same reproducibility section and
// fingerprint, and the same records of `providers` and of the module, in the same order, the
// provider.loaded event of each provider included. With `every_lookup_provided` false the trace
// may hold lookups that got no table. `sandbox` is the profile the configuration gives step a's
// module host (the override in-process sets it to none). Returns the log of the run in module
// hosts.
fs::path expect_the_results_of_the_runtimes_process(
    const fs::path& dir, const std::string& config,
    const std::function<RuntimeOptions(const fs::path&)>& options_for,
    const std::vector<std::string>& providers, bool every_lookup_provided = true,
    const std::string& sandbox = "none") {
    std::map<IsolationMode, fs::path> outs, logs;
    for (const IsolationMode mode : {IsolationMode::in_process, IsolationMode::process}) {
        const std::string name = mode == IsolationMode::process ? "process" : "in-process";
        SCOPED_TRACE(name);
        logs[mode] = dir / (name + ".log");
        outs[mode] = dir / name;
        run_completed(options_for(logs[mode]), config, mode, outs[mode]);
        EXPECT_EQ(started_module_hosts(logs[mode]).size(),
                  mode == IsolationMode::process ? 1u : 0u);
        expect_module_hosts_gone(logs[mode]);
    }
    const fs::path& in_process = outs.at(IsolationMode::in_process);
    const fs::path& process = outs.at(IsolationMode::process);
    const auto files = result_files(in_process);
    EXPECT_EQ(files, (std::vector<std::string>{"outputs/result.jsonl", "trace/messages.jsonl"}));
    EXPECT_EQ(result_files(process), files);
    for (const std::string& file : files) {
        EXPECT_EQ(read_text(process / file), read_text(in_process / file)) << file;
    }
    // Each service was looked up, and provided, in create.
    const std::string trace = read_text(process / "trace" / "messages.jsonl");
    EXPECT_TRUE(contains(trace, "\"phase\":\"create\",\"provided\":true")) << trace;
    if (every_lookup_provided) {
        EXPECT_FALSE(contains(trace, "\"provided\":false")) << trace;
    }
    const json first = run_record(in_process);
    const json second = run_record(process);
    EXPECT_EQ(second.at("fingerprint"), first.at("fingerprint"));
    EXPECT_EQ(second.at("reproducibility").dump(), first.at("reproducibility").dump());
    EXPECT_EQ(
        second.at("execution").at("isolation"),
        json({{"a",
               {{"mode", "process"}, {"share_compiled_models", false}, {"sandbox", sandbox}}}}));
    EXPECT_EQ(
        first.at("execution").at("isolation"),
        json({{"a",
               {{"mode", "in-process"}, {"share_compiled_models", false}, {"sandbox", "none"}}}}));

    // The providers' records and the module's, the same in both places, in the same order; and
    // the module host started each provider (the runtime's process may have before this test).
    const fs::path& log = logs.at(IsolationMode::process);
    EXPECT_EQ(provider_and_module_records(log),
              provider_and_module_records(logs.at(IsolationMode::in_process)));
    for (const std::string& provider : providers) {
        EXPECT_TRUE(has_event(log, "provider." + provider, "provider.loaded")) << provider;
    }
    return log;
}

} // namespace

// A step in a module host is served its module's services by providers started there, and gives
// the outputs, message trace (its service lookups included), reproducibility section and log
// records (its providers' and its module's) of the runtime's process. The run in the runtime's
// process comes first: it is where these providers start in this process (CTest runs each test in
// a process of its own), so it has their records of the start too.
TEST_P(IsolationRunProviders, GiveTheResultsOfTheRuntimesProcess) {
    const ProviderCase& c = GetParam();
    ScratchDir dir;
    const std::string config =
        write_config(dir.path(), "test.single", "run: { ticks: 2, seed: 7 }\n",
                     step("a", c.module, c.parameters), "outputs: { trace_messages: true }\n");
    const fs::path log = expect_the_results_of_the_runtimes_process(
        dir.path(), config, provider_options_logging_to, c.providers);
    for (const std::string& provider : c.providers) {
        SCOPED_TRACE(provider);
        bool started = false;
        for (const json& record : log_records(log)) {
            started = started || (logger_of(record) == "provider." + provider &&
                                  record.at("body") == provider + " started" &&
                                  record.at("attributes").at("provider") == provider + "@0.1.0");
        }
        EXPECT_TRUE(started) << "no record '" << provider << " started'";
    }
}

INSTANTIATE_TEST_SUITE_P(Fixtures, IsolationRunProviders, testing::ValuesIn(kProviderCases),
                         [](const testing::TestParamInfo<ProviderCase>& info) {
                             return std::string(info.param.name);
                         });

// Shared compiled models: steps in module hosts that share them hand the model one compiled to the
// next through the store in the runtime's process, and the results are those of steps that do not
// share, and of the runtime's process. The store belongs to the runtime's process (this test's), so
// each test gives its model a text no earlier test of this process used.
namespace {

// The words a provider logs at debug through the calling instance's host for a model it compiled,
// and for one it loaded from the shared store.
struct SharingWords {
    std::string compiled, loaded;
};

// The store's events (compiled_models.*) that the records of `file` hold for step `step`, by name.
std::vector<std::string> store_events(const fs::path& file, const std::string& step) {
    std::vector<std::string> events;
    for (const json& record : log_records(file)) {
        if (logger_of(record) == "module." + step &&
            event_of(record).rfind("compiled_models.", 0) == 0) {
            events.push_back(event_of(record));
        }
    }
    return events;
}

// Whether the module of `step` logged a message holding `words`.
bool logged(const fs::path& file, const std::string& step, const std::string& words) {
    const std::vector<std::string> messages = module_messages(file, step);
    return std::any_of(messages.begin(), messages.end(),
                       [&words](const std::string& message) { return contains(message, words); });
}

// Runs test.chain with steps a and b, both simulating-module with `parameters`, four times, each
// with `options_for` its log file: (1) both in module hosts that share compiled models, (2) in
// module hosts that do not, (3) in the runtime's process, (4) in module hosts of which only a's
// shares, with the store holding the model from (1). In (1) step a compiles the model and saves
// it, and b loads it from the store; in (2) both compile, and the store never hears of them; in (4)
// a loads it, and b, which does not share, compiles it and never reads the store. All four give
// the same outputs, byte for byte, the same reproducibility section and the same fingerprint; the
// run record lists each step's sharing. With `sandbox` "restricted", the module hosts of (1), (2)
// and (4) have that profile.
void expect_sharing_to_change_no_result(
    const fs::path& dir, const json& parameters, const SharingWords& words,
    const std::function<RuntimeOptions(const fs::path&)>& options_for,
    const std::string& sandbox = "none") {
    struct Run {
        const char* name;
        std::string run, a_extra;
    };
    const std::string profile = sandbox == "none" ? "" : ", sandbox: " + sandbox;
    const std::vector<Run> runs = {
        {"sharing",
         "run: { ticks: 2, seed: 7, isolation: { mode: process, "
         "share_compiled_models: true" +
             profile + " } }\n",
         ""},
        {"not sharing", "run: { ticks: 2, seed: 7, isolation: { mode: process" + profile + " } }\n",
         ""},
        {"in-process", "run: { ticks: 2, seed: 7 }\n", ""},
        {"a sharing", "run: { ticks: 2, seed: 7, isolation: { mode: process" + profile + " } }\n",
         "    isolation: { share_compiled_models: true }\n"},
    };
    std::vector<fs::path> outs, logs;
    for (std::size_t i = 0; i < runs.size(); ++i) {
        SCOPED_TRACE(runs[i].name);
        const fs::path run_dir = dir / std::to_string(i);
        const std::string config =
            write_config(run_dir, "test.chain", runs[i].run,
                         step("a", "simulating-module", parameters.dump(), runs[i].a_extra) +
                             step("b", "simulating-module", parameters.dump()));
        outs.push_back(run_dir / "out");
        logs.push_back(run_dir / "run.log");
        const Runtime runtime(options_for(logs.back()));
        WorkflowOptions overrides;
        overrides.output_dir = utf8(outs.back());
        const praenora::RunSummary summary = runtime.open(config, overrides)->run();
        ASSERT_EQ(summary.final_state, WorkflowState::completed)
            << (summary.error ? summary.error->message : std::string());
        expect_module_hosts_gone(logs.back());
    }
    // The results.
    const auto files = result_files(outs[0]);
    EXPECT_EQ(files, (std::vector<std::string>{"outputs/result.jsonl"}));
    const json first = run_record(outs[0]);
    for (std::size_t i = 1; i < runs.size(); ++i) {
        SCOPED_TRACE(runs[i].name);
        EXPECT_EQ(result_files(outs[i]), files);
        for (const std::string& file : files) {
            EXPECT_EQ(read_text(outs[i] / file), read_text(outs[0] / file)) << file;
        }
        const json record = run_record(outs[i]);
        EXPECT_EQ(record.at("fingerprint"), first.at("fingerprint"));
        EXPECT_EQ(record.at("reproducibility").dump(), first.at("reproducibility").dump());
    }
    const auto isolation = [&sandbox](const char* mode, bool a, bool b) {
        const std::string profile = std::string(mode) == "process" ? sandbox : "none";
        return json{{"a", {{"mode", mode}, {"share_compiled_models", a}, {"sandbox", profile}}},
                    {"b", {{"mode", mode}, {"share_compiled_models", b}, {"sandbox", profile}}}};
    };
    EXPECT_EQ(first.at("execution").at("isolation"), isolation("process", true, true));
    EXPECT_EQ(run_record(outs[1]).at("execution").at("isolation"),
              isolation("process", false, false));
    EXPECT_EQ(run_record(outs[2]).at("execution").at("isolation"),
              isolation("in-process", false, false));
    EXPECT_EQ(run_record(outs[3]).at("execution").at("isolation"),
              isolation("process", true, false));

    // Who compiled, and who loaded the model from the store.
    using Events = std::vector<std::string>;
    EXPECT_EQ(store_events(logs[0], "a"),
              (Events{"compiled_models.miss", "compiled_models.saved"}));
    EXPECT_EQ(store_events(logs[0], "b"), (Events{"compiled_models.hit"}));
    EXPECT_TRUE(logged(logs[0], "a", words.compiled));
    EXPECT_FALSE(logged(logs[0], "a", words.loaded));
    EXPECT_TRUE(logged(logs[0], "b", words.loaded));
    EXPECT_FALSE(logged(logs[0], "b", words.compiled));
    for (const char* name : {"a", "b"}) {
        SCOPED_TRACE(name);
        EXPECT_EQ(store_events(logs[1], name), Events{});
        EXPECT_TRUE(logged(logs[1], name, words.compiled));
        EXPECT_FALSE(logged(logs[1], name, words.loaded));
        EXPECT_EQ(store_events(logs[2], name), Events{});
    }
    EXPECT_EQ(store_events(logs[3], "a"), (Events{"compiled_models.hit"}));
    EXPECT_TRUE(logged(logs[3], "a", words.loaded));
    EXPECT_EQ(store_events(logs[3], "b"), Events{});
    EXPECT_TRUE(logged(logs[3], "b", words.compiled));
    EXPECT_FALSE(logged(logs[3], "b", words.loaded));
}

// A text of a test-decay model that no earlier test of this process gave: JSON allows any number
// of spaces after the object, and the provider keys a model by its text.
std::string fresh_test_decay_model() {
    static std::atomic<unsigned> next{0};
    return R"({"states": {"x": 8, "y": 3}, "parameters": {"k_x": 0.6931471805599453, "k_y": 0}})" +
           std::string(1 + next.fetch_add(1), ' ');
}

} // namespace

// test-simulation keeps its "compiled" form of a model (the canonical parsed model) in the store:
// two steps of a run that share compiled models compile it once, and the results do not change.
TEST(IsolationRun, StepsThatShareCompiledModelsGiveTheResultsOfStepsThatDoNot) {
    ScratchDir dir;
    const json parameters = {{"model", fresh_test_decay_model()}, {"kernel", true}};
    expect_sharing_to_change_no_result(dir.path(), parameters,
                                       {"test-simulation: compiled the model",
                                        "test-simulation: loaded the model from the shared store"},
                                       provider_options_logging_to);
}

// The restricted sandbox profile: a step whose module host applies it, before it loads
// test-simulation and the module, gives the results of the runtime's process; and steps under it
// that share compiled models give the results of steps that do not. The run record lists each
// step's profile. Skipped in a process that runs in a sandbox already (macOS applies no second
// one).
TEST(IsolationRun, ARestrictedStepGivesTheResultsOfTheRuntimesProcess) {
    if (const char* why = praenora::test::restricted_profile_skip()) {
        GTEST_SKIP() << why;
    }
    ScratchDir dir;
    const std::string config = write_config(
        dir.path(), "test.single",
        "run: { ticks: 2, seed: 7, isolation: { sandbox: restricted } }\n",
        step("a", "simulating-module", "{ kernel: true }"), "outputs: { trace_messages: true }\n");
    expect_the_results_of_the_runtimes_process(dir.path(), config, provider_options_logging_to,
                                               {"test-simulation"}, true, "restricted");
}

namespace {

// A configuration of test.single whose step a is fx-sandbox-probe in a module host under the
// restricted profile, writing outside its temporary directory in `dir`, reading a file there and
// connecting to `listener`.
std::string probe_config(const fs::path& dir, const praenora::test::LoopbackListener& listener) {
    const fs::path read = dir / "read.txt";
    write_text_file(read, "read me");
    const json parameters = {
        {"outside", utf8(dir)}, {"read", utf8(read)}, {"connect_port", listener.port()}};
    return write_config(dir, "test.single", "run: { ticks: 1 }\n",
                        step("a", "fx-sandbox-probe", parameters.dump(),
                             "    isolation: { mode: process, sandbox: restricted }\n"));
}

// The error of a restricted load in a runtime that runs in a sandbox already: macOS applies no
// second sandbox.
void expect_no_second_sandbox(ErrorCode code, const std::string& message,
                              const std::string& details_json) {
    EXPECT_EQ(code, ErrorCode::MODULE_HOST_UNAVAILABLE) << message;
    const json details = json::parse(details_json);
    EXPECT_EQ(details.at("reason"), "sandbox") << details;
    EXPECT_EQ(details.at("system"), "Operation not permitted") << details;
    EXPECT_TRUE(contains(message, "cannot apply the restricted sandbox profile")) << message;
}

} // namespace

// A run gives a step's module host the step's profile: fx-sandbox-probe, in a step under the
// restricted profile, is refused the network, new processes and writes outside its temporary
// directory (also through links, and to /dev/stderr opened by its path), and may write in it and
// read; its standard error is the runtime's, not /dev/null; the run record lists the profile. In a
// runtime that runs in a sandbox already, the step's load fails instead (MODULE_HOST_UNAVAILABLE,
// reason "sandbox", the system's text), and the run fails at it.
TEST(IsolationRun, ARestrictedStepRunsUnderItsProfile) {
    if (!praenora::test::restricted_profile_available()) {
        GTEST_SKIP() << praenora::test::kNoRestrictedProfileSkip;
    }
    ScratchDir dir;
    const praenora::test::LoopbackListener listener;
    const std::string config = probe_config(dir.path(), listener);
    const fs::path log = dir.path() / "run.log";
    const fs::path out = dir.path() / "out";
    const Runtime runtime(options_logging_to(log));
    WorkflowOptions overrides;
    overrides.output_dir = utf8(out);
    const auto workflow = runtime.open(config, overrides);
    if (praenora::test::in_a_sandbox()) {
        const Error error = error_of([&] { workflow->run(); });
        expect_no_second_sandbox(error.code(), error.what(), error.details_json());
        EXPECT_EQ(json::parse(error.details_json()).at("step"), "a");
        EXPECT_EQ(workflow->state(), WorkflowState::failed);
    } else {
        const praenora::RunSummary summary = workflow->run();
        ASSERT_EQ(summary.final_state, WorkflowState::completed)
            << (summary.error ? summary.error->message : std::string());
        const std::string result = read_text(out / "outputs" / "result.jsonl");
        const json data = json::parse(result.substr(0, result.find('\n'))).at("data");
        EXPECT_EQ(data.at("connect"), EPERM) << data;
        EXPECT_EQ(data.at("fork"), EPERM) << data;
        EXPECT_EQ(data.at("spawn"), EPERM) << data;
        EXPECT_EQ(data.at("write_outside"), EPERM) << data;
        EXPECT_EQ(data.at("write_temp"), 0) << data;
        EXPECT_EQ(data.at("read"), 0) << data;
        EXPECT_EQ(data.at("link_outside"), EPERM) << data;
        EXPECT_EQ(data.at("write_through_symlink"), EPERM) << data;
        EXPECT_EQ(data.at("stderr_is_null"), praenora::test::stderr_is_null()) << data;
        EXPECT_EQ(data.at("open_stderr"), EPERM) << data;
        EXPECT_EQ(read_text(dir.path() / "read.txt"), "read me");
        EXPECT_EQ(run_record(out).at("execution").at("isolation").at("a").at("sandbox"),
                  "restricted");
    }
    expect_module_hosts_gone(log);
}

// validate --load gives a step's module host its profile too: it loads under the restricted
// profile without a diagnostic, and in a runtime that runs in a sandbox already reports the
// load's MODULE_HOST_UNAVAILABLE for the step.
TEST(IsolationValidation, LoadGivesAModuleHostItsProfile) {
    if (!praenora::test::restricted_profile_available()) {
        GTEST_SKIP() << praenora::test::kNoRestrictedProfileSkip;
    }
    ScratchDir dir;
    const praenora::test::LoopbackListener listener;
    const std::string config = probe_config(dir.path(), listener);
    const fs::path log = dir.path() / "validate.log";
    const Runtime runtime(options_logging_to(log));
    ValidateOptions load;
    load.load = true;
    const ValidationReport report = runtime.validate(config, load);
    if (praenora::test::in_a_sandbox()) {
        ASSERT_EQ(report.diagnostics.size(), 1u) << report.to_text();
        const Diagnostic& diagnostic = report.diagnostics.front();
        expect_no_second_sandbox(diagnostic.code, diagnostic.message, diagnostic.details_json);
        EXPECT_EQ(diagnostic.step, "a");
    } else {
        EXPECT_TRUE(report.diagnostics.empty()) << report.to_text();
    }
    EXPECT_FALSE(started_module_hosts(log).empty());
    expect_module_hosts_gone(log);
}

// validate --load loads a module once for each set of isolation settings of the steps that run it
// in a module host (the sandbox profile and whether it shares compiled models), not only with the
// first such step's, so that a load that fails under one step's settings only is reported, for
// that step. fx-sandbox-probe's static initializer here ends its process (exit code 70) when it
// cannot write outside its temporary directory, which the restricted profile refuses: step a,
// without a profile, loads; step b, under it, does not, and is reported (MODULE_CRASHED in phase
// load). In a runtime that runs in a sandbox already, step b's load fails at the profile instead
// (MODULE_HOST_UNAVAILABLE, reason "sandbox"); either way only step b is reported.
TEST(IsolationValidation, LoadChecksTheIsolationSettingsOfEveryStep) {
    if (!praenora::test::restricted_profile_available()) {
        GTEST_SKIP() << praenora::test::kNoRestrictedProfileSkip;
    }
    ScratchDir dir;
    const fs::path outside = dir.path() / "outside";
    fs::create_directories(outside);
    const praenora::test::ScopedEnvironment at_load("PRAENORA_TEST_SANDBOX_PROBE_AT_LOAD",
                                                    utf8(outside));
    const praenora::test::ScopedEnvironment exit_at_load("PRAENORA_TEST_SANDBOX_PROBE_EXIT_AT_LOAD",
                                                         std::string("1"));
    const json parameters = {
        {"outside", utf8(outside)}, {"read", utf8(outside / "read.txt")}, {"connect_port", 1}};
    const std::string config =
        write_config(dir.path(), "test.chain", "run: { ticks: 1, isolation: { mode: process } }\n",
                     step("a", "fx-sandbox-probe", parameters.dump()) +
                         step("b", "fx-sandbox-probe", parameters.dump(),
                              "    isolation: { sandbox: restricted }\n"));
    const fs::path log = dir.path() / "validate.log";
    const Runtime runtime(options_logging_to(log));
    ValidateOptions load;
    load.load = true;
    const ValidationReport report = runtime.validate(config, load);
    ASSERT_EQ(report.diagnostics.size(), 1u) << report.to_text();
    const Diagnostic& diagnostic = report.diagnostics.front();
    EXPECT_EQ(diagnostic.step, "b");
    if (praenora::test::in_a_sandbox()) {
        expect_no_second_sandbox(diagnostic.code, diagnostic.message, diagnostic.details_json);
    } else {
        EXPECT_EQ(diagnostic.code, ErrorCode::MODULE_CRASHED) << diagnostic.message;
        const json details = json::parse(diagnostic.details_json);
        EXPECT_EQ(details.at("phase"), "load") << details;
        EXPECT_EQ(details.at("exit_code"), 70) << details;
    }
    const auto started = started_module_hosts(log);
    ASSERT_EQ(started.size(), 2u);
    EXPECT_EQ(started[0].first, "module.a");
    EXPECT_EQ(started[1].first, "module.b");
    expect_module_hosts_gone(log);
}

// Validation takes every parameter fx-sandbox-probe reads, write_stderr and keep included.
TEST(IsolationValidation, TheProbesManifestDeclaresEveryParameterItReads) {
    ScratchDir dir;
    const json parameters = {{"outside", utf8(dir.path())},
                             {"read", utf8(dir.path() / "read.txt")},
                             {"connect_port", 1},
                             {"keep", false},
                             {"write_stderr", true}};
    const std::string config = write_config(dir.path(), "test.single", "run: { ticks: 1 }\n",
                                            step("a", "fx-sandbox-probe", parameters.dump()));
    const Runtime runtime(options_logging_to(dir.path() / "validate.log"));
    const ValidationReport report = runtime.validate(config, {});
    EXPECT_TRUE(report.diagnostics.empty()) << report.to_text();
}

TEST(IsolationRun, RestrictedStepsThatShareCompiledModelsGiveTheResultsOfStepsThatDoNot) {
    if (const char* why = praenora::test::restricted_profile_skip()) {
        GTEST_SKIP() << why;
    }
    ScratchDir dir;
    const json parameters = {{"model", fresh_test_decay_model()}, {"kernel", true}};
    expect_sharing_to_change_no_result(dir.path(), parameters,
                                       {"test-simulation: compiled the model",
                                        "test-simulation: loaded the model from the shared store"},
                                       provider_options_logging_to, "restricted");
}

// A provider library that changes after validation is refused when a step's module host loads it,
// as in the runtime's process: LIBRARY_DIGEST_MISMATCH located at /library/path of the provider's
// manifest, with the same message, before any of its code runs. In a module host the error belongs
// to the step's load.
TEST(IsolationRun, AProviderLibraryThatChangedAfterValidationIsRefused) {
    ScratchDir dir;
    const fs::path providers = dir.path() / "providers";
    const std::string config = write_config(dir.path(), "test.single", "run: { ticks: 1 }\n",
                                            step("a", "simulating-module"));
    std::map<IsolationMode, Error> errors;
    fs::path manifest;
    for (const IsolationMode mode : {IsolationMode::in_process, IsolationMode::process}) {
        const std::string name = mode == IsolationMode::process ? "process" : "in-process";
        SCOPED_TRACE(name);
        // A fresh copy each time: the library as validation will see it.
        manifest = praenora::test::copy_fixture_provider("test-simulation", providers);
        const fs::path log = dir.path() / (name + ".log");
        RuntimeOptions options = options_logging_to(log);
        options.provider_paths = {utf8(providers)};
        const Runtime runtime(options);
        WorkflowOptions overrides;
        overrides.isolation = mode;
        const auto workflow = runtime.open(config, overrides);
        const json document = json::parse(read_text(manifest));
        praenora::test::append_to_file(
            providers /
                praenora::test::utf8_path(document.at("library").at("path").get<std::string>()),
            "changed after validation");
        errors.emplace(mode, error_of([&] { workflow->run(); }));
        expect_module_hosts_gone(log);
    }
    const Error& here = errors.at(IsolationMode::in_process);
    const Error& there = errors.at(IsolationMode::process);
    EXPECT_EQ(here.code(), ErrorCode::LIBRARY_DIGEST_MISMATCH) << here.what();
    EXPECT_EQ(there.code(), here.code()) << there.what();
    EXPECT_EQ(std::string(there.what()), std::string(here.what()));
    EXPECT_TRUE(contains(here.what(), "changed after validation")) << here.what();
    EXPECT_EQ(here.location().file, utf8(manifest));
    EXPECT_EQ(there.location().file, here.location().file);
    EXPECT_EQ(here.location().pointer, "/library/path");
    EXPECT_EQ(there.location().pointer, here.location().pointer);
    // In a module host the step's load failed; in the runtime's process the run's providers.
    const json details = json::parse(there.details_json());
    EXPECT_EQ(details.at("step"), "a");
    EXPECT_EQ(details.at("phase"), "load");
    EXPECT_FALSE(json::parse(here.details_json()).contains("step"));
}

// A provider whose start() fails reports, from a module host, the error of the runtime's process:
// LIBRARY_LOAD_FAILED with reason "start", the provider's error document as its cause, located in
// the provider's manifest; in validate --load and in a run. A module host is a process of its
// own, where the provider starts for the first time each time; in this process start() is called
// once, and a later load (in this test or after an earlier one) reports that first failure again,
// in words that say so around the same message.
TEST(IsolationRun, AProviderThatFailsToStartFailsAsInTheRuntimesProcess) {
    ScratchDir dir;
    const fs::path log = dir.path() / "run.log";
    RuntimeOptions options = options_logging_to(log);
    options.provider_paths = {PRAENORA_TEST_PROVIDERS_DIR "/test-start-fails"};
    const Runtime runtime(options);
    const auto config_in = [&](const char* mode) {
        return write_config(dir.path() / mode, "test.single",
                            std::string("run: { ticks: 1, isolation: { mode: ") + mode + " } }\n",
                            step("a", "fx-service"));
    };
    ValidateOptions load;
    load.load = true;
    const ValidationReport process_report = runtime.validate(config_in("process"), load);
    const Error process_run = error_of([&] { runtime.open(config_in("process"), {})->run(); });
    const ValidationReport in_process_report = runtime.validate(config_in("in-process"), load);
    expect_module_hosts_gone(log);

    ASSERT_EQ(in_process_report.diagnostics.size(), 1u) << in_process_report.to_text();
    ASSERT_EQ(process_report.diagnostics.size(), 1u) << process_report.to_text();
    Diagnostic expected = in_process_report.diagnostics.front();
    const Diagnostic& actual = process_report.diagnostics.front();
    // The first failure, as this process's first load reported it.
    const std::string again = "start() failed when the provider was first loaded in this process, "
                              "and it is not called again: ";
    if (const auto at = expected.message.find(again); at != std::string::npos) {
        expected.message.erase(at, again.size());
    }
    EXPECT_EQ(expected.code, ErrorCode::LIBRARY_LOAD_FAILED);
    EXPECT_TRUE(contains(expected.message, "the engine cannot start")) << expected.message;
    EXPECT_TRUE(contains(expected.location.file, "test-start-fails.provider.json"))
        << expected.location.file;
    EXPECT_EQ(json::parse(expected.details_json).at("reason"), "start");
    EXPECT_EQ(actual.code, expected.code);
    EXPECT_EQ(actual.message, expected.message);
    EXPECT_EQ(actual.location.file, expected.location.file);
    EXPECT_EQ(actual.location.pointer, expected.location.pointer);
    EXPECT_EQ(actual.step, "a");
    EXPECT_EQ(actual.module, expected.module);
    EXPECT_EQ(json::parse(actual.details_json), json::parse(expected.details_json));

    // A run fails at the step's load, with the same error.
    EXPECT_EQ(process_run.code(), expected.code) << process_run.what();
    EXPECT_EQ(std::string(process_run.what()), expected.message);
    EXPECT_EQ(process_run.location().file, expected.location.file);
    const json details = json::parse(process_run.details_json());
    EXPECT_EQ(details.at("step"), "a");
    EXPECT_EQ(details.at("phase"), "load");
    EXPECT_EQ(details.at("cause"), json::parse(expected.details_json).at("cause"));
}

// Lookups that get no table while providers are loaded in the module host are recorded as in the
// runtime's process, beside those a provider serves: fx-service looks up test.absent@0, which its
// manifest does not declare, test.other@1, which it declares and which no provider serves here,
// and test.echo@0, which test-echo serves, in create and in invoke.
TEST(IsolationRun, LookupsWithoutATableAreTracedAsInTheRuntimesProcess) {
    ScratchDir dir;
    const std::string config =
        write_config(dir.path(), "test.single", "run: { ticks: 2 }\n",
                     step("a", "fx-service",
                          "{ lookups: [test.absent@0, test.other@1, test.echo@0], "
                          "lookup_in: [create, invoke] }"),
                     "outputs: { trace_messages: true }\n");
    const auto options_for = [](const fs::path& log) {
        RuntimeOptions options = options_logging_to(log);
        options.provider_paths = {PRAENORA_TEST_PROVIDERS_DIR "/test-echo"};
        return options;
    };
    expect_the_results_of_the_runtimes_process(dir.path(), config, options_for, {"test-echo"},
                                               false);
    const std::string trace = read_text(dir.path() / "process" / "trace" / "messages.jsonl");
    for (const char* phase : {"create", "invoke"}) {
        for (const std::string& service : {"test.absent", "test.other"}) {
            EXPECT_TRUE(contains(trace, std::string("\"phase\":\"") + phase +
                                            "\",\"provided\":false,\"service\":\"" + service +
                                            "\""))
                << phase << " " << service << "\n"
                << trace;
        }
        EXPECT_TRUE(contains(trace, std::string("\"phase\":\"") + phase +
                                        "\",\"provided\":true,\"service\":\"test.echo\""))
            << phase << "\n"
            << trace;
    }
    const std::string result = read_text(dir.path() / "process" / "outputs" / "result.jsonl");
    EXPECT_EQ(json::parse(result.substr(0, result.find('\n'))).at("data").at("found"),
              json({{"test.absent@0", false}, {"test.other@1", false}, {"test.echo@0", true}}))
        << result;
}

// A provider that ends its module host during a service call is a crash of that call: the run
// fails with MODULE_CRASHED (details.phase "invoke", the signal), the step is `crashed`, and the
// runtime ends the run. test-echo-crashes's echo() aborts; fx-service echoes its input in invoke.
TEST(IsolationRun, AProviderThatCrashesInAServiceCallIsACrashOfThatCall) {
    const OwnSignals own_signals;
    ScratchDir dir;
    const fs::path log = dir.path() / "run.log";
    RuntimeOptions options = options_logging_to(log);
    options.provider_paths = {PRAENORA_TEST_PROVIDERS_DIR "/test-echo-crashes"};
    const Runtime runtime(options);
    const std::string config =
        write_config(dir.path(), "test.single", "run: { ticks: 1, isolation: { mode: process } }\n",
                     step("a", "fx-service"));
    const auto workflow = runtime.open(config, {});
    const Error error = error_of([&] { workflow->run(); });
    EXPECT_EQ(error.code(), ErrorCode::MODULE_CRASHED) << error.what();
    const json details = json::parse(error.details_json());
    EXPECT_EQ(details.at("phase"), "invoke");
    EXPECT_EQ(details.at("step"), "a");
    EXPECT_EQ(details.at("tick"), 0);
    EXPECT_TRUE(details.contains("signal_name") || details.contains("exit_code")) << details;
    EXPECT_EQ(step_status(workflow->status(), "a").state, StepState::crashed);
    EXPECT_TRUE(has_event(log, "module.a", "module_host.crashed"));
    expect_module_hosts_gone(log);
}

// A provider whose start() ends its process ends the module host: the step's load fails with
// MODULE_CRASHED (details.phase "load"), the step is `crashed`, and the runtime goes on to end
// the run. (In the runtime's process it would end the runtime.)
TEST(IsolationRun, AProviderThatCrashesAtStartIsACrashOfTheModuleHost) {
    const OwnSignals own_signals;
    ScratchDir dir;
    const fs::path log = dir.path() / "run.log";
    RuntimeOptions options = options_logging_to(log);
    options.provider_paths = {PRAENORA_TEST_PROVIDERS_DIR "/test-start-crashes"};
    const Runtime runtime(options);
    const std::string config =
        write_config(dir.path(), "test.single", "run: { ticks: 1, isolation: { mode: process } }\n",
                     step("a", "fx-service"));
    const auto workflow = runtime.open(config, {});
    const Error error = error_of([&] { workflow->run(); });
    EXPECT_EQ(error.code(), ErrorCode::MODULE_CRASHED) << error.what();
    const json details = json::parse(error.details_json());
    EXPECT_EQ(details.at("phase"), "load");
    EXPECT_EQ(details.at("step"), "a");
    EXPECT_TRUE(details.contains("signal_name") || details.contains("exit_code")) << details;
    EXPECT_EQ(step_status(workflow->status(), "a").state, StepState::crashed);
    EXPECT_TRUE(has_event(log, "module.a", "module_host.crashed"));
    expect_module_hosts_gone(log);
}

// The engines' providers, roadrunner (libRoadRunner) and antimony (libAntimony), in module hosts,
// found where the build tree's runtime looks for the providers of its installation.
#if defined(PRAENORA_TEST_WITH_ROADRUNNER) || defined(PRAENORA_TEST_WITH_ANTIMONY)
namespace {

// A step block binding step a to `module` with `parameters` (JSON, which YAML reads as it is).
std::string json_step(const std::string& module, const json& parameters) {
    return step("a", module, parameters.dump());
}

#ifdef PRAENORA_TEST_WITH_ROADRUNNER
// A number this process has not handed out before. roadrunner keeps the models it compiled in its
// process, and says whether it compiled one or found it compiled: a model whose text holds this
// number is compiled anew in the runtime's process as in a module host, whatever an earlier test
// (or an earlier repetition of the test) of this process compiled.
std::string fresh_number() {
    static std::atomic<unsigned> next{0};
    return std::to_string(::getpid()) + "_" + std::to_string(next.fetch_add(1));
}
#endif

// CVODE with tight tolerances, as libRoadRunner drives it.
const json kCvode = {{"method", "cvode"},
                     {"stiffness", "nonstiff"},
                     {"relative_tolerance", 1e-12},
                     {"absolute_tolerance", 1e-12}};

} // namespace
#endif

#ifdef PRAENORA_TEST_WITH_ROADRUNNER
namespace {

// simulating-module's parameters for the SBML model decay.xml, with a comment holding `mark` that
// no earlier run of this process compiled, simulated with CVODE through a plan and a kernel.
json decay_parameters(const std::string& mark) {
    std::string model = read_text(praenora::test::fixture_path("models/sbml/decay.xml"));
    model.insert(model.find('\n') + 1, "<!-- " + mark + " " + fresh_number() + " -->\n");
    return {{"model", model},
            {"format", "sbml"},
            {"plan",
             {{"parameters", {"k_x"}},
              {"initial_conditions", {"x"}},
              {"time_domain", {{"start", 0}, {"end", 2}}},
              {"output", {{"save_at", "explicit_times"}, {"times", {0, 0.5, 1, 2}}}},
              {"solver", kCvode}}},
            {"values", {0.5, 10}},
            {"kernel", true}};
}

// Runs decay.xml in step a of test.single, in the runtime's process and in a module host with the
// profile `sandbox`, and checks that the module host gives the results of the runtime's process,
// and that they are right: x(t) = 10 exp(-0.5 t), through CVODE, in the row at t = 2 (columns
// time, x, y).
void expect_roadrunner_results(const std::string& sandbox) {
    ScratchDir dir;
    const std::string run =
        sandbox == "none" ? "run: { ticks: 2, seed: 7 }\n"
                          : "run: { ticks: 2, seed: 7, isolation: { sandbox: " + sandbox + " } }\n";
    const std::string config =
        write_config(dir.path(), "test.single", run,
                     json_step("simulating-module", decay_parameters("run " + sandbox)),
                     "outputs: { trace_messages: true }\n");
    expect_the_results_of_the_runtimes_process(dir.path(), config, options_logging_to,
                                               {"roadrunner"}, true, sandbox);
    const std::string result = read_text(dir.path() / "process" / "outputs" / "result.jsonl");
    const json data = json::parse(result.substr(0, result.find('\n'))).at("data");
    ASSERT_EQ(data.at("ok"), true) << data;
    EXPECT_NEAR(data.at("rows").at(3).at(1).get<double>(), 10.0 * std::exp(-1.0), 1e-8) << data;
}

} // namespace

// roadrunner in a module host gives the results of the runtime's process: an SBML model compiled
// by LLVM in the module host, simulated with CVODE through a plan and a kernel.
TEST(IsolationRun, RoadrunnerGivesTheResultsOfTheRuntimesProcess) {
    expect_roadrunner_results("none");
}

// Under the restricted sandbox profile too: LLVM's code generation (MCJit: memory written, then
// made executable) works, libRoadRunner and its provider load after the profile is applied, and
// nothing needs a write outside the module host's temporary directory. Skipped in a process that
// runs in a sandbox already.
TEST(IsolationRun, RoadrunnerGivesTheResultsOfTheRuntimesProcessUnderTheRestrictedProfile) {
    if (const char* why = praenora::test::restricted_profile_skip()) {
        GTEST_SKIP() << why;
    }
    expect_roadrunner_results("restricted");
}

// roadrunner keeps a model's saved state in the store: two steps that share compiled models
// compile an SBML model once, the second loads the first's state without compiling, and the
// outputs are those of steps that do not share and of the runtime's process, byte for byte.
TEST(IsolationRun, RoadrunnerStepsThatShareCompiledModelsGiveTheResultsOfStepsThatDoNot) {
    ScratchDir dir;
    expect_sharing_to_change_no_result(
        dir.path(), decay_parameters("shared"),
        {"roadrunner: compiled the model",
         "roadrunner: loaded the compiled model from the shared store"},
        options_logging_to);
}

// Under the restricted sandbox profile, a saved state loads (loadStateS() makes its code in the
// loading module host) as in a module host without it. Skipped in a process that runs in a
// sandbox already.
TEST(IsolationRun, RestrictedRoadrunnerStepsThatShareCompiledModelsGiveTheResultsOfStepsThatDoNot) {
    if (const char* why = praenora::test::restricted_profile_skip()) {
        GTEST_SKIP() << why;
    }
    ScratchDir dir;
    expect_sharing_to_change_no_result(
        dir.path(), decay_parameters("shared restricted"),
        {"roadrunner: compiled the model",
         "roadrunner: loaded the compiled model from the shared store"},
        options_logging_to, "restricted");
}
#endif

#ifdef PRAENORA_TEST_WITH_ANTIMONY
namespace {

// A chain of `reactions` reactions, S0 -> S1 -> ... with mass-action rates, whose translation to
// SBML takes libAntimony long, longer than linearly in the reactions: on the macOS arm64 machine
// where this was measured, about 0.8 s for 1000 reactions and about 19 s for 6000, in the
// runtime's process and in a module host alike.
std::string chain_of_reactions(int reactions) {
    std::string text = "model *chain()\n";
    for (int i = 0; i < reactions; ++i) {
        const std::string n = std::to_string(i);
        text += "  J" + n + ": S" + n + " => S" + std::to_string(i + 1) + "; k" + n + " * S" + n +
                ";\n";
    }
    for (int i = 0; i <= reactions; ++i) {
        text += "  S" + std::to_string(i) + " = " + std::to_string(i % 7 + 1) + ";\n";
    }
    for (int i = 0; i < reactions; ++i) {
        text += "  k" + std::to_string(i) + " = 0." + std::to_string(i % 9 + 1) + ";\n";
    }
    return text + "end\n";
}

} // namespace

// antimony in a module host gives the results of the runtime's process: an Antimony model
// translated to SBML, the same bytes.
TEST(IsolationRun, AntimonyGivesTheResultsOfTheRuntimesProcess) {
    ScratchDir dir;
    const json parameters = {
        {"model", read_text(praenora::test::fixture_path("models/antimony/decay.ant"))},
        {"from", "antimony"},
        {"to", "sbml"}};
    const std::string config = write_config(dir.path(), "test.single", "run: { ticks: 1 }\n",
                                            json_step("converting-module", parameters),
                                            "outputs: { trace_messages: true }\n");
    expect_the_results_of_the_runtimes_process(dir.path(), config, options_logging_to,
                                               {"antimony"});
    const std::string result = read_text(dir.path() / "process" / "outputs" / "result.jsonl");
    const json data = json::parse(result.substr(0, result.find('\n'))).at("data");
    EXPECT_EQ(data.at("report").at("module"), "decay") << data;
    EXPECT_GT(data.at("bytes").get<int>(), 0) << data;
}

// A translation that takes long, in a module host, is ended for good when its deadline and grace
// have passed: the module host is killed (MODULE_UNRESPONSIVE, details.terminated) while
// libAntimony still translates, and nothing of it is left. A conversion in the module host of the
// next step that converts then succeeds at once: its provider is another process's, whose lock no
// abandoned translation holds (in the runtime's process it would wait for the translation to end).
// The translation runs in the first invoke (converting-module's convert_in), so that only the
// invoke deadline is short: create and initialize, which a slow machine or a sanitizer makes take
// longer, keep the default lifecycle timeout.
TEST(IsolationRun, ALongAntimonyTranslationIsKilledAndTheNextConvertsAtOnce) {
    ScratchDir dir;
    const fs::path log = dir.path() / "run.log";
    const Runtime runtime(options_logging_to(log));
    const json held = {{"model", chain_of_reactions(6000)},
                       {"from", "antimony"},
                       {"to", "sbml"},
                       {"convert_in", "invoke"}};
    const std::string long_config =
        write_config(dir.path() / "long", "test.single",
                     "run:\n  ticks: 1\n  isolation: { mode: process }\n"
                     "  timeouts: { invoke: 100ms, cancel_grace: 100ms }\n",
                     json_step("converting-module", held));
    const auto started = Clock::now();
    const Error error = error_of([&] { runtime.open(long_config, {})->run(); });
    const auto killed_after = Clock::now() - started;
    EXPECT_EQ(error.code(), ErrorCode::MODULE_UNRESPONSIVE) << error.what();
    const json details = json::parse(error.details_json());
    EXPECT_EQ(details.at("phase"), "invoke") << details;
    EXPECT_EQ(details.value("terminated", false), true) << details;
    EXPECT_TRUE(has_event(log, "module.a", "module_host.terminated"));
    expect_module_hosts_gone(log);
    // The bound: the deadline and the grace (200 ms), plus the start of the module host and of the
    // providers there (roadrunner too, in a build that has it, since converting-module can use the
    // simulation service: about 5 s for both runs of this test in such a build), with room for a
    // loaded machine. It must stay well below the translation's own time (about 19 s where it was
    // measured), so that it tells a kill from a finished translation; on a machine that translates
    // the chain in much less, lengthen the chain.
    EXPECT_LT(killed_after, std::chrono::seconds(10));

    const json small = {
        {"model", read_text(praenora::test::fixture_path("models/antimony/decay.ant"))},
        {"from", "antimony"},
        {"to", "sbml"}};
    const std::string next_config = write_config(
        dir.path() / "next", "test.single",
        "run:\n  ticks: 1\n  isolation: { mode: process }\n  timeouts: { lifecycle: 10s }\n",
        json_step("converting-module", small));
    const auto next = Clock::now();
    EXPECT_EQ(runtime.open(next_config, {})->run().final_state, WorkflowState::completed);
    // The same bound: the start of a module host and its providers and a small conversion, well
    // below the translation (about 19 s) that a conversion in the runtime's process would wait for.
    EXPECT_LT(Clock::now() - next, std::chrono::seconds(10));
    expect_module_hosts_gone(log);
}
#endif

#if defined(PRAENORA_TEST_WITH_ANTIMONY) && defined(PRAENORA_TEST_WITH_ROADRUNNER)
// Both engines in one module host, each with a libSBML of its own, as in the runtime's process:
// an Antimony model translated to SBML by antimony and simulated by roadrunner.
TEST(IsolationRun, AntimonyAndRoadrunnerGiveTheResultsOfTheRuntimesProcess) {
    ScratchDir dir;
    // The model, with a parameter that makes its SBML one that no earlier run of this process
    // compiled (libAntimony leaves comments out of the SBML).
    std::string model = read_text(praenora::test::fixture_path("models/antimony/decay.ant"));
    model.insert(model.rfind("end"), "  run_" + fresh_number() + " = 0;\n");
    const json parameters = {
        {"model", model},
        {"from", "antimony"},
        {"to", "sbml"},
        {"simulate",
         {{"time_domain", {{"start", 0}, {"end", 2}}},
          {"output", {{"save_at", "explicit_times"}, {"times", {0, 0.5, 1, 2}}}},
          {"solver", kCvode}}}};
    const std::string config = write_config(dir.path(), "test.single", "run: { ticks: 1 }\n",
                                            json_step("converting-module", parameters),
                                            "outputs: { trace_messages: true }\n");
    expect_the_results_of_the_runtimes_process(dir.path(), config, options_logging_to,
                                               {"antimony", "roadrunner"});
    const std::string result = read_text(dir.path() / "process" / "outputs" / "result.jsonl");
    const json data = json::parse(result.substr(0, result.find('\n'))).at("data");
    ASSERT_EQ(data.at("result").at("ok"), true) << data;
}
#endif

#else

// On Windows, validation refuses a step in a module host.
TEST(IsolationValidation, IsUnsupportedOnWindows) {
    ScratchDir dir;
    write_text_file(dir.path() / "source.json", "{}");
    write_text_file(dir.path() / "workflow.yaml",
                    "kind: workflow-config\nformat_version: \"0.1.0\"\n"
                    "workflow: { name: test.single, version: \"0.1.0\" }\n"
                    "run: { ticks: 1, isolation: { mode: process } }\n"
                    "inputs:\n  source: { source: json-document, path: source.json }\n"
                    "steps:\n  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n");
    RuntimeOptions options;
    options.module_paths = {PRAENORA_TEST_MODULES_DIR};
    options.catalog_paths = {PRAENORA_TEST_CATALOG_DIR};
    options.use_environment = false;
    options.logging.to_stderr = false;
    const auto file = (dir.path() / "workflow.yaml").u8string();
    const ValidationReport report =
        Runtime(options).validate(std::string(file.begin(), file.end()));
    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.diagnostics.front().code, ErrorCode::UNSUPPORTED);
    EXPECT_EQ(report.diagnostics.front().location.pointer, "/run/isolation/mode");
}

#endif
