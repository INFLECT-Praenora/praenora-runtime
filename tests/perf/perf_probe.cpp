// praenora_perf: the performance probe (report-only). It measures what the runtime itself costs
// per module invocation: the test workflow test.noop-chain (tests/fixtures/catalog: four test.echo
// steps a -> b -> c -> d, without workflow inputs) runs with each step bound to the fixture module
// fx-noop, which does nothing, and the probe prints one JSON object on standard output:
//
//   {"ticks":N,"steps":4,"invocations":I,"total_ns":T,"mean_invocation_overhead_ns":T/I,
//    "mean_tick_ns":T/N,"validation":"strict"|"off","isolation":"in-process"|"process"}
//
//   ticks         the ticks run (--ticks)
//   steps         the workflow's steps
//   invocations   the module invocations, as the runtime's metrics count them (steps x ticks)
//   total_ns      the duration of the ticks, each as the engine measures it, from the start of its
//                 work to its end. Starting the run (loading the module, creating and
//                 initializing the instances) and ending it (finalizing, destroying, unloading),
//                 which follows the last tick, are not included.
//   mean_invocation_overhead_ns   total_ns / invocations: the runtime's own cost per invocation,
//                 since the module does nothing
//   mean_tick_ns  total_ns / ticks
//   validation    strict: every payload and envelope is validated against its schema, as in any
//                 run; off: none is, which only measurements like this one use. It is the
//                 engine's EngineSettings::validate_payloads, which the run record reports.
//   isolation     in-process (the default): the steps run in this process; process: each step
//                 runs in a module host of its own (WorkflowOptions::isolation), so each call is a
//                 round trip over its channel. The run record reports it for every step.
// The difference between a strict and an off run is what validation costs; between an in-process
// and a process run, what a module host costs.
//
//   praenora_perf --ticks N --validation strict|off [--isolation in-process|process]
//
// Exit code 0 with the report; 1 when the run fails or does not do what it should (described on
// standard error); 2 on wrong usage. Turning validation off is not part of the public API, so the
// probe links the runtime's object library. It writes the configuration to a scratch directory
// below the system temporary directory (praenora::test::ScratchDir), removed at the end; the run
// itself writes no file (it has no output directory) and logs nothing.
#include "api/validation.hpp"
#include "config/workflow_config.hpp"
#include "core/paths.hpp"
#include "core/seed.hpp"
#include "engine/engine.hpp"
#include "observability/metrics.hpp"

#include "temp_dir.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/status.hpp>

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

namespace fs = std::filesystem;
namespace detail = praenora::detail;

constexpr std::string_view kUsage =
    "usage: praenora_perf --ticks N --validation strict|off [--isolation in-process|process]\n";

struct Options {
    std::uint64_t ticks = 0;
    std::string validation; // strict or off
    praenora::IsolationMode isolation = praenora::IsolationMode::in_process;
};

// The options in argv; nullopt, after saying why on standard error, when they are wrong.
std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    bool have_ticks = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view name = argv[i];
        if ((name != "--ticks" && name != "--validation" && name != "--isolation") ||
            i + 1 == argc) {
            std::cerr << "praenora_perf: unexpected argument '" << name << "'\n" << kUsage;
            return std::nullopt;
        }
        const std::string_view value = argv[++i];
        if (name == "--ticks") {
            const char* const end = value.data() + value.size();
            const auto [last, error] = std::from_chars(value.data(), end, options.ticks);
            if (error != std::errc() || last != end || options.ticks == 0 ||
                options.ticks > detail::kMaxJsonSafeInteger) {
                std::cerr << "praenora_perf: --ticks '" << value << "' is not a count of ticks "
                          << "from 1 to " << detail::kMaxJsonSafeInteger << '\n';
                return std::nullopt;
            }
            have_ticks = true;
        } else if (name == "--isolation") {
            // The names of the configuration's isolation.mode, from the runtime itself.
            const auto mode = detail::isolation_mode_from_name(value);
            if (!mode) {
                std::cerr << "praenora_perf: --isolation '" << value << "' is not "
                          << detail::isolation_mode_name(praenora::IsolationMode::in_process)
                          << " or " << detail::isolation_mode_name(praenora::IsolationMode::process)
                          << '\n';
                return std::nullopt;
            }
            options.isolation = *mode;
        } else if (value == "strict" || value == "off") {
            options.validation = value;
        } else {
            std::cerr << "praenora_perf: --validation '" << value << "' is not strict or off\n";
            return std::nullopt;
        }
    }
    if (!have_ticks || options.validation.empty()) {
        std::cerr << "praenora_perf: --ticks and --validation are required\n" << kUsage;
        return std::nullopt;
    }
    return options;
}

// The configuration: test.noop-chain with every step bound to fx-noop.
std::string configuration() {
    std::string yaml = "kind: workflow-config\n"
                       "format_version: \"0.1.0\"\n"
                       "workflow: { name: test.noop-chain, version: \"0.1.0\" }\n"
                       "steps:\n";
    for (const char* step : {"a", "b", "c", "d"}) {
        yaml += std::string("  ") + step + ": { module: { name: fx-noop, version: \"0.1.0\" } }\n";
    }
    return yaml;
}

// Runs the probe; the exit code.
int run(const Options& options) {
    const praenora::test::ScratchDir directory;
    const fs::path config = directory.path() / "workflow.yaml";
    detail::write_file_atomically(config, configuration());

    praenora::RuntimeOptions runtime_options;
    runtime_options.module_paths = {std::string(PRAENORA_TEST_MODULES_DIR) + "/fx-noop"};
    runtime_options.catalog_paths = {PRAENORA_TEST_CATALOG_DIR};
    runtime_options.use_environment = false;
    // Outlives the engine: the validated workflow points into its catalog.
    const detail::RuntimeContext context = detail::make_runtime_context(runtime_options);

    praenora::WorkflowOptions workflow_options;
    workflow_options.ticks = options.ticks;
    workflow_options.isolation = options.isolation;
    praenora::ValidationReport report;
    std::optional<detail::ValidatedWorkflow> workflow =
        detail::validate_workflow(config, context, workflow_options, report);
    if (!workflow) {
        std::cerr << "praenora_perf: the configuration is not valid:\n" << report.to_text();
        return 1;
    }
    const std::size_t steps = workflow->steps.size();

    detail::MetricsRegistry metrics;
    detail::EngineSettings settings;
    settings.metrics = &metrics;
    settings.services = &context.services;
    settings.validate_payloads = options.validation == "strict";
    detail::Engine engine(std::move(*workflow), settings);
    engine.start();
    std::int64_t total_ns = 0;
    for (std::uint64_t tick = 0; tick < options.ticks; ++tick) {
        engine.run_tick();
        total_ns += metrics.snapshot().last_tick_duration_ns;
    }

    // The run did what it should: it completed, every step ran once per tick and succeeded, and
    // its record reports the validation used.
    if (engine.state() != praenora::WorkflowState::completed) {
        std::cerr << "praenora_perf: the run ended " << praenora::to_string(engine.state())
                  << ", not completed\n";
        return 1;
    }
    std::uint64_t invocations = 0;
    std::uint64_t succeeded = 0;
    for (const praenora::StepStatus& step : metrics.snapshot().steps) {
        invocations += step.invocations;
        succeeded += step.succeeded;
    }
    if (invocations != steps * options.ticks || succeeded != invocations) {
        std::cerr << "praenora_perf: " << invocations << " invocations (" << succeeded
                  << " succeeded), expected " << steps * options.ticks << '\n';
        return 1;
    }
    // A copy that outlives the checks: run_record() returns one by value.
    const nlohmann::json execution = engine.run_record().at("execution");
    const auto& recorded = execution.at("validation");
    if (recorded != options.validation) {
        std::cerr << "praenora_perf: the run record reports validation " << recorded.dump()
                  << ", not \"" << options.validation << "\"\n";
        return 1;
    }
    const std::string isolation_name(detail::isolation_mode_name(options.isolation));
    for (const auto& [step, isolation] : execution.at("isolation").items()) {
        if (isolation.at("mode") != isolation_name) {
            std::cerr << "praenora_perf: the run record reports isolation " << isolation.dump()
                      << " for step " << step << ", not \"" << isolation_name << "\"\n";
            return 1;
        }
    }

    nlohmann::ordered_json result;
    result["ticks"] = options.ticks;
    result["steps"] = steps;
    result["invocations"] = invocations;
    result["total_ns"] = total_ns;
    result["mean_invocation_overhead_ns"] =
        static_cast<double>(total_ns) / static_cast<double>(invocations);
    result["mean_tick_ns"] = static_cast<double>(total_ns) / static_cast<double>(options.ticks);
    result["validation"] = options.validation;
    result["isolation"] = isolation_name;
    std::cout << result.dump() << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
        std::cout << kUsage;
        return 0;
    }
    const std::optional<Options> options = parse_options(argc, argv);
    if (!options) {
        return 2;
    }
    try {
        return run(*options);
    } catch (const praenora::Error& error) {
        std::cerr << "praenora_perf: " << praenora::error_code_info(error.code()).name << ": "
                  << error.what() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "praenora_perf: " << error.what() << '\n';
    }
    return 1;
}
