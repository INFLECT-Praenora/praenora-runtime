// Python modules (docs/specs/python-service.md, Python modules), through the public API only (the
// shared library), with the Python fixture modules (tests/fixtures/modules/python), the test
// catalog and the build tree's provider python, which the runtime finds without a provider path:
// runs in the runtime's process and in module hosts, parameters, failures with their tracebacks,
// cancellation, print() as log records, numpy, two modules in one interpreter, a package clash,
// edited sources, the fingerprint, two instances called at once, a large result, a C++ module
// that calls the service itself, the hash seed that the tool sets, deep recursion on the module
// threads' stacks, and what Python keeps per thread from one call to the next; a crash and the
// restricted sandbox profile in module hosts; and child processes of a spawn pool. And the fixture
// provider test-python-echo, whose table of test.echo@0 is implemented by a Python class, serving
// the fixture module fx-service: in a run, in a module host, with its sources edited after the
// build, and beside the provider python in one interpreter; and, when the roadrunner provider is
// built too, a roadrunner step beside a Python step that simulates with the environment's
// roadrunner package, in one process. Some tests run the praenora tool as a child process
// (PRAENORA_TEST_TOOL). Built with PRAENORA_WITH_PYTHON_ENVIRONMENT only.
#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/runtime.hpp>
#include <praenora/runtime/status.hpp>
#include <praenora/runtime/workflow.hpp>

#include "module_fixtures.hpp"
#include "sandbox_support.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cwchar>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Error;
using praenora::ErrorCode;
using praenora::IsolationMode;
using praenora::LogFormat;
using praenora::LogLevel;
using praenora::Runtime;
using praenora::RuntimeOptions;
using praenora::WorkflowOptions;
using praenora::WorkflowState;
using praenora::test::ScratchDir;
using praenora::test::utf8_path;
using praenora::test::write_text_file;
using Clock = std::chrono::steady_clock;

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

fs::path python_modules() { return utf8_path(PRAENORA_TEST_PYTHON_MODULES_DIR); }
fs::path python_configs() { return praenora::test::fixture_path("configs/python"); }

// The Python fixture modules, the fixture modules and the test catalog, without the environment;
// the runtime and its runs log at level debug, as JSON lines, to `log_file` only.
RuntimeOptions options_logging_to(const fs::path& log_file,
                                  std::vector<std::string> module_paths = {}) {
    RuntimeOptions options;
    options.module_paths =
        module_paths.empty()
            ? std::vector<std::string>{PRAENORA_TEST_PYTHON_MODULES_DIR, PRAENORA_TEST_MODULES_DIR}
            : std::move(module_paths);
    options.catalog_paths = {PRAENORA_TEST_CATALOG_DIR};
    options.use_environment = false;
    options.logging.to_stderr = false;
    options.logging.level = LogLevel::debug;
    options.logging.format = LogFormat::json;
    options.logging.file = utf8(log_file);
    return options;
}

// A step block binding `id` to module `module` with `parameters`, and `extra` lines.
std::string step(const std::string& id, const std::string& module,
                 const std::string& parameters = "{}", const std::string& extra = "") {
    return "  " + id + ":\n    module: { name: " + module +
           ", version: \"0.1.0\" }\n    parameters: " + parameters + "\n" + extra;
}

// The source document of the configurations below: a value and a list of numbers, and text that
// is not ASCII.
const char* const kSource =
    "{\"value\": 42, \"values\": [1, 2, 3.5], \"text\": \"d\xC3\xA9j\xC3\xA0\"}";

// Writes `<dir>/source.json` and `<dir>/workflow.yaml`, a configuration of the test workflow
// `workflow` with `run` and `steps` (YAML blocks), and returns the configuration's path.
std::string write_config(const fs::path& dir, const std::string& workflow, const std::string& run,
                         const std::string& steps, const std::string& extra = "") {
    write_text_file(dir / "source.json", kSource);
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

// The log records of `file` (JSON lines), but a last line that the runtime is still writing.
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
    return record.at("attributes").value("praenora.event", std::string());
}

std::string logger_of(const json& record) {
    return record.at("attributes").value("logger", std::string());
}

// The records that the module of step `step` wrote (logger module.<step>, no event), in order, as
// "<severity> <body>".
std::vector<std::string> module_messages(const fs::path& file, const std::string& step) {
    std::vector<std::string> messages;
    for (const json& record : log_records(file)) {
        if (logger_of(record) == "module." + step && event_of(record).empty()) {
            messages.push_back(record.at("severity_text").get<std::string>() + " " +
                               record.at("body").get<std::string>());
        }
    }
    return messages;
}

// The number of module hosts that the records of `file` say were started.
std::size_t started_module_hosts(const fs::path& file) {
    const auto records = log_records(file);
    return static_cast<std::size_t>(
        std::count_if(records.begin(), records.end(),
                      [](const json& r) { return event_of(r) == "module_host.started"; }));
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

json run_record(const fs::path& out) { return json::parse(read_text(out / "run-record.json")); }

// The data of each envelope of the output `result` of the run in `out`, in order.
std::vector<json> results(const fs::path& out) {
    std::vector<json> data;
    std::istringstream lines(read_text(out / "outputs" / "result.jsonl"));
    for (std::string line; std::getline(lines, line);) {
        data.push_back(json::parse(line).at("data"));
    }
    return data;
}

// Runs the configuration `config` with its output directory `out` and, when given, every step in
// isolation mode `mode` (otherwise the configuration's); true when the run completed.
bool run_to_completion(const Runtime& runtime, const std::string& config, const fs::path& out,
                       std::optional<IsolationMode> mode = std::nullopt) {
    WorkflowOptions overrides;
    overrides.output_dir = utf8(out);
    overrides.isolation = mode;
    const auto workflow = runtime.open(config, overrides);
    const praenora::RunSummary summary = workflow->run();
    EXPECT_EQ(summary.final_state, WorkflowState::completed)
        << (summary.error ? summary.error->message : std::string());
    return summary.final_state == WorkflowState::completed;
}

// The error of a run of `config` that fails, every step in isolation mode `mode` when given.
Error failure_of(const Runtime& runtime, const std::string& config,
                 std::optional<IsolationMode> mode = std::nullopt) {
    WorkflowOptions overrides;
    overrides.isolation = mode;
    const auto workflow = runtime.open(config, overrides);
    return error_of([&] { workflow->run(); });
}

// Whether the environment keeps every module library loaded (the sanitizer test runs on macOS
// do), so that a library loaded again at its path is the first one.
bool keep_libraries_requested() {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv is fine here: nothing changes the environment
#endif
    const char* value = std::getenv("PRAENORA_DEBUG_KEEP_LIBRARIES");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return value != nullptr && std::string(value) == "1";
}

// The module's error document of a run's error (details.cause).
json cause_of(const Error& error) { return json::parse(error.details_json()).at("cause"); }

// -- The praenora tool as a child process.

struct ToolRun {
    bool exited = false; // false: it did not end within the limit, and was killed
    int exit_code = -1;
    std::string out, err;
    double seconds = 0;                          // from its start to its end, on the steady clock
    std::chrono::system_clock::time_point ended; // when the test saw it end, on the wall clock
};

// Seconds since the epoch of a timestamp of the runtime's log records,
// "YYYY-MM-DDTHH:MM:SS.fffffffffZ" (UTC); nullopt for any other text.
std::optional<double> epoch_seconds(const std::string& text) {
    if (text.size() < 21 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != '.' || text.back() != 'Z') {
        return std::nullopt;
    }
    const auto number = [&text](std::size_t from, std::size_t size) {
        long long value = 0;
        for (std::size_t i = from; i < from + size; ++i) {
            value = value * 10 + (text[i] - '0');
        }
        return value;
    };
    long long year = number(0, 4);
    const long long month = number(5, 2);
    const long long day = number(8, 2);
    // Days from 1970-01-01 to the date (the proleptic Gregorian calendar).
    year -= month <= 2 ? 1 : 0;
    const long long era = (year >= 0 ? year : year - 399) / 400;
    const long long year_of_era = year - era * 400;
    const long long day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const long long day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    const long long days = era * 146097 + day_of_era - 719468;
    const std::size_t digits = text.size() - 21; // the fraction's, between '.' and 'Z'
    double fraction = 0;
    double scale = 0.1;
    for (std::size_t i = 0; i < digits; ++i, scale /= 10) {
        fraction += (text[20 + i] - '0') * scale;
    }
    return static_cast<double>(days * 86400 + number(11, 2) * 3600 + number(14, 2) * 60 +
                               number(17, 2)) +
           fraction;
}

double epoch_seconds(std::chrono::system_clock::time_point time) {
    return std::chrono::duration<double>(time.time_since_epoch()).count();
}

#ifdef _WIN32
std::wstring wide(const std::string& text) { return utf8_path(text).wstring(); }

// `argument` quoted for CreateProcessW, so that the C runtime of the program splits it back as it
// was (backslashes before a quote are doubled).
std::wstring quoted(const std::wstring& argument) {
    std::wstring text = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        text.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        text += c;
        backslashes = 0;
    }
    text.append(backslashes * 2, L'\\');
    text += L'"';
    return text;
}

// This process's environment block without the variables named in `unset` (compared without
// regard to case, as Windows compares them).
std::vector<wchar_t> environment_without(const std::vector<std::string>& unset) {
    std::vector<wchar_t> block;
    LPWCH strings = GetEnvironmentStringsW();
    for (LPWCH entry = strings; entry != nullptr && *entry != L'\0'; entry += wcslen(entry) + 1) {
        const std::wstring text(entry);
        const std::size_t equals = text.find(L'=', 1); // "=C:=..." entries start with '='
        const std::wstring name = text.substr(0, equals);
        const bool dropped = std::any_of(unset.begin(), unset.end(), [&](const std::string& n) {
            return CompareStringOrdinal(name.c_str(), -1, wide(n).c_str(), -1, TRUE) == CSTR_EQUAL;
        });
        if (!dropped) {
            block.insert(block.end(), text.begin(), text.end());
            block.push_back(L'\0');
        }
    }
    if (strings != nullptr) {
        FreeEnvironmentStringsW(strings);
    }
    block.push_back(L'\0');
    if (block.size() == 1) {
        block.push_back(L'\0');
    }
    return block;
}

// A file handle the child inherits, for one of its standard streams.
HANDLE inheritable(const fs::path& file, bool write) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof attributes;
    attributes.bInheritHandle = TRUE;
    return CreateFileW(file.c_str(), write ? GENERIC_WRITE : GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                       write ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}
#endif

// Runs the praenora tool with `args`, its standard output and error to files in `dir`, its
// standard input empty, and this process's environment without the variables of `unset`; waits
// at most `limit`, then kills it.
ToolRun run_tool(const std::vector<std::string>& args, const fs::path& dir,
                 const std::vector<std::string>& unset = {},
                 std::chrono::seconds limit = std::chrono::seconds(120)) {
    fs::create_directories(dir);
    const fs::path out = dir / "stdout.txt";
    const fs::path err = dir / "stderr.txt";
    ToolRun run;
    const auto start = Clock::now();
#ifdef _WIN32
    std::wstring command = quoted(wide(PRAENORA_TEST_TOOL));
    for (const std::string& argument : args) {
        command += L" " + quoted(wide(argument));
    }
    std::vector<wchar_t> environment = environment_without(unset);
    const fs::path null_file = dir / "stdin.txt";
    write_text_file(null_file, "");
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = inheritable(null_file, false);
    startup.hStdOutput = inheritable(out, true);
    startup.hStdError = inheritable(err, true);
    PROCESS_INFORMATION process{};
    const BOOL started =
        CreateProcessW(wide(PRAENORA_TEST_TOOL).c_str(), command.data(), nullptr, nullptr, TRUE,
                       CREATE_UNICODE_ENVIRONMENT, environment.data(), nullptr, &startup, &process);
    for (HANDLE handle : {startup.hStdInput, startup.hStdOutput, startup.hStdError}) {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
    if (!started) {
        ADD_FAILURE() << "CreateProcessW of " << PRAENORA_TEST_TOOL << ": " << GetLastError();
        return run;
    }
    const DWORD waited = WaitForSingleObject(
        process.hProcess,
        static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(limit).count()));
    if (waited == WAIT_OBJECT_0) {
        DWORD code = 0;
        run.exited = GetExitCodeProcess(process.hProcess, &code) != 0;
        run.exit_code = static_cast<int>(code);
    } else {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    std::vector<std::string> command = {PRAENORA_TEST_TOOL};
    command.insert(command.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& argument : command) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    std::vector<char*> envp;
    for (char** entry = environ; *entry != nullptr; ++entry) {
        const std::string text(*entry);
        const std::string name = text.substr(0, text.find('='));
        if (std::find(unset.begin(), unset.end(), name) == unset.end()) {
            envp.push_back(*entry);
        }
    }
    envp.push_back(nullptr);
    const std::string out_path = out.string();
    const std::string err_path = err.string();
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                     0600);
    posix_spawn_file_actions_addopen(&actions, 2, err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                     0600);
    pid_t pid = -1;
    const int started =
        posix_spawn(&pid, command.front().c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (started != 0) {
        ADD_FAILURE() << "posix_spawn() of " << command.front() << ": " << std::strerror(started);
        return run;
    }
    const auto deadline = start + limit;
    for (;;) {
        int status = 0;
        const pid_t reaped = waitpid(pid, &status, WNOHANG);
        if (reaped == pid) {
            run.exited = true;
            run.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            break;
        }
        if (reaped < 0 && errno != EINTR) {
            break;
        }
        if (Clock::now() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif
    run.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    run.ended = std::chrono::system_clock::now();
    run.out = read_text(out);
    run.err = read_text(err);
    return run;
}

// The arguments that give the tool the Python fixture modules and the test catalog.
std::vector<std::string> tool_paths() {
    return {"--module-path", PRAENORA_TEST_PYTHON_MODULES_DIR, "--catalog-path",
            PRAENORA_TEST_CATALOG_DIR};
}

// `praenora run --json` of `config` with `more` arguments, its output directory `out`.
ToolRun run_tool_json(const fs::path& config, const fs::path& dir, const fs::path& out,
                      const std::vector<std::string>& more = {},
                      const std::vector<std::string>& unset = {}) {
    std::vector<std::string> args = {"run", utf8(config), "--json", "--output-dir", utf8(out)};
    const auto paths = tool_paths();
    args.insert(args.end(), paths.begin(), paths.end());
    args.insert(args.end(), more.begin(), more.end());
    return run_tool(args, dir, unset);
}

// The one JSON document of a tool's standard output (a failure when it is not exactly one).
json json_output(const ToolRun& run) {
    const json document = json::parse(run.out, nullptr, false);
    EXPECT_FALSE(document.is_discarded()) << "standard output is not one JSON document:\n"
                                          << run.out << "\nstderr:\n"
                                          << run.err;
    return document.is_discarded() ? json::object() : document;
}

// The isolation modes that this platform runs steps in.
std::vector<IsolationMode> modes() {
#ifdef _WIN32
    return {IsolationMode::in_process};
#else
    return {IsolationMode::in_process, IsolationMode::process};
#endif
}

const char* name_of(IsolationMode mode) {
    return mode == IsolationMode::process ? "process" : "in-process";
}

} // namespace

// fx-py-echo in the runtime's process: each tick copies the source to the output, its text not
// ASCII included; the run record names the provider python and its engine for the step's service.
TEST(PythonModuleRun, EchoInProcess) {
    ScratchDir dir;
    const Runtime runtime(options_logging_to(dir.path() / "run.log"));
    const std::string config =
        write_config(dir.path(), "test.single", "run: { ticks: 2 }\n", step("a", "fx-py-echo"));
    ASSERT_TRUE(run_to_completion(runtime, config, dir.path() / "out"));
    const auto data = results(dir.path() / "out");
    ASSERT_EQ(data.size(), 2u);
    for (const json& tick : data) {
        EXPECT_EQ(tick, json::parse(kSource));
    }
    const json record = run_record(dir.path() / "out");
    const json& reproducibility = record.at("reproducibility");
    EXPECT_EQ(reproducibility.at("steps").at("a").at("services"), json({"praenora.python@0"}));
    const json& service = reproducibility.at("services").at("praenora.python@0");
    EXPECT_EQ(service.at("provider").at("name"), "python");
    EXPECT_EQ(service.at("engine").at("name"), "cpython");
    EXPECT_EQ(service.at("engine").at("environment").at("name"), "praenora-runtime-pyenv");
    EXPECT_EQ(started_module_hosts(dir.path() / "run.log"), 0u);
}

#ifndef _WIN32
// fx-py-echo in a module host gives the outputs of the runtime's process, byte for byte, and the
// same fingerprint and reproducibility section.
TEST(PythonModuleRun, EchoInModuleHost) {
    ScratchDir dir;
    const std::string config = write_config(dir.path(), "test.single", "run: { ticks: 3 }\n",
                                            step("a", "fx-py-echo", "{ add: { n: 1 } }"));
    std::map<IsolationMode, fs::path> outs;
    for (const IsolationMode mode : modes()) {
        SCOPED_TRACE(name_of(mode));
        const fs::path log = dir.path() / (std::string(name_of(mode)) + ".log");
        const Runtime runtime(options_logging_to(log));
        outs[mode] = dir.path() / name_of(mode);
        ASSERT_TRUE(run_to_completion(runtime, config, outs[mode], mode));
        EXPECT_EQ(started_module_hosts(log), mode == IsolationMode::process ? 1u : 0u);
    }
    const fs::path& in_process = outs.at(IsolationMode::in_process);
    const fs::path& process = outs.at(IsolationMode::process);
    EXPECT_EQ(read_text(in_process / "outputs" / "result.jsonl"),
              read_text(process / "outputs" / "result.jsonl"));
    const json first = run_record(in_process);
    const json second = run_record(process);
    EXPECT_EQ(first.at("fingerprint"), second.at("fingerprint"));
    EXPECT_EQ(first.at("reproducibility").dump(), second.at("reproducibility").dump());
    EXPECT_EQ(second.at("execution").at("isolation").at("a").at("mode"), "process");
}
#endif

// The step's parameters reach the class through the create document.
TEST(PythonModuleRun, ParametersReachTheClass) {
    ScratchDir dir;
    const Runtime runtime(options_logging_to(dir.path() / "run.log"));
    const std::string config =
        write_config(dir.path(), "test.single", "run: { ticks: 1 }\n",
                     step("a", "fx-py-echo", "{ add: { gain: 2.5, name: \"caf\xC3\xA9\" } }"));
    ASSERT_TRUE(run_to_completion(runtime, config, dir.path() / "out"));
    const auto data = results(dir.path() / "out");
    ASSERT_EQ(data.size(), 1u);
    EXPECT_EQ(data[0].at("gain"), 2.5);
    EXPECT_EQ(data[0].at("name"), "caf\xC3\xA9");
    EXPECT_EQ(data[0].at("value"), 42);
}

// An exception in invoke fails the step (STEP_FAILED): the module's error is INTERNAL, its message
// "<Type>: <message>", and its details' traceback names the frames of the module's code.
TEST(PythonModuleRun, FailureCarriesTraceback) {
    ScratchDir dir;
    const Runtime runtime(options_logging_to(dir.path() / "run.log"));
    const std::string config =
        write_config(dir.path(), "test.single", "run: { ticks: 1 }\n", step("a", "fx-py-fail"));
    const Error error = failure_of(runtime, config);
    EXPECT_EQ(error.code(), ErrorCode::STEP_FAILED) << error.what();
    EXPECT_TRUE(contains(error.what(), "ValueError: fx-py-fail: the tick 0 is refused"))
        << error.what();
    const json details = json::parse(error.details_json());
    EXPECT_EQ(details.at("phase"), "invoke");
    const json cause = details.at("cause");
    EXPECT_EQ(cause.at("code"), 10); // PRAENORA_STATUS_INTERNAL
    EXPECT_EQ(cause.at("message"), "ValueError: fx-py-fail: the tick 0 is refused");
    const json& traceback = cause.at("details").at("traceback");
    ASSERT_TRUE(traceback.is_array());
    std::string text;
    for (const json& line : traceback) {
        text += line.get<std::string>() + "\n";
    }
    EXPECT_TRUE(contains(text, "fx_py_fail.py")) << text;
    EXPECT_TRUE(contains(text, "in _check")) << text;
    EXPECT_TRUE(contains(text, "in _refuse")) << text;
    EXPECT_EQ(traceback.back(), "ValueError: fx-py-fail: the tick 0 is refused");
}

// A class that polls the cancel flag ends its call with sdk.Cancelled once the deadline passes: the
// call returned after its deadline, within the grace, so the step timed out (STEP_TIMEOUT), and the
// module's error is CANCELLED with the class's details.
TEST(PythonModuleRun, CancelledByDeadline) {
    ScratchDir dir;
    const Runtime runtime(options_logging_to(dir.path() / "run.log"));
    const std::string config =
        write_config(dir.path(), "test.single",
                     "run:\n  ticks: 1\n  timeouts: { invoke: 300ms, cancel_grace: 10s }\n",
                     step("a", "fx-py-cancel"));
    const auto start = Clock::now();
    const Error error = failure_of(runtime, config);
    EXPECT_LT(Clock::now() - start, std::chrono::seconds(10));
    EXPECT_EQ(error.code(), ErrorCode::STEP_TIMEOUT) << error.what();
    const json cause = cause_of(error);
    EXPECT_EQ(cause.at("code"), 7); // PRAENORA_STATUS_CANCELLED
    EXPECT_TRUE(contains(cause.at("message").get<std::string>(), "fx-py-cancel: cancelled"))
        << cause;
    EXPECT_GE(cause.at("details").at("polls").get<int>(), 1);
}

// A call that never polls the cancel flag, in the runtime's process: from the start of the step's
// invoke (its step.ready record, which the first tick's invoke follows at once), `praenora run`
// ends within the invoke timeout (1 s) plus the cancellation grace (1 s) plus a margin of 5 s, the
// run failed as unresponsive (MODULE_UNRESPONSIVE, exit code 5; the call is abandoned), and the
// tool's process exits although the abandoned call still runs Python code. The tool's own start,
// the provider's and the interpreter's come before the invoke and are left out of that bound; the
// whole run is bounded by run_tool()'s limit of 120 s.
TEST(PythonModuleRun, IgnoredCancelEndsWithinGrace) {
    ScratchDir dir;
    const fs::path log = dir.path() / "tool.log";
    const ToolRun run =
        run_tool_json(python_configs() / "ignore-cancel" / "workflow.yaml", dir.path() / "tool",
                      dir.path() / "out",
                      {"--log-level", "debug", "--log-format", "json", "--log-file", utf8(log)});
    ASSERT_TRUE(run.exited) << "praenora run did not exit within 120 s\n" << run.err;
    EXPECT_EQ(run.exit_code, 5) << run.err;
    const json document = json_output(run);
    EXPECT_EQ(document.value("ok", true), false) << document;
    EXPECT_EQ(document.at("error").at("name"), "MODULE_UNRESPONSIVE") << document;
    EXPECT_EQ(run_record(dir.path() / "out").at("execution").at("status"), "failed");

    std::optional<double> ready;
    bool deadline = false;
    for (const json& record : log_records(log)) {
        if (event_of(record) == "step.ready" && !ready) {
            ready = epoch_seconds(record.at("timestamp").get<std::string>());
        }
        deadline =
            deadline || (event_of(record) == "call.deadline" && logger_of(record) == "module.a");
    }
    ASSERT_TRUE(ready.has_value()) << "no step.ready record in " << utf8(log);
    EXPECT_TRUE(deadline) << "no call.deadline record of step a in " << utf8(log);
    const double from_invoke = epoch_seconds(run.ended) - *ready;
    EXPECT_GT(from_invoke, 1.0 + 1.0 - 0.1) << "the call ended before its deadline and grace";
    EXPECT_LT(from_invoke, 1.0 + 1.0 + 5.0) << run.err;
    std::cout << "[ timing   ] from the step's invoke to the tool's end: " << from_invoke
              << " s; the whole run: " << run.seconds << " s\n";
}

// print(), print to sys.stderr and logging in a module become log records of its step, at the
// levels info, warn and warn, in order; nothing of them reaches the tool's standard output, which
// holds exactly one JSON document with --json.
TEST(PythonModuleRun, PrintIsALogRecord) {
    ScratchDir dir;
    const fs::path log = dir.path() / "run.log";
    const Runtime runtime(options_logging_to(log));
    const std::string config =
        write_config(dir.path(), "test.single", "run: { ticks: 2 }\n", step("a", "fx-py-print"));
    ASSERT_TRUE(run_to_completion(runtime, config, dir.path() / "out"));
    EXPECT_EQ(module_messages(log, "a"),
              (std::vector<std::string>{
                  "INFO fx-py-print: printed at tick 0", "WARN fx-py-print: to stderr at tick 0",
                  "WARN fx_py_print: fx-py-print: logged at tick 0",
                  "INFO fx-py-print: printed at tick 1", "WARN fx-py-print: to stderr at tick 1",
                  "WARN fx_py_print: fx-py-print: logged at tick 1"}));

    const ToolRun run =
        run_tool_json(python_configs() / "print" / "workflow.yaml", dir.path() / "tool",
                      dir.path() / "tool-out", {"--log-level", "info"});
    ASSERT_TRUE(run.exited) << run.err;
    EXPECT_EQ(run.exit_code, 0) << run.err;
    EXPECT_EQ(json_output(run).value("ok", false), true) << run.out;
    EXPECT_FALSE(contains(run.out, "fx-py-print")) << run.out;
    EXPECT_TRUE(contains(run.err, "fx-py-print: printed at tick 1")) << run.err;
}

// numpy computes in a module, and its generator, seeded from the run's seed, gives the same draw
// in two runs of one seed and another draw with another seed.
TEST(PythonModuleRun, NumpyComputes) {
    ScratchDir dir;
    const Runtime runtime(options_logging_to(dir.path() / "run.log"));
    std::vector<json> draws;
    for (const char* seed : {"7", "7", "8"}) {
        const fs::path out = dir.path() / ("out-" + std::to_string(draws.size()));
        const std::string config = write_config(
            dir.path(), "test.single", std::string("run: { ticks: 1, seed: ") + seed + " }\n",
            step("a", "fx-py-numpy", "{ count: 1000 }"));
        ASSERT_TRUE(run_to_completion(runtime, config, out));
        const auto data = results(out);
        ASSERT_EQ(data.size(), 1u);
        EXPECT_EQ(data[0].at("sum"), 499500.0);
        double square_root_sum = 0;
        for (int i = 0; i < 1000; ++i) {
            square_root_sum += std::sqrt(static_cast<double>(i));
        }
        EXPECT_NEAR(data[0].at("square_root_sum").get<double>(), square_root_sum, 1e-6);
        draws.push_back(data[0].at("draw"));
    }
    EXPECT_EQ(draws[0], draws[1]);
    EXPECT_NE(draws[0], draws[2]);
}

// Two Python modules in one run share the process's one interpreter: both run, in a chain.
TEST(PythonModuleRun, TwoModulesShareTheInterpreter) {
    ScratchDir dir;
    const fs::path log = dir.path() / "run.log";
    const Runtime runtime(options_logging_to(log));
    const std::string config =
        write_config(dir.path(), "test.chain", "run: { ticks: 2 }\n",
                     step("a", "fx-py-print") + step("b", "fx-py-echo", "{ add: { b: true } }"));
    ASSERT_TRUE(run_to_completion(runtime, config, dir.path() / "out"));
    const auto data = results(dir.path() / "out");
    ASSERT_EQ(data.size(), 2u);
    EXPECT_EQ(data[1], json({{"tick", 1}, {"b", true}}));
    EXPECT_EQ(module_messages(log, "a").size(), 6u);
}

// Two modules that ship a top-level package of one name cannot share a process: the second one's
// create fails (INVALID_ARGUMENT: the package is already loaded from the first one's sources). In
// module hosts, one per step, both run.
TEST(PythonModuleRun, PackageClashRefused) {
    ScratchDir dir;
    const Runtime runtime(options_logging_to(dir.path() / "run.log"));
    const std::string config =
        write_config(dir.path(), "test.chain", "run: { ticks: 1 }\n",
                     step("a", "fx-py-clash-a") + step("b", "fx-py-clash-b"));
    const Error error = failure_of(runtime, config);
    EXPECT_EQ(error.code(), ErrorCode::STEP_FAILED) << error.what();
    const json details = json::parse(error.details_json());
    EXPECT_EQ(details.at("step"), "b");
    EXPECT_EQ(details.at("phase"), "create");
    const json& cause = details.at("cause");
    EXPECT_EQ(cause.at("code"), 2); // PRAENORA_STATUS_INVALID_ARGUMENT
    const std::string message = cause.at("message").get<std::string>();
    EXPECT_TRUE(contains(message, "package fx_py_clash is already loaded from")) << message;
    EXPECT_TRUE(contains(message, "fx-py-clash-a.python")) << message;
    EXPECT_EQ(cause.at("details").at("package"), "fx_py_clash");
#ifndef _WIN32
    ASSERT_TRUE(run_to_completion(runtime, config, dir.path() / "hosts", IsolationMode::process));
    EXPECT_EQ(results(dir.path() / "hosts"), (std::vector<json>{{{"from", "b"}}}));
#endif
}

namespace {

// A copy of fixture module `name`'s directory (library, manifest, its Python sources) in
// `<dir>/<name>`; returns the copy's search path, `dir`.
fs::path copy_python_module(const std::string& name, const fs::path& dir,
                            const fs::path& from = python_modules()) {
    fs::create_directories(dir);
    fs::copy(from / utf8_path(name), dir / utf8_path(name), fs::copy_options::recursive);
    return dir;
}

} // namespace

// Sources edited after the build are refused: a copy of fx-py-echo whose Python file has one more
// line fails its create with DATA_INVALID, naming the digest the library was built with and the
// one of the files; an unedited copy runs.
TEST(PythonModuleRun, EditedSourcesRefused) {
    ScratchDir dir;
    const fs::path unedited = copy_python_module("fx-py-echo", dir.path() / "unedited");
    const fs::path edited = copy_python_module("fx-py-echo", dir.path() / "edited");
    {
        std::ofstream source(edited / "fx-py-echo" / "fx-py-echo.python" / "fx_py_echo.py",
                             std::ios::binary | std::ios::app);
        source << "# edited after the build\n";
    }
    const std::string config =
        write_config(dir.path(), "test.single", "run: { ticks: 1 }\n", step("a", "fx-py-echo"));
    const Runtime good(options_logging_to(dir.path() / "good.log", {utf8(unedited)}));
    ASSERT_TRUE(run_to_completion(good, config, dir.path() / "out"));

    const Runtime bad(options_logging_to(dir.path() / "bad.log", {utf8(edited)}));
    const Error error = failure_of(bad, config);
    EXPECT_EQ(error.code(), ErrorCode::STEP_FAILED) << error.what();
    const json cause = cause_of(error);
    EXPECT_EQ(cause.at("code"), 12) << cause; // PRAENORA_STATUS_DATA_INVALID
    EXPECT_TRUE(contains(cause.at("message").get<std::string>(),
                         "differ from those the module was built with"))
        << cause;
    EXPECT_EQ(cause.at("details").at("expected_sha256"), PRAENORA_TEST_PYTHON_ECHO_DIGEST);
    EXPECT_NE(cause.at("details").at("actual_sha256"), PRAENORA_TEST_PYTHON_ECHO_DIGEST);
}

// The code below a path is fixed for the process: after fx-py-echo ran from a copy, the copy is
// replaced by the variant build (another library, other sources), and its create in the same
// process fails with DATA_INVALID, saying to use a new process; in a module host, a process of its
// own, the variant runs. (Skipped where module libraries stay loaded: the old library would run.)
TEST(PythonModuleRun, SourcesChangedInOneProcessRefused) {
    if (keep_libraries_requested()) {
        GTEST_SKIP() << "PRAENORA_DEBUG_KEEP_LIBRARIES=1 keeps the first library loaded";
    }
    ScratchDir dir;
    const fs::path copy = copy_python_module("fx-py-echo", dir.path() / "modules");
    const std::string config =
        write_config(dir.path(), "test.single", "run: { ticks: 1 }\n", step("a", "fx-py-echo"));
    {
        const Runtime first(options_logging_to(dir.path() / "first.log", {utf8(copy)}));
        ASSERT_TRUE(run_to_completion(first, config, dir.path() / "first"));
    }
    fs::remove_all(copy / "fx-py-echo");
    copy_python_module("fx-py-echo", copy, utf8_path(PRAENORA_TEST_PYTHON_MODULES_VARIANT_DIR));
    // A new Runtime reads the module's manifest again.
    const Runtime runtime(options_logging_to(dir.path() / "run.log", {utf8(copy)}));
    const Error error = failure_of(runtime, config);
    EXPECT_EQ(error.code(), ErrorCode::STEP_FAILED) << error.what();
    const json cause = cause_of(error);
    EXPECT_EQ(cause.at("code"), 12) << cause; // PRAENORA_STATUS_DATA_INVALID
    EXPECT_TRUE(contains(cause.at("message").get<std::string>(), "new process")) << cause;
    EXPECT_EQ(cause.at("details").at("imported_sha256"), PRAENORA_TEST_PYTHON_ECHO_DIGEST);
    EXPECT_EQ(cause.at("details").at("expected_sha256"), PRAENORA_TEST_PYTHON_ECHO_VARIANT_DIGEST);
#ifndef _WIN32
    ASSERT_TRUE(run_to_completion(runtime, config, dir.path() / "host", IsolationMode::process));
#endif
}

// Two builds of fx-py-echo@0.1.0 whose Python sources differ in one line: the libraries' SHA-256
// differ (each carries its sources' digest), and so do the fingerprints; the outputs are the same.
// Each runs in a process of its own (`praenora run`): in one process the second would be refused,
// its package fx_py_echo being loaded already from the first one's sources.
TEST(PythonModuleRun, FingerprintFollowsSources) {
    EXPECT_NE(std::string(PRAENORA_TEST_PYTHON_ECHO_DIGEST),
              std::string(PRAENORA_TEST_PYTHON_ECHO_VARIANT_DIGEST));
    ScratchDir dir;
    const std::string config =
        write_config(dir.path(), "test.single", "run: { ticks: 1 }\n", step("a", "fx-py-echo"));
    std::vector<json> records;
    for (const char* modules :
         {PRAENORA_TEST_PYTHON_MODULES_DIR, PRAENORA_TEST_PYTHON_MODULES_VARIANT_DIR}) {
        const std::string index = std::to_string(records.size());
        const fs::path out = dir.path() / ("out-" + index);
        const ToolRun run =
            run_tool({"run", config, "--json", "--output-dir", utf8(out), "--module-path", modules,
                      "--catalog-path", PRAENORA_TEST_CATALOG_DIR},
                     dir.path() / ("tool-" + index));
        ASSERT_TRUE(run.exited) << run.err;
        ASSERT_EQ(run.exit_code, 0) << run.err;
        records.push_back(run_record(out));
        EXPECT_EQ(results(out), results(dir.path() / "out-0"));
    }
    const auto library = [](const json& record) {
        return record.at("reproducibility").at("steps").at("a").at("module").at("library_sha256");
    };
    EXPECT_NE(library(records[0]), library(records[1]));
    EXPECT_NE(records[0].at("fingerprint"), records[1].at("fingerprint"));
}

// Two instances of one Python module whose manifest declares features.concurrency "instance",
// called at the same time: two runs of one Runtime on two threads, each with a step of
// fx-py-concurrent whose invoke waits at a barrier of two that both instances share, so that each
// tick of each run returns only while the other run's call is in invoke too. Both runs complete
// with their own results.
TEST(PythonModuleRun, ConcurrentInstances) {
    ScratchDir dir;
    const Runtime runtime(options_logging_to(dir.path() / "run.log"));
    std::vector<std::string> configs;
    for (const int factor : {2, 3}) {
        const fs::path subdir = dir.path() / ("config-" + std::to_string(factor));
        configs.push_back(
            write_config(subdir, "test.single", "run: { ticks: 3 }\n",
                         step("a", "fx-py-concurrent",
                              "{ barrier: true, factor: " + std::to_string(factor) + " }")));
    }
    std::vector<std::optional<praenora::RunSummary>> summaries(2);
    std::vector<std::string> failures(2);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < 2; ++i) {
        threads.emplace_back([&, i] {
            try {
                WorkflowOptions overrides;
                overrides.output_dir = utf8(dir.path() / ("out-" + std::to_string(i)));
                const auto workflow = runtime.open(configs[i], overrides);
                summaries[i] = workflow->run();
            } catch (const std::exception& error) {
                failures[i] = error.what();
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (std::size_t i = 0; i < 2; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(failures[i], "");
        ASSERT_TRUE(summaries[i].has_value());
        EXPECT_EQ(summaries[i]->final_state, WorkflowState::completed);
        const auto data = results(dir.path() / ("out-" + std::to_string(i)));
        ASSERT_EQ(data.size(), 3u);
        for (const json& tick : data) {
            EXPECT_EQ(tick, json({{"value", 42 * (i == 0 ? 2 : 3)}, {"step", "a"}}));
        }
    }
}

// A result of 10 MiB of numbers reaches the run's output intact, in several sink writes; one above
// the service's limit of 16 MiB fails the step, with the module's error RESOURCE_EXHAUSTED naming
// the limit.
TEST(PythonModuleRun, LargeResult) {
    ScratchDir dir;
    const Runtime runtime(options_logging_to(dir.path() / "run.log"));
    const int count = 1500000;
    const std::string config =
        write_config(dir.path(), "test.single", "run: { ticks: 1 }\n",
                     step("a", "fx-py-large", "{ count: " + std::to_string(count) + " }"));
    ASSERT_TRUE(run_to_completion(runtime, config, dir.path() / "out"));
    const std::string line = read_text(dir.path() / "out" / "outputs" / "result.jsonl");
    EXPECT_GT(line.size(), std::size_t{10} << 20);
    const json numbers = json::parse(line).at("data").at("numbers");
    ASSERT_EQ(numbers.size(), static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        if (numbers[static_cast<std::size_t>(i)] != i) {
            FAIL() << "number " << i << " is " << numbers[static_cast<std::size_t>(i)];
        }
    }

    const fs::path too_large = dir.path() / "too-large";
    const std::string refused = write_config(too_large, "test.single", "run: { ticks: 1 }\n",
                                             step("a", "fx-py-large", "{ count: 2300000 }"));
    const Error error = failure_of(runtime, refused);
    EXPECT_EQ(error.code(), ErrorCode::STEP_FAILED) << error.what();
    const json cause = cause_of(error);
    EXPECT_EQ(cause.at("code"), 6) << cause; // PRAENORA_STATUS_RESOURCE_EXHAUSTED
    EXPECT_TRUE(contains(cause.at("message").get<std::string>(), "16 MiB")) << cause;
}

// fx-python-call, a C++ module, creates an object of its own Python sources through
// praenora.python@0 and calls it in each tick, in the runtime's process and in a module host.
TEST(PythonModuleRun, ServiceUsedDirectlyByCppModule) {
    ScratchDir dir;
    const std::string config =
        write_config(dir.path(), "test.single", "run: { ticks: 2 }\n", step("a", "fx-python-call"));
    for (const IsolationMode mode : modes()) {
        SCOPED_TRACE(name_of(mode));
        const Runtime runtime(options_logging_to(dir.path() / "run.log"));
        const fs::path out = dir.path() / name_of(mode);
        ASSERT_TRUE(run_to_completion(runtime, config, out, mode));
        EXPECT_EQ(results(out), (std::vector<json>{{{"count", 3}, {"sum", 6.5}, {"calls", 1}},
                                                   {{"count", 3}, {"sum", 6.5}, {"calls", 2}}}));
    }
}

// `praenora run` started without PYTHONHASHSEED sets it to 0 for its own process and its module
// hosts: fx-py-env sees "0".
TEST(PythonModuleRun, ToolSetsHashSeed) {
    ScratchDir dir;
    for (const IsolationMode mode : modes()) {
        SCOPED_TRACE(name_of(mode));
        const fs::path out = dir.path() / name_of(mode);
        const ToolRun run = run_tool_json(python_configs() / "hash-seed" / "workflow.yaml",
                                          dir.path() / (std::string("tool-") + name_of(mode)), out,
                                          {"--isolation", name_of(mode)}, {"PYTHONHASHSEED"});
        ASSERT_TRUE(run.exited) << run.err;
        ASSERT_EQ(run.exit_code, 0) << run.err;
        EXPECT_EQ(results(out), (std::vector<json>{{{"PYTHONHASHSEED", "0"}}}));
    }
}

// Python code that recurses close to the default recursion limit (900 calls through
// functools.lru_cache, each of which enters the interpreter again and takes the C stack) runs, in
// the runtime's process and in a module host, as it does in a Python program: the threads that
// make a module's calls have a main thread's stack. The tool runs each case, so that a stack that
// is too small fails the tool's process, not this test's.
TEST(PythonModuleRun, DeepRecursionRuns) {
    ScratchDir dir;
    const std::string config = write_config(dir.path(), "test.single", "run: { ticks: 1 }\n",
                                            step("a", "fx-py-deep", "{ depth: 900 }"));
    for (const IsolationMode mode : modes()) {
        SCOPED_TRACE(name_of(mode));
        const fs::path out = dir.path() / (std::string("out-") + name_of(mode));
        const ToolRun run =
            run_tool_json(config, dir.path() / (std::string("tool-") + name_of(mode)), out,
                          {"--isolation", name_of(mode)});
        ASSERT_TRUE(run.exited) << run.err;
        EXPECT_EQ(run.exit_code, 0) << run.out << "\n" << run.err;
        if (run.exit_code == 0) {
            EXPECT_EQ(results(out), (std::vector<json>{{{"depth", 900}}}));
        }
    }
}

// What Python keeps per thread lasts from one call of an instance to the next, in the runtime's
// process and in a module host: a context variable, numpy's error state and a threading.local
// value that initialize sets are what each invoke reads, a count kept in the threading.local
// value grows from tick to tick, and every call runs on the thread that created the instance.
TEST(PythonModuleRun, ThreadStateLastsFromCallToCall) {
    ScratchDir dir;
    const std::string config = write_config(dir.path(), "test.single", "run: { ticks: 2 }\n",
                                            step("a", "fx-py-thread-state"));
    for (const IsolationMode mode : modes()) {
        SCOPED_TRACE(name_of(mode));
        const Runtime runtime(options_logging_to(dir.path() / "run.log"));
        const fs::path out = dir.path() / name_of(mode);
        ASSERT_TRUE(run_to_completion(runtime, config, out, mode));
        std::vector<json> expected;
        for (const int invocations : {1, 2}) {
            expected.push_back({{"context", "initialized"},
                                {"numpy_divide", "raise"},
                                {"local", "initialized"},
                                {"invocations", invocations},
                                {"same_thread", true}});
        }
        EXPECT_EQ(results(out), expected);
    }
}

// A class that does not derive from praenora.sdk.Module has no class method _praenora_adapter, by
// which the runtime creates a Python module's object: create fails, INTERNAL, naming it.
TEST(PythonModuleRun, ClassNotAModuleFailsCreate) {
    ScratchDir dir;
    const Runtime runtime(options_logging_to(dir.path() / "run.log"));
    const std::string config = write_config(dir.path(), "test.single", "run: { ticks: 1 }\n",
                                            step("a", "fx-py-plain-class"));
    const Error error = failure_of(runtime, config);
    EXPECT_EQ(error.code(), ErrorCode::STEP_FAILED) << error.what();
    const json details = json::parse(error.details_json());
    EXPECT_EQ(details.at("phase"), "create");
    EXPECT_EQ(details.at("cause").at("code"), 10); // PRAENORA_STATUS_INTERNAL
    const std::string message = details.at("cause").at("message").get<std::string>();
    EXPECT_TRUE(contains(message, "AttributeError")) << message;
    EXPECT_TRUE(contains(message, "_praenora_adapter")) << message;
    EXPECT_TRUE(details.at("cause").at("details").at("traceback").is_array());
}

#ifndef _WIN32

// os.abort() in a module host ends that process only: the step fails with MODULE_CRASHED (SIGABRT)
// and the run's other step, in the runtime's process, is finalized and destroyed as usual.
TEST(PythonModuleIsolation, AbortIsContained) {
    ScratchDir dir;
    const fs::path log = dir.path() / "run.log";
    const Runtime runtime(options_logging_to(log));
    const std::string config =
        write_config(dir.path(), "test.chain", "run: { ticks: 2 }\n",
                     step("a", "fx-lifecycle") +
                         step("b", "fx-py-abort", "{}", "    isolation: { mode: process }\n"));
    const Error error = failure_of(runtime, config);
    EXPECT_EQ(error.code(), ErrorCode::MODULE_CRASHED) << error.what();
    const json details = json::parse(error.details_json());
    EXPECT_EQ(details.at("step"), "b");
    EXPECT_EQ(details.at("signal_name"), "SIGABRT");
    std::vector<std::string> messages = module_messages(log, "a");
    ASSERT_GE(messages.size(), 2u);
    EXPECT_EQ(messages[messages.size() - 2], "INFO lifecycle:finalize");
    EXPECT_EQ(messages.back(), "INFO lifecycle:destroy");
}

// A Python step in a module host under the restricted sandbox profile runs, with the results of
// the runtime's process. Skipped in a process that runs in a sandbox already.
TEST(PythonModuleIsolation, RestrictedProfileRuns) {
    if (const char* why = praenora::test::restricted_profile_skip()) {
        GTEST_SKIP() << why;
    }
    ScratchDir dir;
    const Runtime runtime(options_logging_to(dir.path() / "run.log"));
    const std::string config =
        write_config(dir.path(), "test.single",
                     "run: { ticks: 2, isolation: { mode: process, sandbox: restricted } }\n",
                     step("a", "fx-py-echo", "{ add: { restricted: true } }"));
    ASSERT_TRUE(run_to_completion(runtime, config, dir.path() / "out"));
    json expected = json::parse(kSource);
    expected["restricted"] = true;
    EXPECT_EQ(results(dir.path() / "out"), (std::vector<json>{expected, expected}));
    EXPECT_EQ(run_record(dir.path() / "out").at("execution").at("isolation").at("a").at("sandbox"),
              "restricted");
}

// Under the restricted profile a module host may start no process: fx-py-pool's spawn pool fails
// in initialize with a Python exception, which fails the step as any exception does (STEP_FAILED,
// INTERNAL with a traceback), not as a crash. Skipped in a process that runs in a sandbox already.
TEST(PythonModuleIsolation, RestrictedProfileRefusesPool) {
    if (const char* why = praenora::test::restricted_profile_skip()) {
        GTEST_SKIP() << why;
    }
    ScratchDir dir;
    const Runtime runtime(options_logging_to(dir.path() / "run.log"));
    const std::string config =
        write_config(dir.path(), "test.single",
                     "run:\n  ticks: 1\n  isolation: { mode: process, sandbox: restricted }\n"
                     "  timeouts: { lifecycle: 60s, cancel_grace: 2s }\n",
                     step("a", "fx-py-pool"));
    const Error error = failure_of(runtime, config);
    EXPECT_EQ(error.code(), ErrorCode::STEP_FAILED) << error.what();
    const json details = json::parse(error.details_json());
    EXPECT_EQ(details.at("phase"), "initialize") << details;
    EXPECT_EQ(details.at("cause").at("code"), 10) << details; // PRAENORA_STATUS_INTERNAL
    EXPECT_TRUE(details.at("cause").at("details").at("traceback").is_array()) << details;
}

#endif

// fx-py-pool maps numpy work over a spawn pool opened in initialize and closed in finalize: two
// runs in the runtime's process and one in a module host give the same outputs, byte for byte, and
// the default start method is spawn.
TEST(PythonChildProcesses, PoolInProcessAndInModuleHost) {
    ScratchDir dir;
    const std::string config =
        write_config(dir.path(), "test.single", "run: { ticks: 2 }\n", step("a", "fx-py-pool"));
    const Runtime runtime(options_logging_to(dir.path() / "run.log"));
    std::vector<fs::path> outs;
    std::vector<IsolationMode> runs = {IsolationMode::in_process, IsolationMode::in_process};
#ifndef _WIN32
    runs.push_back(IsolationMode::process);
#endif
    for (const IsolationMode mode : runs) {
        outs.push_back(dir.path() / ("out-" + std::to_string(outs.size())));
        ASSERT_TRUE(run_to_completion(runtime, config, outs.back(), mode));
    }
    const auto data = results(outs[0]);
    ASSERT_EQ(data.size(), 2u);
    EXPECT_EQ(data[0].at("start_method"), "spawn");
    ASSERT_EQ(data[0].at("sums").size(), 3u);
    EXPECT_NEAR(data[0].at("sums")[0].get<double>(), 19.306000526035, 1e-9);
    for (const fs::path& out : outs) {
        EXPECT_EQ(read_text(out / "outputs" / "result.jsonl"),
                  read_text(outs[0] / "outputs" / "result.jsonl"))
            << utf8(out);
    }
}

// What a pool's child prints goes to the tool's standard error: `praenora run --json` prints one
// JSON document on its standard output, without the child's line, in its own process and in a
// module host.
TEST(PythonChildProcesses, ChildPrintAvoidsJsonOutput) {
    ScratchDir dir;
    for (const IsolationMode mode : modes()) {
        SCOPED_TRACE(name_of(mode));
        const ToolRun run = run_tool_json(python_configs() / "child-print" / "workflow.yaml",
                                          dir.path() / (std::string("tool-") + name_of(mode)),
                                          dir.path() / (std::string("out-") + name_of(mode)),
                                          {"--isolation", name_of(mode)});
        ASSERT_TRUE(run.exited) << run.err;
        EXPECT_EQ(run.exit_code, 0) << run.err;
        EXPECT_EQ(json_output(run).value("ok", false), true) << run.out;
        EXPECT_FALSE(contains(run.out, "a child printed this")) << run.out;
        EXPECT_TRUE(contains(run.err, "fx-py-pool: a child printed this")) << run.err;
    }
}

#ifndef _WIN32

namespace {

// Waits up to `limit` until no process `pid` exists (a killed child that its new parent has not
// reaped yet still exists).
bool gone_within(std::int64_t pid, std::chrono::milliseconds limit) {
    const auto deadline = Clock::now() + limit;
    while (!(::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH)) {
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

} // namespace

// A module host killed at its call's deadline takes the children of its pool with it: fx-py-pool
// logs its children's process ids, then sleeps past the deadline without polling; the module host
// is killed (MODULE_UNRESPONSIVE, terminated), and none of those processes is left.
TEST(PythonChildProcesses, KilledHostTakesChildren) {
    ScratchDir dir;
    const fs::path log = dir.path() / "run.log";
    const Runtime runtime(options_logging_to(log));
    const std::string config =
        write_config(dir.path(), "test.single",
                     "run:\n  ticks: 1\n  isolation: { mode: process }\n"
                     "  timeouts: { invoke: 3s, cancel_grace: 1s }\n",
                     step("a", "fx-py-pool", "{ report_children: true, sleep_s: 600 }"));
    const Error error = failure_of(runtime, config);
    EXPECT_EQ(error.code(), ErrorCode::MODULE_UNRESPONSIVE) << error.what();
    EXPECT_EQ(json::parse(error.details_json()).value("terminated", false), true)
        << error.details_json();
    std::vector<std::int64_t> children;
    for (const std::string& message : module_messages(log, "a")) {
        const std::string prefix = "INFO fx-py-pool children: ";
        if (message.rfind(prefix, 0) == 0) {
            std::istringstream ids(message.substr(prefix.size()));
            for (std::int64_t id = 0; ids >> id;) {
                children.push_back(id);
            }
        }
    }
    ASSERT_EQ(children.size(), 2u) << "the module logged no children's process ids";
    for (const std::int64_t child : children) {
        EXPECT_TRUE(gone_within(child, std::chrono::seconds(10))) << "child " << child;
    }
}

#endif

namespace {

// The fixture provider test-python-echo's directory, a provider path that holds it alone.
fs::path python_echo_provider() {
    return utf8_path(PRAENORA_TEST_PROVIDERS_DIR) / utf8_path("test-python-echo");
}

// options_logging_to(), with test-python-echo as the only provider beside those of the build
// tree's installation directory (the provider python).
RuntimeOptions echo_options_logging_to(const fs::path& log_file) {
    RuntimeOptions options = options_logging_to(log_file);
    options.provider_paths = {utf8(python_echo_provider())};
    return options;
}

// What fx-service outputs for the source of write_config(), echoed through test.echo@0.
json echoed_source() {
    return {{"echo", json::parse(kSource)}, {"found", {{"test.echo@0", true}}}};
}

// The arguments that give the tool the fixture modules too, and test-python-echo at `provider`.
std::vector<std::string> echo_tool_paths(const fs::path& provider) {
    return {"--module-path", PRAENORA_TEST_MODULES_DIR, "--provider-path", utf8(provider)};
}

} // namespace

// fx-service, in the runtime's process, echoes its input through test-python-echo, the only
// provider of test.echo@0 there: the Python class returns every byte, its text not ASCII
// included; the run record names test-python-echo and its engine object, which names the
// environment.
TEST(PythonEchoProvider, ServesFxServiceInARun) {
    ScratchDir dir;
    const Runtime runtime(echo_options_logging_to(dir.path() / "run.log"));
    const std::string config = write_config(dir.path(), "test.single", "run: { ticks: 2 }\n",
                                            step("a", "fx-service", "{ require_echo: true }"));
    ASSERT_TRUE(run_to_completion(runtime, config, dir.path() / "out"));
    EXPECT_EQ(results(dir.path() / "out"), (std::vector<json>{echoed_source(), echoed_source()}));
    const json record = run_record(dir.path() / "out");
    const json& reproducibility = record.at("reproducibility");
    EXPECT_EQ(reproducibility.at("steps").at("a").at("services"), json({"test.echo@0"}));
    const json& service = reproducibility.at("services").at("test.echo@0");
    EXPECT_EQ(service.at("provider").at("name"), "test-python-echo");
    json engine = service.at("engine");
    EXPECT_EQ(engine.at("environment").at("name"), "praenora-runtime-pyenv") << engine;
    engine.erase("environment");
    EXPECT_EQ(engine, json({{"name", "test-python-echo-engine"}, {"version", "1.0.0"}}));
    EXPECT_EQ(started_module_hosts(dir.path() / "run.log"), 0u);
}

#ifndef _WIN32
// fx-service in a module host, which starts test-python-echo and its Python class itself, gives
// the outputs of the runtime's process, byte for byte, and the same fingerprint.
TEST(PythonEchoProvider, InModuleHost) {
    ScratchDir dir;
    const std::string config = write_config(dir.path(), "test.single", "run: { ticks: 2 }\n",
                                            step("a", "fx-service", "{ require_echo: true }"));
    std::map<IsolationMode, fs::path> outs;
    for (const IsolationMode mode : modes()) {
        SCOPED_TRACE(name_of(mode));
        const fs::path log = dir.path() / (std::string(name_of(mode)) + ".log");
        const Runtime runtime(echo_options_logging_to(log));
        outs[mode] = dir.path() / name_of(mode);
        ASSERT_TRUE(run_to_completion(runtime, config, outs[mode], mode));
        EXPECT_EQ(started_module_hosts(log), mode == IsolationMode::process ? 1u : 0u);
    }
    const fs::path& in_process = outs.at(IsolationMode::in_process);
    const fs::path& process = outs.at(IsolationMode::process);
    EXPECT_EQ(results(process), (std::vector<json>{echoed_source(), echoed_source()}));
    EXPECT_EQ(read_text(in_process / "outputs" / "result.jsonl"),
              read_text(process / "outputs" / "result.jsonl"));
    EXPECT_EQ(run_record(in_process).at("fingerprint"), run_record(process).at("fingerprint"));
}
#endif

// A copy of test-python-echo's directory serves fx-service as the build left it; with one more
// line in its Python source, the provider's start fails, in a process of its own (`praenora run`):
// LIBRARY_LOAD_FAILED, reason start, and the cause of the Python core, DATA_INVALID, naming the
// copy's python/ directory, the digest the library was built with and the digest of the files.
TEST(PythonEchoProvider, EditedSourcesFailStart) {
    ScratchDir dir;
    const fs::path copy = dir.path() / "providers" / "test-python-echo";
    fs::create_directories(copy.parent_path());
    fs::copy(python_echo_provider(), copy, fs::copy_options::recursive);
    const std::string config =
        write_config(dir.path() / "config", "test.single", "run: { ticks: 1 }\n",
                     step("a", "fx-service", "{ require_echo: true }"));

    const ToolRun as_built = run_tool_json(config, dir.path() / "tool-as-built",
                                           dir.path() / "as-built", echo_tool_paths(copy));
    ASSERT_TRUE(as_built.exited) << as_built.err;
    EXPECT_EQ(as_built.exit_code, 0) << as_built.out << as_built.err;
    EXPECT_EQ(results(dir.path() / "as-built"), (std::vector<json>{echoed_source()}));

    const fs::path source = copy / "python" / "test_python_echo" / "__init__.py";
    ASSERT_TRUE(fs::is_regular_file(source));
    {
        std::ofstream out(source, std::ios::binary | std::ios::app);
        out << "# edited after the build\n";
    }
    const ToolRun edited = run_tool_json(config, dir.path() / "tool-edited", dir.path() / "edited",
                                         echo_tool_paths(copy));
    ASSERT_TRUE(edited.exited) << edited.err;
    EXPECT_EQ(edited.exit_code, 4) << edited.out << edited.err;
    const json document = json_output(edited);
    EXPECT_EQ(document.value("ok", true), false) << document;
    const json& error = document.at("error");
    EXPECT_EQ(error.at("name"), "LIBRARY_LOAD_FAILED") << document;
    EXPECT_EQ(error.at("details").at("reason"), "start") << document;
    const json& cause = error.at("details").at("cause");
    EXPECT_EQ(cause.at("code"), 12) << cause; // PRAENORA_STATUS_DATA_INVALID
    const std::string message = cause.at("message").get<std::string>();
    EXPECT_TRUE(contains(message, "the Python sources below")) << message;
    EXPECT_TRUE(contains(message, "differ from those")) << message;
    const fs::path named = utf8_path(cause.at("details").at("path").get<std::string>());
    EXPECT_TRUE(fs::exists(named) && fs::equivalent(named, copy / "python")) << cause;
    EXPECT_EQ(cause.at("details").at("expected_sha256"), PRAENORA_TEST_PYTHON_ECHO_SOURCES_SHA256);
    EXPECT_NE(cause.at("details").at("actual_sha256"), PRAENORA_TEST_PYTHON_ECHO_SOURCES_SHA256);
}

// One process, one interpreter: a run whose step a (fx-service) uses test-python-echo and whose
// step b (fx-py-loaded, a Python module) uses the provider python, in the tool's process. Both
// providers start, each starting the one Python core (the core's start called twice, and the
// second accepted: each logs the core it started, the same one), and the Python class of step b
// sees the package of test-python-echo's class loaded in its interpreter.
TEST(PythonEchoProvider, SharesInterpreterWithPython) {
    ScratchDir dir;
    const std::string config = write_config(
        dir.path() / "config", "test.chain", "run: { ticks: 1 }\n",
        step("a", "fx-service", "{ require_echo: true }") +
            step("b", "fx-py-loaded", "{ names: [test_python_echo, fx_py_loaded, fx_py_echo] }"));
    const fs::path log = dir.path() / "tool.log";
    std::vector<std::string> arguments = echo_tool_paths(python_echo_provider());
    const std::vector<std::string> logging = {"--log-level", "debug",      "--log-format",
                                              "json",        "--log-file", utf8(log)};
    arguments.insert(arguments.end(), logging.begin(), logging.end());
    const ToolRun run = run_tool_json(config, dir.path() / "tool", dir.path() / "out", arguments);
    ASSERT_TRUE(run.exited) << run.err;
    ASSERT_EQ(run.exit_code, 0) << run.out << run.err;
    EXPECT_EQ(
        results(dir.path() / "out"),
        (std::vector<json>{
            {{"in", echoed_source()},
             {"loaded",
              {{"test_python_echo", true}, {"fx_py_loaded", true}, {"fx_py_echo", false}}}}}));

    const std::string prefix = "the Python core started: ";
    std::map<std::string, std::vector<std::string>> started;
    for (const json& record : log_records(log)) {
        const std::string body = record.value("body", std::string());
        if (body.rfind(prefix, 0) == 0) {
            started[logger_of(record)].push_back(body.substr(prefix.size()));
        }
    }
    ASSERT_EQ(started.size(), 2u) << "the records of " << utf8(log);
    ASSERT_EQ(started["provider.python"].size(), 1u);
    ASSERT_EQ(started["provider.test-python-echo"].size(), 1u);
    EXPECT_EQ(started["provider.python"].front(), started["provider.test-python-echo"].front());
    EXPECT_EQ(started_module_hosts(log), 0u);

    const json services = run_record(dir.path() / "out").at("reproducibility").at("services");
    EXPECT_EQ(services.at("test.echo@0").at("provider").at("name"), "test-python-echo");
    EXPECT_EQ(services.at("praenora.python@0").at("provider").at("name"), "python");
}

#ifdef PRAENORA_TEST_WITH_ROADRUNNER
// The roadrunner provider (libRoadRunner and its LLVM, linked into it) and the environment's
// roadrunner package (which brings a libRoadRunner and an LLVM of its own) in one process: a run
// whose step a (simulating-module) simulates the SBML model decay.xml through the provider, and
// whose step b (fx-py-roadrunner, a Python module) simulates an Antimony model of the same
// equation with the package, both in the runtime's process. Both give x(2) = 10 exp(-1), and the
// run record names both providers.
TEST(PythonRoadrunner, CoexistInOneProcess) {
    ScratchDir dir;
    const fs::path log = dir.path() / "run.log";
    const Runtime runtime(options_logging_to(log));
    const json cvode = {{"method", "cvode"},
                        {"stiffness", "nonstiff"},
                        {"relative_tolerance", 1e-12},
                        {"absolute_tolerance", 1e-12}};
    const json simulation = {
        {"model", read_text(praenora::test::fixture_path("models/sbml/decay.xml"))},
        {"format", "sbml"},
        {"plan",
         {{"parameters", {"k_x"}},
          {"initial_conditions", {"x"}},
          {"time_domain", {{"start", 0}, {"end", 2}}},
          {"output", {{"save_at", "explicit_times"}, {"times", {0, 0.5, 1, 2}}}},
          {"solver", cvode}}},
        {"values", {0.5, 10}}};
    const std::string config = write_config(dir.path(), "test.chain", "run: { ticks: 1 }\n",
                                            step("a", "simulating-module", simulation.dump()) +
                                                step("b", "fx-py-roadrunner"));
    // A source without "values", so that step a takes them from its parameters.
    write_text_file(dir.path() / "source.json", "{\"value\": 1}");
    ASSERT_TRUE(run_to_completion(runtime, config, dir.path() / "out", IsolationMode::in_process));
    EXPECT_EQ(started_module_hosts(log), 0u);

    const std::vector<json> data = results(dir.path() / "out");
    ASSERT_EQ(data.size(), 1u);
    const json& provider = data[0].at("in");
    ASSERT_EQ(provider.at("ok"), true) << provider;
    EXPECT_NEAR(provider.at("rows").at(3).at(1).get<double>(), 10.0 * std::exp(-1.0), 1e-8)
        << provider;
    const json& package = data[0].at("rows");
    ASSERT_EQ(package.size(), 5u) << package;
    EXPECT_EQ(package.at(4).at(0).get<double>(), 2.0) << package;
    EXPECT_NEAR(package.at(4).at(1).get<double>(), 10.0 * std::exp(-1.0), 1e-8) << package;

    const json services = run_record(dir.path() / "out").at("reproducibility").at("services");
    EXPECT_EQ(services.at("praenora.simulation@0").at("provider").at("name"), "roadrunner");
    EXPECT_EQ(services.at("praenora.python@0").at("provider").at("name"), "python");
}
#endif
