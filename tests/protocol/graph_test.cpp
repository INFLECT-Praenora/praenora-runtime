// Workflow graphs of valid catalog workflows: validation and the deterministic schedule.
#include "catalog/catalog.hpp"
#include "core/semver.hpp"
#include "graph/graph.hpp"

#include "test_support.hpp"

#include <praenora/runtime/error.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using praenora::detail::build_graph;
using praenora::detail::Catalog;
using praenora::detail::Endpoint;
using praenora::detail::parse_semver;
using praenora::detail::validate_graph;

namespace {

const praenora::detail::SemVer kV010 = parse_semver("0.1.0");

const praenora::detail::WorkflowDef& workflow(const Catalog& catalog, const std::string& name) {
    const auto* definition = catalog.find_workflow(name, kV010);
    if (definition == nullptr) {
        throw std::runtime_error("no workflow " + name);
    }
    return *definition;
}

} // namespace

TEST(Graph, ExampleScheduleIsEstimateThenPredict) {
    const Catalog catalog = Catalog::load({});
    const auto& definition = workflow(catalog, "example.estimate-predict");
    const auto report = validate_graph(definition, catalog);
    EXPECT_TRUE(report.ok()) << report.to_text();
    const auto graph = build_graph(definition, catalog);
    EXPECT_EQ(graph.definition, &definition);
    EXPECT_EQ(graph.schedule, (std::vector<std::string>{"estimate", "predict"}));
    ASSERT_EQ(graph.incoming.at("estimate").size(), 2u);
    EXPECT_EQ(graph.incoming.at("estimate").at("observations").from.kind,
              Endpoint::Kind::workflow_input);
    EXPECT_EQ(graph.incoming.at("estimate").at("observations").type, "pmp.observation-batch");
    EXPECT_EQ(graph.incoming.at("predict").at("state").from.to_string(), "estimate.state");
    ASSERT_EQ(graph.outputs.size(), 2u);
    EXPECT_EQ(graph.outputs.at("predictions").from.to_string(), "predict.predictions");
    EXPECT_EQ(graph.outputs.at("state").from.to_string(), "estimate.state");
    EXPECT_EQ(graph.outputs.at("state").type, "example.state-estimate");
}

TEST(Graph, DraftReferenceWorkflowIsValid) {
    const Catalog catalog = Catalog::load({});
    const auto& definition = workflow(catalog, "pmp.continuous-forecast");
    const auto report = validate_graph(definition, catalog);
    EXPECT_TRUE(report.ok()) << report.to_text();
    const auto graph = build_graph(definition, catalog);
    EXPECT_EQ(graph.schedule, (std::vector<std::string>{"propose", "curate", "predict"}));
    const auto& replenish = graph.incoming.at("propose").at("replenish");
    EXPECT_EQ(replenish.from.to_string(), "curate.replenish");
    EXPECT_EQ(replenish.delay, 1);
    EXPECT_EQ(graph.outputs.at("candidates").from.to_string(), "curate.survivors");
    EXPECT_EQ(graph.outputs.at("predictions").type, "pmp.prediction-set");
}

TEST(Graph, TestLoopScheduleIsAThenB) {
    const Catalog catalog = Catalog::load({praenora::test::utf8_path(PRAENORA_TEST_CATALOG_DIR)});
    for (const char* name : {"test.single", "test.chain", "test.loop"}) {
        const auto report = validate_graph(workflow(catalog, name), catalog);
        EXPECT_TRUE(report.ok()) << name << "\n" << report.to_text();
    }
    EXPECT_EQ(build_graph(workflow(catalog, "test.single"), catalog).schedule,
              (std::vector<std::string>{"a"}));
    EXPECT_EQ(build_graph(workflow(catalog, "test.chain"), catalog).schedule,
              (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(build_graph(workflow(catalog, "test.loop"), catalog).schedule,
              (std::vector<std::string>{"a", "b"}));
}

TEST(Graph, FeedbackConnectionRecordedWithDelay) {
    const Catalog catalog = Catalog::load({praenora::test::utf8_path(PRAENORA_TEST_CATALOG_DIR)});
    const auto graph = build_graph(workflow(catalog, "test.loop"), catalog);
    const auto& feedback = graph.incoming.at("a").at("in");
    EXPECT_EQ(feedback.from.to_string(), "b.out");
    EXPECT_EQ(feedback.to.to_string(), "a.in");
    EXPECT_EQ(feedback.delay, 1);
    EXPECT_EQ(feedback.type, "test.blob");
    EXPECT_EQ(praenora::detail::to_string(feedback.type_version), "0.1.0");
    EXPECT_EQ(graph.incoming.at("b").at("in").delay, 0);
    EXPECT_EQ(graph.outputs.at("result").from.to_string(), "b.out");
}
