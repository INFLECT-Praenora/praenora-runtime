// The error taxonomy (praenora/runtime/error.hpp): the code table, its agreement with the C header,
// exit codes, Error and ValidationReport, plus the state names of praenora/runtime/status.hpp.
#include <praenora/runtime/error.hpp>
#include <praenora/runtime/status.hpp>
#include <praenora/sdk/status.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <string>

using praenora::all_error_codes;
using praenora::Diagnostic;
using praenora::Error;
using praenora::error_code_from_name;
using praenora::error_code_info;
using praenora::ErrorCategory;
using praenora::ErrorCode;
using praenora::Severity;
using praenora::SourceLocation;
using praenora::ValidationReport;

namespace {

SourceLocation location(std::string file, std::int64_t line, std::int64_t column,
                        std::string pointer) {
    SourceLocation loc;
    loc.file = std::move(file);
    loc.line = line;
    loc.column = column;
    loc.pointer = std::move(pointer);
    return loc;
}

Diagnostic diagnostic(Severity severity, ErrorCode code, std::string message, SourceLocation loc) {
    Diagnostic d;
    d.severity = severity;
    d.code = code;
    d.message = std::move(message);
    d.location = std::move(loc);
    return d;
}

} // namespace

TEST(ErrorTaxonomy, EveryCodeHasUniqueNameAndInfo) {
    const auto& codes = all_error_codes();
    // Every code of the taxonomy table plus 406 CATALOG_ENTRY_DUPLICATE, 407 SERVICE_UNAVAILABLE,
    // 503 MODULE_HOST_UNAVAILABLE and 706 MODULE_CRASHED.
    EXPECT_EQ(codes.size(), 53u);
    std::set<std::string> names;
    std::set<std::int32_t> values;
    std::int32_t previous = -1;
    for (const auto& info : codes) {
        const auto value = static_cast<std::int32_t>(info.code);
        SCOPED_TRACE(std::string(info.name));
        EXPECT_TRUE(names.insert(std::string(info.name)).second) << "duplicate name";
        EXPECT_TRUE(values.insert(value).second) << "duplicate code";
        EXPECT_GT(value, previous) << "the table is sorted by code";
        previous = value;
        EXPECT_FALSE(info.name.empty());
        EXPECT_FALSE(info.summary.empty());
        EXPECT_FALSE(info.owner.empty());
        const auto& looked_up = error_code_info(info.code);
        EXPECT_EQ(looked_up.code, info.code);
        EXPECT_EQ(looked_up.name, info.name);
        EXPECT_EQ(&looked_up, &info) << "error_code_info returns the table entry";
    }
}

TEST(ErrorTaxonomy, GeneralCodesMatchCHeader) {
    EXPECT_EQ(static_cast<int>(ErrorCode::OK), PRAENORA_STATUS_OK);
    EXPECT_EQ(static_cast<int>(ErrorCode::UNKNOWN), PRAENORA_STATUS_UNKNOWN);
    EXPECT_EQ(static_cast<int>(ErrorCode::INVALID_ARGUMENT), PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(static_cast<int>(ErrorCode::INVALID_STATE), PRAENORA_STATUS_INVALID_STATE);
    EXPECT_EQ(static_cast<int>(ErrorCode::NOT_FOUND), PRAENORA_STATUS_NOT_FOUND);
    EXPECT_EQ(static_cast<int>(ErrorCode::UNSUPPORTED), PRAENORA_STATUS_UNSUPPORTED);
    EXPECT_EQ(static_cast<int>(ErrorCode::RESOURCE_EXHAUSTED), PRAENORA_STATUS_RESOURCE_EXHAUSTED);
    EXPECT_EQ(static_cast<int>(ErrorCode::CANCELLED), PRAENORA_STATUS_CANCELLED);
    EXPECT_EQ(static_cast<int>(ErrorCode::TIMEOUT), PRAENORA_STATUS_TIMEOUT);
    EXPECT_EQ(static_cast<int>(ErrorCode::IO_ERROR), PRAENORA_STATUS_IO_ERROR);
    EXPECT_EQ(static_cast<int>(ErrorCode::INTERNAL), PRAENORA_STATUS_INTERNAL);
    EXPECT_EQ(static_cast<int>(ErrorCode::FAILED), PRAENORA_STATUS_FAILED);
    EXPECT_EQ(static_cast<int>(ErrorCode::DATA_INVALID), PRAENORA_STATUS_DATA_INVALID);

    // The C header names the general codes exactly like the C++ table, and the table has no other
    // code below 100.
    for (const auto& info : all_error_codes()) {
        const auto value = static_cast<std::int32_t>(info.code);
        if (value < 100) {
            EXPECT_LE(value, PRAENORA_STATUS_DATA_INVALID);
            EXPECT_EQ(info.name, praenora_status_name(value));
        }
    }
}

TEST(ErrorTaxonomy, ExitCodesFollowCategories) {
    EXPECT_EQ(error_code_info(ErrorCode::PARSE_ERROR).exit_code, 3);
    EXPECT_EQ(error_code_info(ErrorCode::LIBRARY_LOAD_FAILED).exit_code, 4);
    EXPECT_EQ(error_code_info(ErrorCode::STEP_TIMEOUT).exit_code, 5);
    EXPECT_EQ(error_code_info(ErrorCode::RUN_STOPPED).exit_code, 130);
    EXPECT_EQ(error_code_info(ErrorCode::INTERNAL).exit_code, 70);
    EXPECT_EQ(error_code_info(ErrorCode::IO_ERROR).exit_code, 74);
    EXPECT_EQ(error_code_info(ErrorCode::STEP_FAILED).exit_code, 1);

    const std::map<ErrorCategory, int> exit_for_category = {
        {ErrorCategory::usage, 2},         {ErrorCategory::validation, 3},
        {ErrorCategory::compatibility, 3}, {ErrorCategory::load, 4},
        {ErrorCategory::execution, 1},     {ErrorCategory::timeout, 5},
        {ErrorCategory::cancelled, 130},   {ErrorCategory::data, 1},
        {ErrorCategory::environment, 74},  {ErrorCategory::internal, 70},
    };
    for (const auto& info : all_error_codes()) {
        SCOPED_TRACE(std::string(info.name));
        if (info.code == ErrorCode::OK) {
            EXPECT_EQ(info.exit_code, 0);
        } else {
            EXPECT_EQ(info.exit_code, exit_for_category.at(info.category));
        }
    }
}

TEST(ErrorTaxonomy, NameRoundTrip) {
    for (const auto& info : all_error_codes()) {
        const auto code = error_code_from_name(info.name);
        ASSERT_TRUE(code.has_value()) << info.name;
        EXPECT_EQ(*code, info.code);
    }
    EXPECT_EQ(error_code_from_name("PARSE_ERROR"), ErrorCode::PARSE_ERROR);
    EXPECT_FALSE(error_code_from_name("parse_error").has_value()) << "names are case-sensitive";
    EXPECT_FALSE(error_code_from_name("").has_value());
    EXPECT_FALSE(error_code_from_name("NO_SUCH_CODE").has_value());
}

TEST(ErrorTaxonomy, CatalogDuplicateIs406) {
    EXPECT_EQ(static_cast<int>(ErrorCode::CATALOG_ENTRY_DUPLICATE), 406);
    const auto& info = error_code_info(ErrorCode::CATALOG_ENTRY_DUPLICATE);
    EXPECT_EQ(info.name, "CATALOG_ENTRY_DUPLICATE");
    EXPECT_EQ(info.category, ErrorCategory::compatibility);
    EXPECT_EQ(info.exit_code, 3);
    EXPECT_EQ(info.owner, "protocol");
}

TEST(ErrorTaxonomy, ServiceUnavailableIs407) {
    EXPECT_EQ(static_cast<int>(ErrorCode::SERVICE_UNAVAILABLE), 407);
    const auto& info = error_code_info(ErrorCode::SERVICE_UNAVAILABLE);
    EXPECT_EQ(info.name, "SERVICE_UNAVAILABLE");
    EXPECT_EQ(info.category, ErrorCategory::compatibility);
    EXPECT_EQ(info.exit_code, 3);
    EXPECT_FALSE(info.retryable);
    EXPECT_EQ(error_code_from_name("SERVICE_UNAVAILABLE"), ErrorCode::SERVICE_UNAVAILABLE);
}

TEST(ErrorTaxonomy, ModuleHostUnavailableIs503) {
    EXPECT_EQ(static_cast<int>(ErrorCode::MODULE_HOST_UNAVAILABLE), 503);
    const auto& info = error_code_info(ErrorCode::MODULE_HOST_UNAVAILABLE);
    EXPECT_EQ(info.name, "MODULE_HOST_UNAVAILABLE");
    EXPECT_EQ(info.category, ErrorCategory::load);
    EXPECT_EQ(info.exit_code, 4);
    EXPECT_EQ(info.owner, "environment");
    EXPECT_FALSE(info.retryable);
    EXPECT_EQ(error_code_from_name("MODULE_HOST_UNAVAILABLE"), ErrorCode::MODULE_HOST_UNAVAILABLE);
}

TEST(ErrorTaxonomy, ModuleCrashedIs706) {
    EXPECT_EQ(static_cast<int>(ErrorCode::MODULE_CRASHED), 706);
    const auto& info = error_code_info(ErrorCode::MODULE_CRASHED);
    EXPECT_EQ(info.name, "MODULE_CRASHED");
    EXPECT_EQ(info.category, ErrorCategory::execution);
    EXPECT_EQ(info.exit_code, 1);
    EXPECT_EQ(info.owner, "module");
    EXPECT_FALSE(info.retryable);
    EXPECT_EQ(error_code_from_name("MODULE_CRASHED"), ErrorCode::MODULE_CRASHED);
}

TEST(ErrorTaxonomy, SpotChecksAgainstTheTable) {
    const auto& parse = error_code_info(ErrorCode::PARSE_ERROR);
    EXPECT_EQ(static_cast<int>(parse.code), 100);
    EXPECT_EQ(parse.category, ErrorCategory::validation);
    EXPECT_EQ(parse.owner, "user");
    EXPECT_FALSE(parse.retryable);

    const auto& io = error_code_info(ErrorCode::IO_ERROR);
    EXPECT_EQ(io.category, ErrorCategory::environment);
    EXPECT_TRUE(io.retryable);

    const auto& target = error_code_info(ErrorCode::TARGET_DATA_INVALID);
    EXPECT_EQ(static_cast<int>(target.code), 800);
    EXPECT_EQ(target.category, ErrorCategory::data);
    EXPECT_EQ(target.owner, "user data");

    // A data file that cannot be read: the machine's, unlike its content, and worth a retry.
    const auto& unavailable = error_code_info(ErrorCode::TARGET_DATA_UNAVAILABLE);
    EXPECT_EQ(static_cast<int>(unavailable.code), 801);
    EXPECT_EQ(unavailable.category, ErrorCategory::data);
    EXPECT_EQ(unavailable.owner, "environment");
    EXPECT_TRUE(unavailable.retryable);

    // The ends of a connection carry two versions of one message type: the workflow
    // definition's fault, like PORT_TYPE_MISMATCH.
    const auto& incompatible = error_code_info(ErrorCode::MESSAGE_TYPE_INCOMPATIBLE);
    EXPECT_EQ(static_cast<int>(incompatible.code), 404);
    EXPECT_EQ(incompatible.category, ErrorCategory::validation);
    EXPECT_EQ(incompatible.owner, error_code_info(ErrorCode::PORT_TYPE_MISMATCH).owner);

    EXPECT_EQ(error_code_info(ErrorCode::UNSUPPORTED).category, ErrorCategory::compatibility);
    EXPECT_EQ(error_code_info(ErrorCode::MODULE_UNRESPONSIVE).category, ErrorCategory::timeout);
    EXPECT_EQ(praenora::to_string(ErrorCategory::environment), "environment");
    EXPECT_EQ(praenora::to_string(ErrorCategory::compatibility), "compatibility");
}

TEST(ErrorTaxonomy, CodesOutsideTheTableReportUnknown) {
    // Module-defined codes (10000 and up) are not runtime codes; they are reported as causes.
    const auto& info = error_code_info(static_cast<ErrorCode>(10005));
    EXPECT_EQ(info.code, ErrorCode::UNKNOWN);
    EXPECT_EQ(info.name, "UNKNOWN");
}

TEST(Error, ToJsonContainsCodeName) {
    const Error error(ErrorCode::PARSE_ERROR, "duplicate key 'a'",
                      location("workflow.yaml", 3, 1, "/a"), R"({"key":"a"})");
    EXPECT_EQ(error.code(), ErrorCode::PARSE_ERROR);
    EXPECT_STREQ(error.what(), "duplicate key 'a'");
    EXPECT_EQ(error.location().file, "workflow.yaml");
    EXPECT_EQ(error.location().line, 3);
    EXPECT_EQ(error.details_json(), R"({"key":"a"})");

    const auto text = error.to_json();
    const auto json = nlohmann::json::parse(text);
    EXPECT_EQ(json.at("code"), 100);
    EXPECT_EQ(json.at("name"), "PARSE_ERROR");
    EXPECT_EQ(json.at("category"), "validation");
    EXPECT_EQ(json.at("message"), "duplicate key 'a'");
    EXPECT_EQ(
        json.at("location"),
        nlohmann::json::parse(R"({"file":"workflow.yaml","line":3,"column":1,"pointer":"/a"})"));
    EXPECT_EQ(json.at("details"), nlohmann::json::parse(R"({"key":"a"})"));
    // Documented key order: code, name, category, message, location, details.
    EXPECT_EQ(text.rfind(R"({"code":100,"name":"PARSE_ERROR","category":"validation",)", 0), 0u)
        << text;
}

TEST(Error, DefaultsAndUnsetLocationParts) {
    const Error error(ErrorCode::IO_ERROR, "cannot read file");
    EXPECT_EQ(error.details_json(), "{}");
    const auto json = nlohmann::json::parse(error.to_json());
    EXPECT_EQ(json.at("location"), nlohmann::json::object()) << "unset parts are omitted";
    EXPECT_EQ(json.at("details"), nlohmann::json::object());
    EXPECT_EQ(json.at("category"), "environment");
}

TEST(Error, IsCaughtAsStdException) {
    try {
        throw Error(ErrorCode::CONFIG_INVALID, "bad duration");
    } catch (const std::exception& e) {
        EXPECT_STREQ(e.what(), "bad duration");
        const auto* error = dynamic_cast<const Error*>(&e);
        ASSERT_NE(error, nullptr);
        const Error copy = *error;
        EXPECT_EQ(copy.code(), ErrorCode::CONFIG_INVALID);
        EXPECT_STREQ(copy.what(), "bad duration");
    }
}

TEST(Error, DetailsThatAreNotJsonAreKeptAsText) {
    const Error error(ErrorCode::STEP_FAILED, "step failed", {}, "not json");
    const auto json = nlohmann::json::parse(error.to_json());
    EXPECT_EQ(json.at("details"), "not json");
}

TEST(Error, InvalidUtf8InMessageIsReplaced) {
    const Error error(ErrorCode::STEP_FAILED, std::string("bad \xff byte"));
    const auto json = nlohmann::json::parse(error.to_json());
    EXPECT_EQ(json.at("message"), "bad \xEF\xBF\xBD byte");
}

TEST(ValidationReport, TextFormatIsCompilerStyle) {
    ValidationReport report;
    EXPECT_TRUE(report.ok());
    EXPECT_EQ(report.to_text(), "");
    report.add(diagnostic(Severity::error, ErrorCode::PARAMETERS_INVALID, "too large",
                          location("workflow.yaml", 14, 7, "/steps/estimate/parameters/gain")));
    EXPECT_FALSE(report.ok());
    EXPECT_EQ(report.to_text(), "workflow.yaml:14:7: error[PARAMETERS_INVALID]: "
                                "/steps/estimate/parameters/gain: too large\n");
}

TEST(ValidationReport, TextOmitsEmptyParts) {
    ValidationReport report;
    report.add(diagnostic(Severity::warning, ErrorCode::MANIFEST_INVALID, "skipped",
                          location("m.pmm.json", 0, 0, "")));
    report.add(diagnostic(Severity::note, ErrorCode::MODULE_NOT_FOUND, "searched 2 directories",
                          location("", 0, 0, "")));
    report.add(diagnostic(Severity::error, ErrorCode::PARSE_ERROR, "unexpected end",
                          location("a.yaml", 3, 0, "/x")));
    EXPECT_EQ(report.to_text(), "m.pmm.json: warning[MANIFEST_INVALID]: skipped\n"
                                "note[MODULE_NOT_FOUND]: searched 2 directories\n"
                                "a.yaml:3: error[PARSE_ERROR]: /x: unexpected end\n");
}

TEST(ValidationReport, OkIgnoresWarningsAndNotes) {
    ValidationReport report;
    report.add(diagnostic(Severity::warning, ErrorCode::MANIFEST_INVALID, "shadowed", {}));
    report.add(diagnostic(Severity::note, ErrorCode::OK, "fyi", {}));
    EXPECT_TRUE(report.ok());
    report.add(diagnostic(Severity::error, ErrorCode::BINDING_INVALID, "extra step", {}));
    EXPECT_FALSE(report.ok());
    EXPECT_EQ(report.diagnostics.size(), 3u);
}

TEST(ValidationReport, ErrorCountIgnoresWarningsAndNotes) {
    ValidationReport report;
    EXPECT_EQ(report.error_count(), 0u);
    report.add(diagnostic(Severity::warning, ErrorCode::MANIFEST_INVALID, "shadowed", {}));
    report.add(diagnostic(Severity::note, ErrorCode::OK, "fyi", {}));
    EXPECT_EQ(report.error_count(), 0u);
    report.add(diagnostic(Severity::error, ErrorCode::BINDING_INVALID, "extra step", {}));
    report.add(diagnostic(Severity::warning, ErrorCode::MANIFEST_INVALID, "skipped", {}));
    report.add(diagnostic(Severity::error, ErrorCode::PARSE_ERROR, "unexpected end", {}));
    EXPECT_EQ(report.error_count(), 2u);
}

TEST(ValidationReport, FirstErrorSkipsWarningsAndNotes) {
    ValidationReport report;
    EXPECT_EQ(report.first_error(), nullptr);
    report.add(diagnostic(Severity::warning, ErrorCode::MANIFEST_INVALID, "shadowed", {}));
    report.add(diagnostic(Severity::note, ErrorCode::OK, "fyi", {}));
    EXPECT_EQ(report.first_error(), nullptr);
    report.add(diagnostic(Severity::error, ErrorCode::PARAMETERS_INVALID, "too large",
                          location("workflow.yaml", 14, 7, "/steps/estimate/parameters/gain")));
    report.add(diagnostic(Severity::error, ErrorCode::BINDING_INVALID, "extra step", {}));
    ASSERT_EQ(report.first_error(), &report.diagnostics[2]);
    EXPECT_EQ(report.first_error()->code, ErrorCode::PARAMETERS_INVALID);
    EXPECT_EQ(report.first_error()->location.line, 14);
}

TEST(ValidationReport, JsonListsDiagnostics) {
    ValidationReport report;
    auto d = diagnostic(Severity::error, ErrorCode::PARAMETERS_INVALID, "too large",
                        location("workflow.yaml", 14, 7, "/steps/estimate/parameters/gain"));
    d.step = "estimate";
    d.module = "example-estimator@0.1.0";
    report.add(d);
    report.add(diagnostic(Severity::warning, ErrorCode::MANIFEST_INVALID, "skipped", {}));

    const auto json = nlohmann::json::parse(report.to_json());
    EXPECT_EQ(json.at("ok"), false);
    ASSERT_EQ(json.at("diagnostics").size(), 2u);
    const auto& first = json.at("diagnostics")[0];
    EXPECT_EQ(first.at("severity"), "error");
    EXPECT_EQ(first.at("code"), 203);
    EXPECT_EQ(first.at("name"), "PARAMETERS_INVALID");
    EXPECT_EQ(first.at("category"), "validation");
    EXPECT_EQ(first.at("message"), "too large");
    EXPECT_EQ(first.at("location").at("line"), 14);
    EXPECT_EQ(first.at("location").at("pointer"), "/steps/estimate/parameters/gain");
    EXPECT_EQ(first.at("step"), "estimate");
    EXPECT_EQ(first.at("module"), "example-estimator@0.1.0");
    const auto& second = json.at("diagnostics")[1];
    EXPECT_EQ(second.at("severity"), "warning");
    EXPECT_EQ(second.at("location"), nlohmann::json::object());
    EXPECT_FALSE(second.contains("step"));
    EXPECT_FALSE(second.contains("module"));
}

// A diagnostic's details: the JSON object they are, left out when empty, and text that is not
// JSON as a string.
TEST(ValidationReport, JsonCarriesDetailsWhenSet) {
    ValidationReport report;
    auto with = diagnostic(Severity::error, ErrorCode::SERVICE_UNAVAILABLE, "no provider", {});
    with.details_json = R"({"service":"test.echo","major":0,"providers":[]})";
    report.add(with);
    report.add(diagnostic(Severity::warning, ErrorCode::MANIFEST_INVALID, "skipped", {}));
    auto text = diagnostic(Severity::error, ErrorCode::INTERNAL, "odd", {});
    text.details_json = "not JSON";
    report.add(text);

    const auto json = nlohmann::json::parse(report.to_json());
    const auto& diagnostics = json.at("diagnostics");
    ASSERT_EQ(diagnostics.size(), 3u);
    EXPECT_EQ(diagnostics[0].at("details"),
              nlohmann::json::parse(R"({"service":"test.echo","major":0,"providers":[]})"));
    EXPECT_FALSE(diagnostics[1].contains("details"));
    EXPECT_EQ(diagnostics[2].at("details"), "not JSON");
}

TEST(StatusNames, EveryStateHasItsName) {
    using praenora::Health;
    using praenora::StepState;
    using praenora::to_string;
    using praenora::WorkflowState;
    EXPECT_EQ(to_string(WorkflowState::created), "created");
    EXPECT_EQ(to_string(WorkflowState::starting), "starting");
    EXPECT_EQ(to_string(WorkflowState::ready), "ready");
    EXPECT_EQ(to_string(WorkflowState::running), "running");
    EXPECT_EQ(to_string(WorkflowState::stopping), "stopping");
    EXPECT_EQ(to_string(WorkflowState::completed), "completed");
    EXPECT_EQ(to_string(WorkflowState::stopped), "stopped");
    EXPECT_EQ(to_string(WorkflowState::failed), "failed");
    EXPECT_EQ(to_string(StepState::loaded), "loaded");
    EXPECT_EQ(to_string(StepState::created), "created");
    EXPECT_EQ(to_string(StepState::ready), "ready");
    EXPECT_EQ(to_string(StepState::busy), "busy");
    EXPECT_EQ(to_string(StepState::finalized), "finalized");
    EXPECT_EQ(to_string(StepState::destroyed), "destroyed");
    EXPECT_EQ(to_string(StepState::failed), "failed");
    EXPECT_EQ(to_string(StepState::unresponsive), "unresponsive");
    EXPECT_EQ(to_string(StepState::crashed), "crashed");
    EXPECT_EQ(to_string(Health::unknown), "unknown");
    EXPECT_EQ(to_string(Health::healthy), "healthy");
    EXPECT_EQ(to_string(Health::degraded), "degraded");
    EXPECT_EQ(to_string(Health::unhealthy), "unhealthy");
}
