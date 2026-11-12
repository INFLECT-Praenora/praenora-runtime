// The run fingerprint: the SHA-256 of the canonical reproducibility section, which covers what
// determines a run's results (versions, catalog entries, the semantic configuration, modules,
// input data and seeds) and nothing else (output and observability settings, paths).
#include "api/validation.hpp"
#include "core/paths.hpp"
#include "core/seed.hpp"
#include "core/semver.hpp"
#include "core/sha256.hpp"
#include "document/json_util.hpp"
#include "document/schema_registry.hpp"

#include "module_fixtures.hpp"
#include "test_support.hpp"
#include "validation_fixtures.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/version.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::detail::path_to_utf8;
using praenora::test::ScratchDir;
using praenora::test::validate_text;
using praenora::test::write_text_file;

namespace {

// A valid configuration of test.chain over the observations CSV `obs.csv` beside it. `run` and
// `extra` are inserted as given; `b_parameters` are the parameters of step b (fx-slow).
std::string chain_config(const std::string& run = "run: { seed: 7 }\n",
                         const std::string& b_parameters = "{}", const std::string& extra = "") {
    return "kind: workflow-config\n"
           "format_version: \"0.1.0\"\n"
           "workflow: { name: test.chain, version: \"0.1.0\" }\n" +
           run +
           "inputs:\n"
           "  source: { source: observation-csv, path: obs.csv }\n"
           "steps:\n"
           "  a: { module: { name: fx-echo, version: \"0.1.0\" } }\n"
           "  b:\n"
           "    module: { name: fx-slow, version: \"0.1.0\" }\n"
           "    parameters: " +
           b_parameters + "\n" + extra;
}

// Validates `yaml` in a fresh directory holding the observations CSV `csv`.
praenora::detail::ValidatedWorkflow
validated(const std::string& yaml, const praenora::WorkflowOptions& options = {},
          const std::string& csv = praenora::test::observations_csv()) {
    ScratchDir dir;
    write_text_file(dir.path() / "obs.csv", csv);
    auto result = validate_text(dir.path(), yaml, options);
    if (!result.workflow) {
        ADD_FAILURE() << result.report.to_text();
        throw std::runtime_error("validation failed");
    }
    return std::move(*result.workflow);
}

} // namespace

TEST(Fingerprint, StableAcrossLoads) {
    const auto first = validated(chain_config());
    const auto second = validated(chain_config()); // another directory, same content
    EXPECT_EQ(first.fingerprint, second.fingerprint);
    EXPECT_EQ(first.reproducibility, second.reproducibility);
    const std::string canonical = praenora::detail::canonical_json(first.reproducibility);
    EXPECT_EQ(first.fingerprint, praenora::detail::sha256_hex(canonical));
    // Paths are not part of it.
    EXPECT_EQ(first.reproducibility.dump().find("obs.csv"), std::string::npos);
}

TEST(Fingerprint, ChangesWithSeed) {
    const auto seven = validated(chain_config("run: { seed: 7 }\n"));
    const auto eight = validated(chain_config("run: { seed: 8 }\n"));
    EXPECT_NE(seven.fingerprint, eight.fingerprint);
    // The caller's seed takes the place of the file's.
    praenora::WorkflowOptions options;
    options.seed = 8;
    EXPECT_EQ(validated(chain_config("run: { seed: 7 }\n"), options).fingerprint,
              eight.fingerprint);
    EXPECT_EQ(eight.reproducibility.at("seed"), 8);
    EXPECT_EQ(eight.reproducibility.at("steps").at("b").at("seed"),
              praenora::detail::derive_step_seed(8, "b"));
}

TEST(Fingerprint, IgnoresObservabilityAndOutputs) {
    const auto plain = validated(chain_config());
    const auto dressed = validated(
        chain_config("run: { seed: 7 }\n", "{}",
                     "    log_level: trace\n"
                     "outputs: { directory: out, record: [result], trace_messages: true }\n"
                     "observability:\n"
                     "  log_level: debug\n"
                     "  log_format: json\n"
                     "  log_file: logs/run.log\n"
                     "  status_file: status.json\n"
                     "  status_interval: 250ms\n"));
    EXPECT_EQ(plain.fingerprint, dressed.fingerprint);
    // The caller's output and status settings do not count either.
    praenora::WorkflowOptions options;
    options.output_dir = "elsewhere";
    options.status_file = "status.json";
    options.trace_messages = true;
    EXPECT_EQ(validated(chain_config(), options).fingerprint, plain.fingerprint);
}

TEST(Fingerprint, ChangesWithTicks) {
    // Without run.ticks the run lasts until the replay input is exhausted.
    const auto until_exhausted = validated(chain_config("run: { seed: 7 }\n"));
    const auto three = validated(chain_config("run: { seed: 7, ticks: 3 }\n"));
    const auto four = validated(chain_config("run: { seed: 7, ticks: 4 }\n"));
    EXPECT_NE(three.fingerprint, until_exhausted.fingerprint);
    EXPECT_NE(three.fingerprint, four.fingerprint);
    EXPECT_EQ(until_exhausted.reproducibility.at("ticks"), nullptr);
    EXPECT_EQ(three.reproducibility.at("ticks"), 3);
    // The caller's tick count takes the place of the file's.
    praenora::WorkflowOptions options;
    options.ticks = 3;
    EXPECT_EQ(validated(chain_config("run: { seed: 7, ticks: 4 }\n"), options).fingerprint,
              three.fingerprint);
    EXPECT_EQ(validated(chain_config("run: { seed: 7 }\n"), options).fingerprint,
              three.fingerprint);
}

TEST(Fingerprint, DefaultsMaterializedBeforeHashing) {
    // fx-slow's parameter defaults: sleep_ms 100, finalize_sleep_ms 0, cooperative true.
    const auto omitted = validated(chain_config("run: { seed: 7 }\n", "{}"));
    const auto explicit_defaults = validated(chain_config(
        "run: { seed: 7 }\n", "{ sleep_ms: 100, finalize_sleep_ms: 0, cooperative: true }"));
    EXPECT_EQ(omitted.fingerprint, explicit_defaults.fingerprint);
    EXPECT_EQ(omitted.reproducibility.at("steps").at("b").at("parameters"),
              json::parse(R"({"sleep_ms":100,"finalize_sleep_ms":0,"cooperative":true})"));
    // A value other than the default does change it.
    EXPECT_NE(validated(chain_config("run: { seed: 7 }\n", "{ sleep_ms: 101 }")).fingerprint,
              omitted.fingerprint);
}

TEST(Fingerprint, ChangesWithDataTimeBaseAndTimeouts) {
    const auto base = validated(chain_config());
    // One byte more in the data.
    EXPECT_NE(validated(chain_config(), {}, std::string(praenora::test::observations_csv()) + "\n")
                  .fingerprint,
              base.fingerprint);
    EXPECT_NE(validated(chain_config("run: { seed: 7, time: { period: 0.5 } }\n")).fingerprint,
              base.fingerprint);
    const auto ten = validated(chain_config("run: { seed: 7, timeouts: { invoke: 10s } }\n"));
    EXPECT_NE(ten.fingerprint, base.fingerprint);
    // The run's own timeouts are in the section, beside each step's effective ones.
    EXPECT_EQ(ten.reproducibility.at("timeouts"),
              json::parse(R"({"invoke":"10s","lifecycle":"60s","cancel_grace":"5s"})"));
    EXPECT_EQ(ten.reproducibility.at("steps").at("a").at("timeouts").at("invoke"), "10s");
}

TEST(Fingerprint, ReproducibilitySectionMatchesTheRunRecordSchema) {
    const auto workflow = validated(chain_config());
    const json& section = workflow.reproducibility;
    const praenora::VersionInfo versions = praenora::version_info();

    // The section is embedded in the run record; it must satisfy that schema.
    const json record = {
        {"kind", "run-record"},
        {"format_version", "0.1.0"},
        {"fingerprint", workflow.fingerprint},
        {"reproducibility", section},
        {"execution",
         {{"status", "running"},
          {"ticks_completed", 0},
          {"platform",
           {{"os", "macos"}, {"arch", "arm64"}, {"compiler", "c"}, {"build_type", "Debug"}}},
          {"overrides", json::object()},
          {"validation", "strict"}}}};
    const auto& schema = praenora::detail::SchemaRegistry::builtin().for_kind(
        "run-record", praenora::detail::parse_semver("0.1.0"));
    const auto issues = schema.validate(record);
    for (const auto& issue : issues) {
        ADD_FAILURE() << issue.pointer << ": " << issue.message;
    }

    EXPECT_EQ(section.at("versions"),
              (json{{"runtime", versions.runtime},
                    {"abi", {{"major", versions.abi_major}, {"minor", versions.abi_minor}}},
                    {"pmp", versions.pmp}}));
    EXPECT_EQ(section.at("workflow").at("name"), "test.chain");
    EXPECT_EQ(section.at("workflow").at("digest"), workflow.definition->digest);
    // The catalog entries the workflow uses, by id@version.
    json catalog = json::object();
    catalog["test.echo@0.1.0"] = workflow.steps.at(0).capability->digest;
    catalog["test.blob@0.1.0"] =
        praenora::test::test_context()
            .catalog.find_message_type("test.blob", praenora::detail::parse_semver("0.1.0"))
            ->digest;
    EXPECT_EQ(section.at("catalog"), catalog);
    EXPECT_EQ(section.at("time"), json::parse(R"({"unit":"1","start":0.0,"period":1.0})"));
    EXPECT_EQ(section.at("seed"), 7);
    EXPECT_EQ(section.at("ticks"), nullptr); // until the replay input is exhausted
    EXPECT_EQ(section.at("timeouts"),
              json::parse(R"({"invoke":"300s","lifecycle":"60s","cancel_grace":"5s"})"));
    EXPECT_EQ(
        section.at("inputs"),
        (json{{"source",
               {{"source", "observation-csv"},
                {"digest", praenora::detail::sha256_hex(praenora::test::observations_csv())}}}}));
    const json& a = section.at("steps").at("a");
    const auto& manifest = *workflow.steps.at(0).manifest;
    EXPECT_EQ(a.at("capability"), json::parse(R"({"id":"test.echo","version":"0.1.0"})"));
    EXPECT_EQ(a.at("module"),
              (json{{"name", "fx-echo"},
                    {"version", "0.1.0"},
                    {"manifest_sha256", manifest.manifest_sha256},
                    {"library_sha256", praenora::detail::sha256_file_hex(manifest.library_path)},
                    {"sdk_version", "0.1.0"}}));
    EXPECT_EQ(a.at("parameters"), json::object());
    EXPECT_EQ(a.at("seed"), praenora::detail::derive_step_seed(7, "a"));
    EXPECT_EQ(a.at("timeouts"),
              json::parse(R"({"invoke":"300s","lifecycle":"60s","cancel_grace":"5s"})"));
}
