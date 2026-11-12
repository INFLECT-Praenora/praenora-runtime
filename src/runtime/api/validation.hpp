// The validation sequence: everything a run checks before tick 0, without loading any module
// code. It turns a configuration file into a ValidatedWorkflow (resolved steps, input sources,
// the reproducibility section and the run fingerprint) or into diagnostics.
#ifndef PRAENORA_RUNTIME_API_VALIDATION_HPP
#define PRAENORA_RUNTIME_API_VALIDATION_HPP

#include "catalog/catalog.hpp"
#include "config/workflow_config.hpp"
#include "discovery/manifest.hpp"
#include "discovery/module_index.hpp"
#include "graph/graph.hpp"
#include "instance/services.hpp"
#include "io/input_source.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace praenora::detail {

// A step of the workflow with everything needed to run it.
struct ResolvedStep {
    std::string id;
    const CapabilityDef* capability = nullptr; // in the context's catalog
    const ModuleManifest* manifest = nullptr;  // owned by ValidatedWorkflow::manifests
    nlohmann::json parameters;                 // with the parameter schema's defaults filled in
    std::uint64_t seed = 0;                    // derive_step_seed(run seed, id)
    Timeouts timeouts;
    std::optional<LogLevel> log_level;
    std::string library_sha256; // of the library file when it was validated
};

struct ValidatedWorkflow {
    // The configuration with the caller's overrides applied (see read_workflow_config()), and
    // `record` filled in with every workflow output when the file lists none.
    WorkflowConfig config;
    const WorkflowDef* definition = nullptr; // in the context's catalog
    WorkflowGraph graph;
    std::vector<ResolvedStep> steps;                             // in schedule order
    std::map<std::string, std::unique_ptr<InputSource>> sources; // by workflow input
    // The run record's reproducibility section (the run-record schema's shape):
    //   versions {runtime, abi {major, minor}, pmp},
    //   workflow {name, version, digest},
    //   catalog {"<id>@<version>": digest} for every capability and message type the workflow
    //     uses (keys become "<kind>:<id>@<version>" for an id@version that two kinds share),
    //   time {unit, start, period}, seed,
    //   ticks (null when the run lasts until its replay inputs are exhausted),
    //   timeouts {invoke, lifecycle, cancel_grace} (the run's),
    //   inputs {<input>: {source, digest[, columns]}},
    //   steps {<step>: {capability {id, version},
    //                   module {name, version, manifest_sha256, library_sha256, sdk_version},
    //                   parameters, seed, timeouts {invoke, lifecycle, cancel_grace}}}.
    // It holds no paths, no wall-clock time and nothing from `outputs` or `observability`.
    nlohmann::json reproducibility;
    std::string fingerprint; // sha256_hex(canonical_json(reproducibility))
    // The manifests of the steps' modules, one per manifest file.
    std::vector<std::shared_ptr<const ModuleManifest>> manifests;
    // Every message type the workflow's ports carry (its inputs and outputs and the ports of its
    // steps' capabilities), by "<id>@<version>"; in the context's catalog.
    std::map<std::string, const MessageTypeDef*> message_types;
};

// What validation runs against: the catalog, the index of the module search paths, and the
// runtime services.
//
// Invariant: `modules` is ModuleIndex::scan(module_search_paths(module_paths, {},
// use_environment)). validate_workflow() relies on it: when a configuration names module search
// paths of its own, it rebuilds the search list from `module_paths`, those paths and
// `use_environment` (the caller's paths, the configuration's, PRAENORA_MODULE_PATH, the
// installation's directory), so a context whose fields disagree would drop or misplace the
// caller's paths. Build contexts with make_runtime_context(), which keeps them consistent.
struct RuntimeContext {
    Catalog catalog;
    ModuleIndex modules;
    ServiceRegistry services;
    std::vector<std::string> module_paths;
    bool use_environment = true;
};

// The runtime context for `options`:
// - catalog: the built-in entries, then each of options.catalog_paths (UTF-8; relative ones
//   against the current directory; each must be a readable directory, else IO_ERROR), then, when
//   options.use_environment, the entries of PRAENORA_CATALOG_PATH (':'-separated, ';' on Windows;
//   relative ones against the current directory; entries that are not directories are skipped,
//   as for PRAENORA_MODULE_PATH). A directory named more than once is loaded once.
// - modules: ModuleIndex::scan(module_search_paths(options.module_paths, {},
//   options.use_environment)), with module_paths and use_environment copied from `options`.
// - services: empty.
// Throws what Catalog::load() throws.
RuntimeContext make_runtime_context(const RuntimeOptions& options);

// Validates the configuration in `config`, adding every problem to `report`, in this order:
//   1-2. the configuration is read and checked (read_workflow_config(), with `options` as the
//        overrides);
//   3. its workflow definition is in the catalog (WORKFLOW_NOT_FOUND), and so are the message
//      types and capabilities the definition uses (CATALOG_ENTRY_NOT_FOUND);
//   4. the definition's graph is valid (graph/graph.hpp);
//   5. every step is bound and no other (BINDING_INVALID), every workflow input is bound and no
//      other (INPUT_BINDING_INVALID), and outputs.record names workflow outputs
//      (CONFIG_INVALID);
//   6. each step's module resolves on the search paths (MODULE_NOT_FOUND, PLATFORM_MISMATCH),
//      is compatible (compat/compat.hpp) and its library file exists and matches its pinned
//      digest (LIBRARY_NOT_FOUND, LIBRARY_DIGEST_MISMATCH, IO_ERROR); nothing is loaded;
//   7. each step's parameters satisfy its module's parameter schema (PARAMETERS_INVALID, located
//      at /steps/<step>/parameters<pointer> in the configuration), which then fills in defaults;
//   8. each input's data file exists and its source can be built (io/input_source.hpp);
//   9. the reproducibility section and the fingerprint are computed.
// Every problem is collected; a check is skipped only when it depends on one that failed (no
// definition: nothing after step 3; an unknown capability or message type: the graph, and the
// steps and inputs that use it; an error in an input's path or columns: its source). Diagnostics
// from the scan of the module search paths are added as warnings. Returns nullopt when any error
// was reported.
std::optional<ValidatedWorkflow> validate_workflow(const std::filesystem::path& config,
                                                   const RuntimeContext& context,
                                                   const WorkflowOptions& options,
                                                   ValidationReport& report);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_API_VALIDATION_HPP
