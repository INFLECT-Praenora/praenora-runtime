// Invalid workflow graphs (tests/fixtures/catalog-bad/<case>/): each one is reported with its
// error code, and build_graph() refuses it.
#include "catalog/catalog.hpp"
#include "core/semver.hpp"
#include "graph/graph.hpp"

#include "test_support.hpp"

#include <praenora/runtime/error.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using praenora::Error;
using praenora::ErrorCode;
using praenora::detail::build_graph;
using praenora::detail::Catalog;
using praenora::detail::parse_semver;
using praenora::detail::validate_graph;

namespace {

struct Outcome {
    praenora::ValidationReport report;
    bool build_threw = false;
    ErrorCode build_code = ErrorCode::OK;
};

// Validates and builds workflow bad.<name>@0.1.0 of catalog-bad/<name>, on top of the test catalog.
Outcome check_case(const std::string& name) {
    const Catalog catalog = Catalog::load({praenora::test::utf8_path(PRAENORA_TEST_CATALOG_DIR),
                                           praenora::test::fixture_path("catalog-bad/" + name)});
    const auto* definition = catalog.find_workflow("bad." + name, parse_semver("0.1.0"));
    if (definition == nullptr) {
        throw std::runtime_error("no workflow bad." + name);
    }
    Outcome outcome;
    outcome.report = validate_graph(*definition, catalog);
    try {
        (void)build_graph(*definition, catalog);
    } catch (const Error& error) {
        outcome.build_threw = true;
        outcome.build_code = error.code();
    }
    return outcome;
}

// The first diagnostic with `code`, or nullptr.
const praenora::Diagnostic* find(const praenora::ValidationReport& report, ErrorCode code) {
    const auto it = std::find_if(report.diagnostics.begin(), report.diagnostics.end(),
                                 [&](const praenora::Diagnostic& d) { return d.code == code; });
    return it == report.diagnostics.end() ? nullptr : &*it;
}

void expect_rejected(const std::string& name, ErrorCode code) {
    const Outcome outcome = check_case(name);
    EXPECT_FALSE(outcome.report.ok()) << name;
    EXPECT_NE(find(outcome.report, code), nullptr) << name << "\n" << outcome.report.to_text();
    EXPECT_TRUE(outcome.build_threw) << name;
    EXPECT_EQ(outcome.build_code, code) << name;
}

} // namespace

TEST(Graph, CycleWithoutDelayIsGraphCycle) {
    expect_rejected("cycle", ErrorCode::GRAPH_CYCLE);
    const Outcome outcome = check_case("cycle");
    const auto* cycle = find(outcome.report, ErrorCode::GRAPH_CYCLE);
    ASSERT_NE(cycle, nullptr);
    EXPECT_NE(cycle->message.find("a -> b -> a"), std::string::npos) << cycle->message;
}

TEST(Graph, UnknownPortIsPortUnknown) {
    expect_rejected("unknown-port", ErrorCode::PORT_UNKNOWN);
    const Outcome outcome = check_case("unknown-port");
    const auto* unknown = find(outcome.report, ErrorCode::PORT_UNKNOWN);
    ASSERT_NE(unknown, nullptr);
    EXPECT_NE(unknown->message.find("a.input"), std::string::npos) << unknown->message;
    EXPECT_EQ(unknown->location.pointer, "/connections/0/to");
}

// The first diagnostic of case `name` with `code`; its message must contain every one of `parts`.
void expect_message(const std::string& name, ErrorCode code,
                    const std::vector<std::string>& parts) {
    const Outcome outcome = check_case(name);
    const auto* diagnostic = find(outcome.report, code);
    ASSERT_NE(diagnostic, nullptr) << name;
    for (const auto& part : parts) {
        EXPECT_NE(diagnostic->message.find(part), std::string::npos)
            << name << ": '" << part << "' not in: " << diagnostic->message;
    }
}

TEST(Graph, TypeMismatchIsPortTypeMismatch) {
    expect_rejected("type-mismatch", ErrorCode::PORT_TYPE_MISMATCH);
    expect_message("type-mismatch", ErrorCode::PORT_TYPE_MISMATCH,
                   {"inputs.source -> a.in", "pmp.observation-batch@0.1.0", "test.blob@0.1.0"});
}

TEST(Graph, TypeVersionMismatchIsPortTypeMismatch) {
    expect_rejected("version-mismatch", ErrorCode::PORT_TYPE_MISMATCH);
    expect_message("version-mismatch", ErrorCode::PORT_TYPE_MISMATCH,
                   {"test.blob@0.2.0", "test.blob@0.1.0"});
}

TEST(Graph, FanInIsGraphInvalid) {
    expect_rejected("fan-in", ErrorCode::GRAPH_INVALID);
    expect_message("fan-in", ErrorCode::GRAPH_INVALID,
                   {"inputs.other -> a.in", "a.in already has an incoming connection"});
}

TEST(Graph, DelayedEdgeIntoRequiredPortIsGraphInvalid) {
    expect_rejected("delayed-required", ErrorCode::GRAPH_INVALID);
    expect_message("delayed-required", ErrorCode::GRAPH_INVALID,
                   {"a.out -> b.in", "delayed connection", "optional input"});
}

TEST(Graph, OptionalOutputIntoRequiredInputIsGraphInvalid) {
    expect_rejected("optional-output-into-required", ErrorCode::GRAPH_INVALID);
    expect_message("optional-output-into-required", ErrorCode::GRAPH_INVALID,
                   {"a.out -> b.in", "required input", "optional output"});
}

TEST(Graph, RequiredInputUnconnectedIsPortUnconnected) {
    expect_rejected("required-unconnected", ErrorCode::PORT_UNCONNECTED);
    const Outcome outcome = check_case("required-unconnected");
    const auto* unconnected = find(outcome.report, ErrorCode::PORT_UNCONNECTED);
    ASSERT_NE(unconnected, nullptr);
    EXPECT_EQ(unconnected->step, "b");
    EXPECT_NE(unconnected->message.find("b.in"), std::string::npos) << unconnected->message;
}

TEST(Graph, WorkflowOutputUnconnectedIsPortUnconnected) {
    expect_rejected("output-unconnected", ErrorCode::PORT_UNCONNECTED);
    const Outcome outcome = check_case("output-unconnected");
    const auto* unconnected = find(outcome.report, ErrorCode::PORT_UNCONNECTED);
    ASSERT_NE(unconnected, nullptr);
    EXPECT_NE(unconnected->message.find("outputs.extra"), std::string::npos)
        << unconnected->message;
}

TEST(Graph, DelayGreaterThanOneRejected) {
    expect_rejected("delay-two", ErrorCode::GRAPH_INVALID);
    const Outcome outcome = check_case("delay-two");
    const auto* invalid = find(outcome.report, ErrorCode::GRAPH_INVALID);
    ASSERT_NE(invalid, nullptr);
    EXPECT_EQ(invalid->location.pointer, "/connections/1/delay");
}
