// The public C++ API (praenora/runtime/runtime.hpp and workflow.hpp), through the shared library
// only: version facts, validation reports, opening a workflow, listing and inspecting modules, the
// workflow's states, concurrent control calls and closing. The workflows and modules are the test
// catalog's and the fixture modules (tests/fixtures/catalog and tests/fixtures/modules).
#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/runtime.hpp>
#include <praenora/runtime/status.hpp>
#include <praenora/runtime/version.hpp>
#include <praenora/runtime/workflow.hpp>
#include <praenora/sdk/abi.h>
#include <praenora/sdk/version.h>

#include "module_fixtures.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Error;
using praenora::ErrorCode;
using praenora::LogFormat;
using praenora::LogLevel;
using praenora::ModuleInfo;
using praenora::RunSummary;
using praenora::Runtime;
using praenora::RuntimeOptions;
using praenora::Severity;
using praenora::StatusSnapshot;
using praenora::StepState;
using praenora::TickResult;
using praenora::ValidationReport;
using praenora::Workflow;
using praenora::WorkflowOptions;
using praenora::WorkflowState;
using praenora::test::ScratchDir;
using praenora::test::write_text_file;
using Clock = std::chrono::steady_clock;

namespace {

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

// The fixture modules and the test catalog, without the environment and without logging to
// standard error.
RuntimeOptions fixture_options() {
    RuntimeOptions options;
    options.module_paths = {PRAENORA_TEST_MODULES_DIR};
    options.catalog_paths = {PRAENORA_TEST_CATALOG_DIR};
    options.use_environment = false;
    options.logging.to_stderr = false;
    return options;
}

// The praenora::Error that `call` throws; fails the test when it throws none.
template <class Call> Error error_of(Call&& call) {
    try {
        call();
    } catch (const Error& error) {
        return error;
    }
    ADD_FAILURE() << "no praenora::Error was thrown";
    return Error(ErrorCode::OK, "nothing thrown");
}

// A step block binding `id` to fixture module `module` (at `version`) with `parameters`.
std::string step(const std::string& id, const std::string& module,
                 const std::string& parameters = "{}", const std::string& version = "0.1.0") {
    return "  " + id + ":\n    module: { name: " + module + ", version: \"" + version +
           "\" }\n    parameters: " + parameters + "\n";
}

// Writes `<dir>/source.json` and `<dir>/workflow.yaml`, a configuration of the test workflow
// `workflow` with `run` and `steps` (YAML blocks), and returns the configuration's path.
std::string write_config(const fs::path& dir, const std::string& workflow, const std::string& run,
                         const std::string& steps, const std::string& extra = "") {
    write_text_file(dir / "source.json", "{\"value\": 42}");
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

// The error diagnostics of `report`, in order.
std::vector<praenora::Diagnostic> errors_of(const ValidationReport& report) {
    std::vector<praenora::Diagnostic> errors;
    for (const auto& diagnostic : report.diagnostics) {
        if (diagnostic.severity == Severity::error) {
            errors.push_back(diagnostic);
        }
    }
    return errors;
}

bool has_code(const ValidationReport& report, ErrorCode code) {
    const auto errors = errors_of(report);
    return std::any_of(errors.begin(), errors.end(),
                       [code](const praenora::Diagnostic& d) { return d.code == code; });
}

const praenora::StepStatus& step_status(const StatusSnapshot& status, const std::string& id) {
    for (const auto& step : status.steps) {
        if (step.step == id) {
            return step;
        }
    }
    throw std::runtime_error("no step " + id + " in the status");
}

// Sets (or, with nullopt, removes) an environment variable for the object's lifetime.
class ScopedEnvironment {
public:
    ScopedEnvironment(const char* name, const std::optional<std::string>& value) : name_(name) {
#ifdef _MSC_VER
#pragma warning(suppress : 4996) // getenv is fine here: the tests change it one at a time
#endif
        if (const char* previous = std::getenv(name)) {
            previous_ = previous;
        }
        set(value);
    }
    ~ScopedEnvironment() { set(previous_); }
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
    void set(const std::optional<std::string>& value) const {
#ifdef _WIN32
        _putenv_s(name_, value ? value->c_str() : "");
#else
        if (value) {
            setenv(name_, value->c_str(), 1);
        } else {
            unsetenv(name_);
        }
#endif
    }

    const char* name_;
    std::optional<std::string> previous_;
};

std::string read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The events of the engine's own logger (praenora.engine) in a log file of JSON lines, as
// "<SEVERITY> <message>"; "text" when the file holds a line that is not JSON.
std::vector<std::string> engine_events(const fs::path& file) {
    std::vector<std::string> events;
    std::istringstream lines(read_text(file));
    for (std::string line; std::getline(lines, line);) {
        const json event = json::parse(line, nullptr, false);
        if (event.is_discarded()) {
            return {"text"};
        }
        if (event.at("attributes").at("logger") == "praenora.engine") {
            events.push_back(event.at("severity_text").get<std::string>() + " " +
                             event.at("body").get<std::string>());
        }
    }
    return events;
}

// Every file below `dir`, by its path relative to `dir`, with its contents.
std::map<std::string, std::string> directory_contents(const fs::path& dir) {
    std::map<std::string, std::string> contents;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            contents[utf8(entry.path().lexically_relative(dir))] = read_text(entry.path());
        }
    }
    return contents;
}

bool has_severity(const std::vector<std::string>& events, const std::string& severity) {
    return std::any_of(events.begin(), events.end(), [&severity](const std::string& event) {
        return event.compare(0, severity.size() + 1, severity + " ") == 0;
    });
}

} // namespace

TEST(Api, VersionInfoMatchesBuild) {
    const praenora::VersionInfo info = praenora::version_info();
    EXPECT_EQ(info.runtime, PRAENORA_TEST_RUNTIME_VERSION);
    EXPECT_EQ(info.sdk, PRAENORA_SDK_VERSION_STRING);
    EXPECT_EQ(info.pmp, PRAENORA_PMP_VERSION_STRING);
    EXPECT_EQ(info.abi_major, PRAENORA_ABI_MAJOR);
    EXPECT_EQ(info.abi_minor, PRAENORA_ABI_MINOR);
    EXPECT_EQ(info.platform, PRAENORA_TEST_PLATFORM);
    EXPECT_EQ(info.build_type, PRAENORA_TEST_BUILD_TYPE);
    EXPECT_FALSE(info.compiler.empty());
    EXPECT_EQ(info.formats.size(), 9u);
    for (const auto& [kind, version] : info.formats) {
        EXPECT_EQ(version, PRAENORA_TEST_FORMAT_VERSION) << kind;
    }

    // The JSON form says the same.
    const json document = json::parse(praenora::version_info_json());
    EXPECT_EQ(document.at("runtime"), info.runtime);
    EXPECT_EQ(document.at("sdk"), info.sdk);
    EXPECT_EQ(document.at("pmp"), info.pmp);
    EXPECT_EQ(document.at("abi"), json({{"major", info.abi_major}, {"minor", info.abi_minor}}));
    EXPECT_EQ(document.at("formats"), json(info.formats));
    EXPECT_EQ(document.at("platform"), info.platform);
    EXPECT_EQ(document.at("compiler"), info.compiler);
    EXPECT_EQ(document.at("build_type"), info.build_type);
}

TEST(Api, ValidateReportsDiagnosticsWithoutThrowing) {
    ScratchDir dir;
    const Runtime runtime(fixture_options());

    // Three problems at once: a module version nobody has, a step the workflow lacks and a data
    // file that does not exist. Each is reported, located in the configuration.
    const fs::path bad = dir.path() / "bad" / "workflow.yaml";
    write_text_file(bad, "kind: workflow-config\n"
                         "format_version: \"0.1.0\"\n"
                         "workflow: { name: test.single, version: \"0.1.0\" }\n"
                         "run: { ticks: 2 }\n"
                         "inputs:\n"
                         "  source: { source: json-document, path: missing.json }\n"
                         "steps:\n" +
                             step("a", "fx-echo", "{}", "9.9.9") + step("z", "fx-echo"));
    ValidationReport report;
    EXPECT_NO_THROW(report = runtime.validate(utf8(bad)));
    EXPECT_FALSE(report.ok());
    EXPECT_TRUE(has_code(report, ErrorCode::MODULE_NOT_FOUND)) << report.to_text();
    EXPECT_TRUE(has_code(report, ErrorCode::BINDING_INVALID)) << report.to_text();
    EXPECT_TRUE(has_code(report, ErrorCode::TARGET_DATA_INVALID)) << report.to_text();
    for (const auto& error : errors_of(report)) {
        EXPECT_TRUE(contains(error.location.file, "workflow.yaml")) << report.to_text();
        EXPECT_GT(error.location.line, 0) << report.to_text();
    }
    EXPECT_TRUE(contains(report.to_text(), "error[BINDING_INVALID]")) << report.to_text();
    EXPECT_EQ(json::parse(report.to_json()).at("ok"), false);

    // A file that does not exist is a report too.
    report = runtime.validate(utf8(dir.path() / "none.yaml"));
    ASSERT_FALSE(report.ok());
    EXPECT_EQ(errors_of(report).front().code, ErrorCode::IO_ERROR) << report.to_text();

    // A valid configuration passes, with and without loading its modules.
    const std::string good = write_config(dir.path() / "good", "test.single", "run: { ticks: 2 }\n",
                                          step("a", "fx-echo"));
    report = runtime.validate(good);
    EXPECT_TRUE(report.ok()) << report.to_text();
    EXPECT_TRUE(errors_of(report).empty()) << report.to_text();
    report = runtime.validate(good, {true});
    EXPECT_TRUE(report.ok()) << report.to_text();

    // Loading finds what the manifest alone cannot show: fx-identity-mismatch's describe() reports
    // another version than its manifest.
    const std::string mismatch =
        write_config(dir.path() / "mismatch", "test.single", "run: { ticks: 2 }\n",
                     step("a", "fx-identity-mismatch"));
    report = runtime.validate(mismatch);
    EXPECT_TRUE(report.ok()) << report.to_text();
    report = runtime.validate(mismatch, {true});
    ASSERT_FALSE(report.ok());
    const auto errors = errors_of(report);
    ASSERT_EQ(errors.size(), 1u) << report.to_text();
    EXPECT_EQ(errors.front().code, ErrorCode::MODULE_IDENTITY_MISMATCH) << report.to_text();
    EXPECT_EQ(errors.front().step, "a");
    EXPECT_EQ(errors.front().module, "fx-identity-mismatch@0.1.0");
    EXPECT_TRUE(contains(errors.front().location.file, "fx-identity-mismatch.pmm.json"))
        << report.to_text();

    // validate_document() reports the same way, for any document: by its kind, or for the module
    // ABI's documents, which have none, by the fields they require.
    const auto document = [&](const std::string& name, const std::string& text) {
        write_text_file(dir.path() / "documents" / name, text);
        return runtime.validate_document(utf8(dir.path() / "documents" / name));
    };
    const auto first_error = [](const ValidationReport& r) {
        const auto found = errors_of(r);
        return found.empty() ? ErrorCode::OK : found.front().code;
    };
    report = runtime.validate_document(utf8(praenora::test::fixture_manifest("fx-echo")));
    EXPECT_TRUE(report.ok()) << report.to_text();
    report = document("result.json", R"({"outputs": {"out": {"data": {}}}})");
    EXPECT_TRUE(report.ok()) << report.to_text(); // pmp-result
    report = document("error.json", R"({"code": 11})");
    EXPECT_EQ(first_error(report), ErrorCode::DOCUMENT_KIND_MISMATCH) << report.to_text();
    report = document("bad-result.json", R"({"outputs": {"out": {}}, "pmp_version": "0.1.0"})");
    EXPECT_EQ(first_error(report), ErrorCode::SCHEMA_VIOLATION) << report.to_text();
    EXPECT_EQ(errors_of(report).front().location.pointer, "/outputs/out") << report.to_text();
    report = document("unknown.yaml", "kind: recipe\nformat_version: \"0.1.0\"\n");
    EXPECT_EQ(first_error(report), ErrorCode::DOCUMENT_KIND_MISMATCH) << report.to_text();
    EXPECT_EQ(errors_of(report).front().location.pointer, "/kind") << report.to_text();
    EXPECT_EQ(errors_of(report).front().location.line, 1) << report.to_text();
    report = document("future.json", R"({"kind": "status-snapshot", "format_version": "7.0.0"})");
    EXPECT_EQ(first_error(report), ErrorCode::FORMAT_VERSION_UNSUPPORTED) << report.to_text();
    report = document("broken.json", "{\"kind\": ");
    EXPECT_EQ(first_error(report), ErrorCode::PARSE_ERROR) << report.to_text();
    report = runtime.validate_document(utf8(dir.path() / "documents" / "none.json"));
    EXPECT_EQ(first_error(report), ErrorCode::IO_ERROR) << report.to_text();
}

TEST(Api, OpenThrowsWithReportInDetails) {
    ScratchDir dir;
    const Runtime runtime(fixture_options());
    const std::string file =
        write_config(dir.path(), "test.single", "run: { ticks: 2 }\n",
                     step("a", "fx-echo", "{}", "9.9.9") + step("z", "fx-echo"));
    const ValidationReport report = runtime.validate(file);
    const auto errors = errors_of(report);
    ASSERT_GE(errors.size(), 2u) << report.to_text();

    const Error error = error_of([&] { runtime.open(file); });
    EXPECT_EQ(error.code(), errors.front().code);
    EXPECT_EQ(std::string(error.what()),
              "workflow validation failed: " + std::to_string(errors.size()) + " error(s)");
    EXPECT_EQ(error.location().file, errors.front().location.file);
    EXPECT_EQ(error.location().line, errors.front().location.line);
    EXPECT_EQ(error.location().column, errors.front().location.column);
    EXPECT_EQ(error.location().pointer, errors.front().location.pointer);
    EXPECT_EQ(json::parse(error.details_json()), json::parse(report.to_json()));
}

TEST(Api, ListAndInspectModules) {
    const Runtime runtime(fixture_options());
    const std::vector<ModuleInfo> modules = runtime.list_modules();
    const auto echo = std::find_if(modules.begin(), modules.end(),
                                   [](const ModuleInfo& m) { return m.name == "fx-echo"; });
    ASSERT_NE(echo, modules.end());
    EXPECT_GE(modules.size(), 10u);
    EXPECT_EQ(echo->version, "0.1.0");
    EXPECT_EQ(fs::path(echo->manifest_path), praenora::test::fixture_manifest("fx-echo"));
    EXPECT_TRUE(fs::is_regular_file(praenora::test::utf8_path(echo->library_path)))
        << echo->library_path;
    EXPECT_EQ(echo->os + "-" + echo->arch, PRAENORA_TEST_PLATFORM);
    EXPECT_EQ(echo->capabilities, std::vector<std::string>{"test.echo@0.1.0"});
    EXPECT_TRUE(echo->describe_json.empty()); // nothing is loaded to list modules
    EXPECT_TRUE(echo->library_sha256.empty());

    // Inspecting without loading tells what the manifest tells; the version defaults to the
    // highest one there is.
    ModuleInfo info = runtime.inspect_module("fx-echo");
    EXPECT_EQ(info.name, "fx-echo");
    EXPECT_EQ(info.version, "0.1.0");
    EXPECT_EQ(info.manifest_path, echo->manifest_path);
    EXPECT_TRUE(info.describe_json.empty());

    // Loading adds what the library itself reports.
    info = runtime.inspect_module("fx-echo", std::string("0.1.0"), true);
    const json describe = json::parse(info.describe_json);
    EXPECT_EQ(describe.at("name"), "fx-echo");
    EXPECT_EQ(describe.at("version"), "0.1.0");
    EXPECT_EQ(describe.at("capabilities"),
              json::array({{{"id", "test.echo"}, {"version", "0.1.0"}}}));
    ASSERT_EQ(info.library_sha256.size(), 64u);
    EXPECT_EQ(info.library_sha256.find_first_not_of("0123456789abcdef"), std::string::npos);

    EXPECT_EQ(error_of([&] { runtime.inspect_module("fx-nowhere"); }).code(),
              ErrorCode::MODULE_NOT_FOUND);
    EXPECT_EQ(error_of([&] { runtime.inspect_module("fx-echo", std::string("9.9.9")); }).code(),
              ErrorCode::MODULE_NOT_FOUND);
    EXPECT_EQ(error_of([&] { runtime.inspect_module("fx-echo", std::string("one")); }).code(),
              ErrorCode::INVALID_ARGUMENT);
    // The manifest is fine; the library's describe() is not.
    EXPECT_EQ(runtime.inspect_module("fx-identity-mismatch").version, "0.1.0");
    EXPECT_EQ(error_of([&] { runtime.inspect_module("fx-identity-mismatch", {}, true); }).code(),
              ErrorCode::MODULE_IDENTITY_MISMATCH);
}

TEST(Api, CatalogAccessors) {
    const Runtime runtime(fixture_options());
    const std::vector<praenora::CatalogEntryInfo> entries = runtime.list_catalog();
    // Every entry, built-in and from the test catalog, in (kind, id, version) order.
    EXPECT_TRUE(std::is_sorted(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return std::tie(a.kind, a.id, a.version) < std::tie(b.kind, b.id, b.version);
    }));
    const auto find = [&entries](const std::string& kind, const std::string& id) {
        return std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
            return entry.kind == kind && entry.id == id;
        });
    };
    const auto example = find("workflow-definition", "example.estimate-predict");
    ASSERT_NE(example, entries.end());
    EXPECT_EQ(example->version, "0.1.0");
    EXPECT_EQ(example->status, "stable");
    const auto blob = find("message-type", "test.blob");
    ASSERT_NE(blob, entries.end());
    EXPECT_EQ(blob->summary, "Any JSON object (tests).");
    EXPECT_NE(find("capability-definition", "test.echo"), entries.end());

    // An entry is its canonical JSON.
    const std::string text = runtime.catalog_entry("example.estimate-predict", "0.1.0");
    const json written =
        json::parse(read_text(praenora::test::utf8_path(PRAENORA_TEST_SOURCE_DIR) /
                              "spec/catalog/workflows/example.estimate-predict/0.1.0.json"));
    EXPECT_EQ(json::parse(text), written);
    EXPECT_EQ(text, written.dump());

    EXPECT_EQ(error_of([&] { runtime.catalog_entry("example.nothing", "0.1.0"); }).code(),
              ErrorCode::CATALOG_ENTRY_NOT_FOUND);
    EXPECT_EQ(error_of([&] { runtime.catalog_entry("example.estimate-predict", "9.0.0"); }).code(),
              ErrorCode::CATALOG_ENTRY_NOT_FOUND);
    EXPECT_EQ(error_of([&] { runtime.catalog_entry("example.estimate-predict", "one"); }).code(),
              ErrorCode::INVALID_ARGUMENT);

    // An id@version that two kinds define cannot name one entry.
    RuntimeOptions shared = fixture_options();
    shared.catalog_paths.push_back(utf8(praenora::test::fixture_path("catalog-shared-id")));
    const Runtime both(shared);
    const Error ambiguous = error_of([&] { both.catalog_entry("test.shared", "0.1.0"); });
    EXPECT_EQ(ambiguous.code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_TRUE(contains(ambiguous.what(), "message-type")) << ambiguous.what();
    EXPECT_TRUE(contains(ambiguous.what(), "capability-definition")) << ambiguous.what();
}

TEST(Api, SchemaAccessors) {
    const Runtime runtime(fixture_options());
    const std::vector<std::string> schemas = runtime.list_schemas();
    EXPECT_EQ(schemas.size(), 16u);
    EXPECT_TRUE(std::is_sorted(schemas.begin(), schemas.end()));
    for (const char* expected : {"module-manifest@0.1", "pmp-envelope@0.1", "workflow-config@0.1",
                                 "status-snapshot@0.1"}) {
        EXPECT_NE(std::find(schemas.begin(), schemas.end(), expected), schemas.end()) << expected;
    }

    // The embedded text, the same for the highest version, major.minor and a full version.
    const std::string file = read_text(praenora::test::utf8_path(PRAENORA_TEST_SOURCE_DIR) /
                                       "spec/schemas/module-manifest.0.1.schema.json");
    EXPECT_EQ(runtime.schema("module-manifest"), file);
    EXPECT_EQ(runtime.schema("module-manifest", std::string("0.1")), file);
    EXPECT_EQ(runtime.schema("module-manifest", std::string("0.1.0")), file);
    EXPECT_EQ(runtime.schema("module-manifest", std::string("0.1.7")), file);

    EXPECT_EQ(error_of([&] { runtime.schema("module-manifest", std::string("0.2")); }).code(),
              ErrorCode::FORMAT_VERSION_UNSUPPORTED);
    EXPECT_EQ(error_of([&] { runtime.schema("no-such-kind"); }).code(),
              ErrorCode::FORMAT_VERSION_UNSUPPORTED);
    EXPECT_EQ(error_of([&] { runtime.schema("module-manifest", std::string("zero")); }).code(),
              ErrorCode::INVALID_ARGUMENT);
}

TEST(Api, GraphOfAWorkflowOrAConfiguration) {
    ScratchDir dir;
    const Runtime runtime(fixture_options());
    praenora::GraphRequest by_name;
    by_name.workflow = "test.single@0.1.0";
    const std::string text = runtime.graph(by_name);
    EXPECT_TRUE(contains(text, "schedule: a\n")) << text;
    EXPECT_TRUE(contains(text, "  inputs.source -> a.in\n")) << text;

    // A configuration names its workflow; only that part of it matters.
    praenora::GraphRequest by_config;
    by_config.config_path =
        write_config(dir.path(), "test.single", "run: { ticks: 1 }\n", step("a", "fx-nowhere"));
    by_config.format = praenora::GraphFormat::json;
    const json graph = json::parse(runtime.graph(by_config));
    EXPECT_EQ(graph.at("workflow").at("name"), "test.single");
    EXPECT_EQ(graph.at("schedule"), json::array({"a"}));

    praenora::GraphRequest both = by_name;
    both.config_path = by_config.config_path;
    EXPECT_EQ(error_of([&] { runtime.graph(both); }).code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_EQ(error_of([&] { runtime.graph(praenora::GraphRequest{}); }).code(),
              ErrorCode::INVALID_ARGUMENT);
    praenora::GraphRequest bad = by_name;
    bad.workflow = "test.single";
    EXPECT_EQ(error_of([&] { runtime.graph(bad); }).code(), ErrorCode::INVALID_ARGUMENT);
    bad.workflow = "test.nothing@0.1.0";
    EXPECT_EQ(error_of([&] { runtime.graph(bad); }).code(), ErrorCode::WORKFLOW_NOT_FOUND);

    // An invalid definition: its problems are located in its catalog file.
    RuntimeOptions broken_options = fixture_options();
    broken_options.catalog_paths.push_back(
        utf8(praenora::test::fixture_path("catalog-bad/unknown-port")));
    const Runtime broken(broken_options);
    bad.workflow = "bad.unknown-port@0.1.0";
    const Error invalid = error_of([&] { broken.graph(bad); });
    EXPECT_EQ(invalid.code(), ErrorCode::PORT_UNKNOWN);
    EXPECT_EQ(fs::path(praenora::test::utf8_path(invalid.location().file)),
              praenora::test::fixture_path(
                  "catalog-bad/unknown-port/workflows/bad.unknown-port/0.1.0.json"));
    EXPECT_GT(invalid.location().line, 0);
    const json details = json::parse(invalid.details_json());
    ASSERT_EQ(details.at("diagnostics").size(), 1u);
    EXPECT_EQ(details.at("diagnostics")[0].at("name"), "PORT_UNKNOWN");
}

TEST(Api, WorkflowNamesItsOutputDirectory) {
    ScratchDir dir;
    const Runtime runtime(fixture_options());
    const std::string config =
        write_config(dir.path(), "test.single", "run: { ticks: 1 }\n", step("a", "fx-echo"));
    EXPECT_EQ(runtime.open(config)->output_dir(), ""); // the configuration names none

    WorkflowOptions options;
    options.output_dir = utf8(dir.path() / "out");
    const auto workflow = runtime.open(config, options);
    EXPECT_EQ(fs::path(praenora::test::utf8_path(workflow->output_dir())), dir.path() / "out");
}

TEST(Api, WorkflowStateTransitions) {
    ScratchDir dir;
    const Runtime runtime(fixture_options());
    const std::string file =
        write_config(dir.path(), "test.single", "run: { ticks: 2 }\n", step("a", "fx-echo"));
    const std::unique_ptr<Workflow> workflow = runtime.open(file);
    ASSERT_NE(workflow, nullptr);
    EXPECT_EQ(workflow->state(), WorkflowState::created);
    EXPECT_EQ(workflow->fingerprint().size(), 64u);
    EXPECT_EQ(workflow->run_record_json(), "null");
    EXPECT_EQ(workflow->latest_outputs_json(), "{}");
    EXPECT_EQ(error_of([&] { workflow->tick(); }).code(), ErrorCode::INVALID_STATE);

    workflow->start();
    EXPECT_EQ(workflow->state(), WorkflowState::ready);
    EXPECT_EQ(error_of([&] { workflow->start(); }).code(), ErrorCode::INVALID_STATE);

    const TickResult first = workflow->tick();
    EXPECT_EQ(workflow->state(), WorkflowState::running);
    EXPECT_EQ(first.tick, 0u);
    EXPECT_EQ(first.logical_time, 1.0);
    EXPECT_EQ(first.outputs, std::vector<std::string>{"result"});
    EXPECT_FALSE(first.finished);
    EXPECT_EQ(json::parse(workflow->latest_outputs_json()).at("result").at("data"),
              json({{"value", 42}}));

    const TickResult second = workflow->tick();
    EXPECT_EQ(second.tick, 1u);
    EXPECT_TRUE(second.finished);
    EXPECT_EQ(workflow->state(), WorkflowState::completed);
    EXPECT_EQ(error_of([&] { workflow->tick(); }).code(), ErrorCode::INVALID_STATE);

    // On a finished run, run() returns the summary at once.
    const RunSummary summary = workflow->run();
    EXPECT_EQ(summary.final_state, WorkflowState::completed);
    EXPECT_EQ(summary.ticks_completed, 2u);
    EXPECT_EQ(summary.fingerprint, workflow->fingerprint());
    EXPECT_FALSE(summary.error.has_value());

    const StatusSnapshot status = workflow->status();
    EXPECT_EQ(status.state, WorkflowState::completed);
    EXPECT_EQ(status.ticks_completed, 2u);
    EXPECT_EQ(step_status(status, "a").invocations, 2u);
    EXPECT_EQ(step_status(status, "a").state, StepState::destroyed);
    const json status_document = json::parse(workflow->status_json());
    EXPECT_EQ(status_document.at("kind"), "status-snapshot");
    EXPECT_EQ(status_document.at("workflow").at("state"), "completed");
    EXPECT_TRUE(json::parse(workflow->metrics_json()).at("metrics").is_array());
    const json record = json::parse(workflow->run_record_json());
    EXPECT_EQ(record.at("kind"), "run-record");
    EXPECT_EQ(record.at("fingerprint"), workflow->fingerprint());
    EXPECT_EQ(record.at("execution").at("status"), "completed");

    // run() starts a workflow that has not started yet.
    const std::unique_ptr<Workflow> other = runtime.open(file);
    const RunSummary ran = other->run();
    EXPECT_EQ(ran.final_state, WorkflowState::completed);
    EXPECT_EQ(ran.ticks_completed, 2u);
    EXPECT_EQ(ran.fingerprint, workflow->fingerprint());
}

TEST(Api, RunThrowsTheFailureOfAFailedRun) {
    // run() returns normally only for completed and stopped runs: on a run that failed in an
    // earlier start() or tick(), it throws that failure again, with its code, message, location
    // and details.
    ScratchDir dir;
    const Runtime runtime(fixture_options());
    const auto check = [](Workflow& workflow, const Error& failure) {
        EXPECT_EQ(workflow.state(), WorkflowState::failed);
        // start() on a run that is over is a refusal, never the run's failure.
        const Error refused = error_of([&] { workflow.start(); });
        EXPECT_EQ(refused.code(), ErrorCode::INVALID_STATE) << refused.what();
        EXPECT_EQ(error_of([&] { workflow.tick(); }).code(), ErrorCode::INVALID_STATE);
        const Error again = error_of([&] { workflow.run(); });
        EXPECT_EQ(again.code(), failure.code());
        EXPECT_EQ(std::string(again.what()), failure.what());
        EXPECT_EQ(again.location().file, failure.location().file);
        EXPECT_EQ(again.location().pointer, failure.location().pointer);
        EXPECT_EQ(json::parse(again.details_json()), json::parse(failure.details_json()));
        EXPECT_EQ(error_of([&] { workflow.run(); }).code(), failure.code()); // every time
    };

    // fx-fail fails its second invocation.
    const std::unique_ptr<Workflow> in_tick =
        runtime.open(write_config(dir.path() / "tick", "test.single", "run: { ticks: 3 }\n",
                                  step("a", "fx-fail", "{ at_invocation: 2 }")));
    in_tick->start();
    EXPECT_FALSE(in_tick->tick().finished);
    const Error tick_failure = error_of([&] { in_tick->tick(); });
    EXPECT_EQ(tick_failure.code(), ErrorCode::STEP_FAILED);
    EXPECT_EQ(json::parse(tick_failure.details_json()).at("tick"), 1);
    check(*in_tick, tick_failure);

    // fx-fail fails its create.
    const std::unique_ptr<Workflow> in_start =
        runtime.open(write_config(dir.path() / "start", "test.single", "run: { ticks: 3 }\n",
                                  step("a", "fx-fail", "{ phase: create }")));
    const Error start_failure = error_of([&] { in_start->start(); });
    EXPECT_EQ(start_failure.code(), ErrorCode::STEP_FAILED);
    EXPECT_EQ(json::parse(start_failure.details_json()).at("phase"), "create");
    check(*in_start, start_failure);
}

TEST(Api, FailedOpenLeavesTheEarlierRunAlone) {
    // With overwrite, open() removes an earlier run's files only once everything else it does
    // that can fail has succeeded; a refused open changes nothing in the directory.
    ScratchDir dir;
    const Runtime runtime(fixture_options());
    const std::string file = write_config(dir.path(), "test.single", "run: { ticks: 1 }\n",
                                          step("a", "fx-echo"), "outputs: { directory: out }\n");
    ASSERT_EQ(runtime.open(file)->run().final_state, WorkflowState::completed);
    const fs::path out = dir.path() / "out";
    const std::string record = read_text(out / "run-record.json");
    const std::string outputs = read_text(out / "outputs" / "result.jsonl");
    ASSERT_FALSE(record.empty());
    ASSERT_FALSE(outputs.empty());

    // A log file that cannot be opened (its directory would be a regular file) fails the open
    // before the earlier run is removed.
    write_text_file(dir.path() / "blocker", "a file, not a directory\n");
    WorkflowOptions overwrite;
    overwrite.overwrite = true;
    overwrite.log_file = utf8(dir.path() / "blocker" / "run.log");
    EXPECT_EQ(error_of([&] { runtime.open(file, overwrite); }).code(), ErrorCode::IO_ERROR);
    EXPECT_EQ(read_text(out / "run-record.json"), record);
    EXPECT_EQ(read_text(out / "outputs" / "result.jsonl"), outputs);

    // A log file at or under a path the run manages in its output directory (outputs/, trace/,
    // status.json, run-record.json) would be removed by an overwrite while it is open: it is
    // refused, given for the run or by the configuration, before anything is created or removed.
    WorkflowOptions inside = overwrite;
    inside.log_file = utf8(out / "outputs" / "run.log");
    Error managed = error_of([&] { runtime.open(file, inside); });
    EXPECT_EQ(managed.code(), ErrorCode::CONFIG_INVALID) << managed.to_json();
    EXPECT_TRUE(contains(managed.what(), *inside.log_file)) << managed.what();
    EXPECT_FALSE(fs::exists(out / "outputs" / "run.log"));
    EXPECT_EQ(read_text(out / "run-record.json"), record);
    EXPECT_EQ(read_text(out / "outputs" / "result.jsonl"), outputs);
    const std::string configured = write_config(
        dir.path() / "configured", "test.single", "run: { ticks: 1 }\n", step("a", "fx-echo"),
        "outputs: { directory: ../out }\n"
        "observability: { log_file: ../out/trace/run.log }\n");
    WorkflowOptions plain_overwrite;
    plain_overwrite.overwrite = true;
    managed = error_of([&] { runtime.open(configured, plain_overwrite); });
    EXPECT_EQ(managed.code(), ErrorCode::CONFIG_INVALID) << managed.to_json();
    EXPECT_EQ(managed.location().pointer, "/observability/log_file") << managed.to_json();
    EXPECT_TRUE(contains(managed.location().file, "workflow.yaml")) << managed.to_json();
    EXPECT_FALSE(fs::exists(out / "trace"));
    EXPECT_EQ(read_text(out / "run-record.json"), record);
    EXPECT_EQ(read_text(out / "outputs" / "result.jsonl"), outputs);

    // The runtime's own log file counts as well: as the run's log file (no override, none in the
    // configuration), and beside a run log file of its own elsewhere. The runtime opens its file
    // when it is constructed; the refused opens then create and remove nothing.
    {
        RuntimeOptions logging = fixture_options();
        logging.logging.file = utf8(out / "outputs" / "runtime.log");
        const Runtime logging_runtime(logging);
        const auto before = directory_contents(out);
        ASSERT_EQ(before.count("outputs/runtime.log"), 1u);
        WorkflowOptions as_run_log;
        as_run_log.overwrite = true;
        managed = error_of([&] { logging_runtime.open(file, as_run_log); });
        EXPECT_EQ(managed.code(), ErrorCode::CONFIG_INVALID) << managed.to_json();
        EXPECT_TRUE(contains(managed.what(), logging.logging.file)) << managed.what();
        WorkflowOptions beside = as_run_log;
        beside.log_file = utf8(dir.path() / "beside.log");
        managed = error_of([&] { logging_runtime.open(file, beside); });
        EXPECT_EQ(managed.code(), ErrorCode::CONFIG_INVALID) << managed.to_json();
        EXPECT_TRUE(contains(managed.what(), "the runtime's log file")) << managed.what();
        EXPECT_FALSE(fs::exists(dir.path() / "beside.log"));
        EXPECT_EQ(directory_contents(out), before);
        EXPECT_EQ(read_text(out / "run-record.json"), record);
        EXPECT_EQ(read_text(out / "outputs" / "result.jsonl"), outputs);
    }

    // Without overwrite the earlier run is refused before anything is written, a log file inside
    // the directory included.
    WorkflowOptions refused;
    refused.log_file = utf8(out / "logs" / "run.log");
    const Error error = error_of([&] { runtime.open(file, refused); });
    EXPECT_EQ(error.code(), ErrorCode::IO_ERROR);
    EXPECT_TRUE(contains(error.what(), "already contains a run record")) << error.what();
    EXPECT_FALSE(fs::exists(out / "logs"));
    EXPECT_EQ(read_text(out / "run-record.json"), record);

    // With a usable log file the overwrite goes ahead: a new run, the log file kept.
    overwrite.log_file = refused.log_file;
    ASSERT_EQ(runtime.open(file, overwrite)->run().final_state, WorkflowState::completed);
    EXPECT_NE(read_text(out / "run-record.json"), record);
    EXPECT_FALSE(read_text(out / "logs" / "run.log").empty());
}

TEST(Api, ManagedPathLogFileRefusedInAnyCase) {
    // On a case-insensitive file system OUTPUTS/ is outputs/, which an overwrite removes: a log
    // file there is refused like one in outputs/.
    ScratchDir dir;
    write_text_file(dir.path() / "probe.txt", "probe\n");
    if (!fs::exists(dir.path() / "PROBE.TXT")) {
        GTEST_SKIP() << "the file system under " << utf8(dir.path()) << " is case-sensitive";
    }
    const Runtime runtime(fixture_options());
    const std::string file = write_config(dir.path(), "test.single", "run: { ticks: 1 }\n",
                                          step("a", "fx-echo"), "outputs: { directory: out }\n");
    ASSERT_EQ(runtime.open(file)->run().final_state, WorkflowState::completed);
    const fs::path out = dir.path() / "out";
    const auto before = directory_contents(out);

    WorkflowOptions overwrite;
    overwrite.overwrite = true;
    overwrite.log_file = utf8(out / "OUTPUTS" / "run.log");
    const Error error = error_of([&] { runtime.open(file, overwrite); });
    EXPECT_EQ(error.code(), ErrorCode::CONFIG_INVALID) << error.to_json();
    EXPECT_TRUE(contains(error.what(), *overwrite.log_file)) << error.what();
    EXPECT_EQ(directory_contents(out), before); // nothing created, nothing removed
}

TEST(Api, ManagedPathLogFileRefusedThroughAHardLink) {
    // A log file that is another name for a file the run manages (a hard link to the earlier
    // run's run-record.json) is refused too: logging into it would write into that file.
    ScratchDir dir;
    const Runtime runtime(fixture_options());
    const std::string file = write_config(dir.path(), "test.single", "run: { ticks: 1 }\n",
                                          step("a", "fx-echo"), "outputs: { directory: out }\n");
    ASSERT_EQ(runtime.open(file)->run().final_state, WorkflowState::completed);
    const fs::path out = dir.path() / "out";
    const fs::path alias = dir.path() / "alias.log";
    std::error_code ec;
    fs::create_hard_link(out / "run-record.json", alias, ec);
    if (ec) {
        GTEST_SKIP() << "no hard link could be made under " << utf8(dir.path()) << ": "
                     << ec.message();
    }
    const auto before = directory_contents(out);

    WorkflowOptions overwrite;
    overwrite.overwrite = true;
    overwrite.log_file = utf8(alias);
    const Error error = error_of([&] { runtime.open(file, overwrite); });
    EXPECT_EQ(error.code(), ErrorCode::CONFIG_INVALID) << error.to_json();
    EXPECT_TRUE(contains(error.what(), *overwrite.log_file)) << error.what();
    EXPECT_EQ(directory_contents(out), before); // nothing written, nothing removed
}

TEST(Api, StopRequestedBeforeTheRunCallsNoModule) {
    // fx-lifecycle logs "lifecycle:<call>" for every call it receives; the run's log file, at
    // trace level, shows which calls it got.
    ScratchDir dir;
    RuntimeOptions options = fixture_options();
    options.logging.level = LogLevel::trace;
    options.logging.format = LogFormat::json;
    options.logging.file = utf8(dir.path() / "run.log");
    const Runtime runtime(options);
    const std::string file =
        write_config(dir.path(), "test.single", "run: { ticks: 2 }\n", step("a", "fx-lifecycle"));
    const auto module_calls = [&] {
        std::vector<std::string> calls;
        std::istringstream lines(read_text(dir.path() / "run.log"));
        for (std::string line; std::getline(lines, line);) {
            const std::string body = json::parse(line).at("body").get<std::string>();
            if (body.compare(0, 10, "lifecycle:") == 0) {
                calls.push_back(body.substr(10));
            }
        }
        return calls;
    };

    // Before start(): start() ends the run as stopped, loading and calling nothing.
    const std::unique_ptr<Workflow> before_start = runtime.open(file);
    before_start->request_stop();
    EXPECT_NO_THROW(before_start->start());
    EXPECT_EQ(before_start->state(), WorkflowState::stopped);
    EXPECT_EQ(step_status(before_start->status(), "a").state, StepState::loaded);

    // Before run(): run() starts the run, which ends as stopped at once.
    const std::unique_ptr<Workflow> before_run = runtime.open(file);
    before_run->request_stop();
    RunSummary summary;
    EXPECT_NO_THROW(summary = before_run->run());
    EXPECT_EQ(summary.final_state, WorkflowState::stopped);
    EXPECT_EQ(summary.ticks_completed, 0u);
    ASSERT_TRUE(summary.error.has_value());
    EXPECT_EQ(summary.error->code, ErrorCode::RUN_STOPPED);
    EXPECT_EQ(step_status(before_run->status(), "a").invocations, 0u);
    EXPECT_EQ(module_calls(), std::vector<std::string>{});

    // The same run without a stop calls the module as usual, so the log does show its calls.
    EXPECT_EQ(runtime.open(file)->run().final_state, WorkflowState::completed);
    EXPECT_EQ(module_calls(), (std::vector<std::string>{"create", "initialize", "invoke", "invoke",
                                                        "finalize", "destroy"}));
}

TEST(Api, ConcurrentControlCallsRejected) {
    ScratchDir dir;
    const Runtime runtime(fixture_options());
    // b sleeps 5 s in each invocation, polling for cancellation.
    const std::string file =
        write_config(dir.path(), "test.chain", "run: { ticks: 3 }\n",
                     step("a", "fx-echo") + step("b", "fx-slow", "{ sleep_ms: 5000 }"));
    const std::unique_ptr<Workflow> workflow = runtime.open(file);

    std::optional<RunSummary> summary;
    std::thread runner([&] { summary = workflow->run(); });
    // b is busy during its create and initialize too, while run() starts the run; a busy b in a
    // running run is in its 5 s invocation.
    const auto in_invocation = [&] {
        const StatusSnapshot status = workflow->status();
        return status.state == WorkflowState::running &&
               step_status(status, "b").state == StepState::busy;
    };
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (!in_invocation() && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!in_invocation()) {
        workflow->request_stop();
        runner.join();
        FAIL() << "step b never ran its invocation";
    }

    // While run() is in progress, every other control call is refused; queries work.
    for (const char* call : {"start", "tick", "run"}) {
        SCOPED_TRACE(call);
        const Error error = error_of([&] {
            if (std::string(call) == "start") {
                workflow->start();
            } else if (std::string(call) == "tick") {
                workflow->tick();
            } else {
                workflow->run();
            }
        });
        EXPECT_EQ(error.code(), ErrorCode::INVALID_STATE);
        EXPECT_TRUE(contains(error.what(), "in progress")) << error.what();
    }
    EXPECT_EQ(workflow->state(), WorkflowState::running);
    EXPECT_EQ(json::parse(workflow->status_json()).at("workflow").at("state"), "running");

    // close() cannot be refused: it asks the run in progress to stop.
    workflow->close();
    runner.join();
    ASSERT_TRUE(summary.has_value());
    EXPECT_EQ(summary->final_state, WorkflowState::stopped);
    ASSERT_TRUE(summary->error.has_value());
    EXPECT_EQ(summary->error->code, ErrorCode::RUN_STOPPED);
    EXPECT_EQ(workflow->state(), WorkflowState::stopped);

    // Closing again completes the close; control calls are refused from then on.
    workflow->close();
    EXPECT_EQ(error_of([&] { workflow->run(); }).code(), ErrorCode::INVALID_STATE);
}

TEST(Api, CloseIsIdempotent) {
    ScratchDir dir;
    const Runtime runtime(fixture_options());
    const std::string file = write_config(
        dir.path(), "test.single", "run: { ticks: 5 }\n", step("a", "fx-echo"),
        "outputs: { directory: out }\nobservability: { status_file: out/status.json }\n");

    // A run closed before its end is stopped, its run record final.
    const std::unique_ptr<Workflow> workflow = runtime.open(file);
    workflow->start();
    workflow->tick();
    workflow->close();
    EXPECT_EQ(workflow->state(), WorkflowState::stopped);
    workflow->close();
    workflow->close();
    EXPECT_EQ(workflow->state(), WorkflowState::stopped);
    const json record = json::parse(workflow->run_record_json());
    EXPECT_EQ(record.at("execution").at("status"), "stopped");
    EXPECT_EQ(record.at("execution").at("ticks_completed"), 1);
    EXPECT_EQ(record.at("execution").at("error").at("name"), "RUN_STOPPED");
    EXPECT_EQ(json::parse(Runtime::read_status_file(utf8(dir.path() / "out" / "status.json")))
                  .at("workflow")
                  .at("state"),
              "stopped");
    // Every control call is refused from now on; the queries still answer.
    EXPECT_EQ(error_of([&] { workflow->start(); }).code(), ErrorCode::INVALID_STATE);
    EXPECT_EQ(error_of([&] { workflow->tick(); }).code(), ErrorCode::INVALID_STATE);
    EXPECT_EQ(error_of([&] { workflow->run(); }).code(), ErrorCode::INVALID_STATE);
    EXPECT_EQ(workflow->status().state, WorkflowState::stopped);
    EXPECT_EQ(json::parse(workflow->latest_outputs_json()).at("result").at("data"),
              json({{"value", 42}}));

    // A workflow closed before it started stays unstarted for good.
    const fs::path second = dir.path() / "second";
    const std::string other_file =
        write_config(second, "test.single", "run: { ticks: 5 }\n", step("a", "fx-echo"));
    const std::unique_ptr<Workflow> unstarted = runtime.open(other_file);
    unstarted->close();
    unstarted->close();
    EXPECT_EQ(unstarted->state(), WorkflowState::created);
    EXPECT_EQ(error_of([&] { unstarted->start(); }).code(), ErrorCode::INVALID_STATE);
    EXPECT_EQ(unstarted->run_record_json(), "null");

    // The destructor closes a workflow that nobody closed: the run ends as stopped, and its run
    // record and its status file say so.
    const fs::path third = dir.path() / "third";
    const std::string dropped_file = write_config(
        third, "test.single", "run: { ticks: 5 }\n", step("a", "fx-echo"),
        "outputs: { directory: out }\nobservability: { status_file: out/status.json }\n");
    {
        const std::unique_ptr<Workflow> dropped = runtime.open(dropped_file);
        dropped->start();
        dropped->tick();
    }
    const json dropped_record = json::parse(read_text(third / "out" / "run-record.json"));
    EXPECT_EQ(dropped_record.at("execution").at("status"), "stopped");
    EXPECT_EQ(dropped_record.at("execution").at("ticks_completed"), 1);
    EXPECT_EQ(dropped_record.at("execution").at("error").at("name"), "RUN_STOPPED");
    EXPECT_EQ(dropped_record.at("execution").at("final_status").at("workflow").at("state"),
              "stopped");
    EXPECT_EQ(json::parse(Runtime::read_status_file(utf8(third / "out" / "status.json")))
                  .at("workflow")
                  .at("state"),
              "stopped");
}

TEST(Api, LoggingResolvesEachSettingOnItsOwn) {
    // Each logging setting comes from the first of: the workflow's options, the environment
    // (PRAENORA_LOG_LEVEL, PRAENORA_LOG_FORMAT), the configuration's observability section,
    // RuntimeOptions::logging. Nothing of it reaches the fingerprint.
    const ScopedEnvironment level("PRAENORA_LOG_LEVEL", std::nullopt);
    const ScopedEnvironment format("PRAENORA_LOG_FORMAT", std::nullopt);
    const ScopedEnvironment modules("PRAENORA_MODULE_PATH", std::nullopt);
    const ScopedEnvironment catalogs("PRAENORA_CATALOG_PATH", std::nullopt);
    ScratchDir dir;
    const std::string file = write_config(
        dir.path(), "test.single", "run: { ticks: 1 }\n", step("a", "fx-echo"),
        "observability: { log_level: warn, log_format: json, log_file: config.log }\n");
    RuntimeOptions options = fixture_options();
    options.use_environment = true;
    options.logging.level = LogLevel::trace; // below the configuration's warn
    options.logging.format = LogFormat::text;
    const auto run = [&](const Runtime& runtime, const WorkflowOptions& overrides) {
        const std::unique_ptr<Workflow> workflow = runtime.open(file, overrides);
        EXPECT_EQ(workflow->run().final_state, WorkflowState::completed);
        return workflow->fingerprint();
    };
    const fs::path config_log = dir.path() / "config.log";

    // The configuration over RuntimeOptions::logging: JSON lines, nothing below warn.
    const std::string fingerprint = run(Runtime(options), {});
    EXPECT_TRUE(engine_events(config_log).empty()) << read_text(config_log);

    // The environment over the configuration, for the level only: debug events, still JSON.
    {
        const ScopedEnvironment debug("PRAENORA_LOG_LEVEL", std::string("debug"));
        EXPECT_EQ(run(Runtime(options), {}), fingerprint);
    }
    const auto debug_events = engine_events(config_log);
    EXPECT_TRUE(has_severity(debug_events, "DEBUG")) << read_text(config_log);
    EXPECT_TRUE(has_severity(debug_events, "INFO")) << read_text(config_log);

    // The workflow's options over the environment, and a log file of their own.
    {
        const ScopedEnvironment debug("PRAENORA_LOG_LEVEL", std::string("debug"));
        WorkflowOptions overrides;
        overrides.log_level = LogLevel::info;
        overrides.log_file = utf8(dir.path() / "override.log");
        EXPECT_EQ(run(Runtime(options), overrides), fingerprint);
    }
    const auto info_events = engine_events(dir.path() / "override.log");
    EXPECT_TRUE(has_severity(info_events, "INFO")) << read_text(dir.path() / "override.log");
    EXPECT_FALSE(has_severity(info_events, "DEBUG")) << read_text(dir.path() / "override.log");
    EXPECT_EQ(engine_events(config_log), debug_events); // nothing more went there

    // The environment's format over the configuration's.
    {
        const ScopedEnvironment text("PRAENORA_LOG_FORMAT", std::string("text"));
        WorkflowOptions overrides;
        overrides.log_level = LogLevel::info;
        overrides.log_file = utf8(dir.path() / "text.log");
        EXPECT_EQ(run(Runtime(options), overrides), fingerprint);
    }
    EXPECT_EQ(engine_events(dir.path() / "text.log"), std::vector<std::string>{"text"});
    EXPECT_TRUE(contains(read_text(dir.path() / "text.log"), " INFO  praenora.engine "))
        << read_text(dir.path() / "text.log");

    // With no override, no environment variable and no observability settings in the
    // configuration, RuntimeOptions::logging decides level, format and file.
    const std::string plain = write_config(dir.path() / "plain", "test.single",
                                           "run: { ticks: 1 }\n", step("a", "fx-echo"));
    RuntimeOptions fallback = options;
    fallback.logging.level = LogLevel::debug;
    fallback.logging.format = LogFormat::json;
    fallback.logging.file = utf8(dir.path() / "fallback.log");
    EXPECT_EQ(Runtime(fallback).open(plain)->run().final_state, WorkflowState::completed);
    const auto fallback_events = engine_events(dir.path() / "fallback.log");
    EXPECT_TRUE(has_severity(fallback_events, "DEBUG")) << read_text(dir.path() / "fallback.log");
    fallback.logging.level = LogLevel::warn;
    fallback.logging.file = utf8(dir.path() / "fallback-warn.log");
    EXPECT_EQ(Runtime(fallback).open(plain)->run().final_state, WorkflowState::completed);
    EXPECT_TRUE(engine_events(dir.path() / "fallback-warn.log").empty())
        << read_text(dir.path() / "fallback-warn.log");

    // The runtime's own logger, outside any run, takes the environment's level and format over
    // RuntimeOptions::logging: constructing a Runtime logs "runtime ready" at debug level.
    RuntimeOptions own = options;
    own.logging.level = LogLevel::info;
    own.logging.format = LogFormat::text;
    own.logging.file = utf8(dir.path() / "runtime.log");
    {
        const ScopedEnvironment debug("PRAENORA_LOG_LEVEL", std::string("debug"));
        const ScopedEnvironment json_lines("PRAENORA_LOG_FORMAT", std::string("json"));
        const Runtime with_environment(own);
    }
    const std::string runtime_log = read_text(dir.path() / "runtime.log");
    const json ready = json::parse(runtime_log.substr(0, runtime_log.find('\n')));
    EXPECT_EQ(ready.at("severity_text"), "DEBUG") << runtime_log;
    EXPECT_EQ(ready.at("body"), "runtime ready") << runtime_log;
    EXPECT_EQ(ready.at("attributes").at("logger"), "praenora.runtime") << runtime_log;
    {
        const ScopedEnvironment debug("PRAENORA_LOG_LEVEL", std::string("debug"));
        own.use_environment = false;
        const Runtime without_environment(own); // info: the debug record is filtered out
        own.use_environment = true;
    }
    EXPECT_EQ(read_text(dir.path() / "runtime.log"), runtime_log);

    // The environment is read only with use_environment, and a bad value names its variable.
    const ScopedEnvironment loud("PRAENORA_LOG_LEVEL", std::string("loud"));
    const Error error = error_of([&] { Runtime{options}; });
    EXPECT_EQ(error.code(), ErrorCode::CONFIG_INVALID);
    EXPECT_TRUE(contains(error.what(), "PRAENORA_LOG_LEVEL")) << error.what();
    options.use_environment = false;
    EXPECT_EQ(run(Runtime(options), {}), fingerprint);
}
