// The cases of the contract checks (api/module_check.hpp): the test vectors, read and checked;
// the case the lifecycle checks use without usable vectors; and the documents of a case's calls
// into the module. Also the message helpers that the checks share.
#ifndef PRAENORA_RUNTIME_API_MODULE_CHECK_CASES_HPP
#define PRAENORA_RUNTIME_API_MODULE_CHECK_CASES_HPP

#include "catalog/catalog.hpp"
#include "discovery/manifest.hpp"
#include "document/schema.hpp"
#include "document/yaml_loader.hpp"

#include <praenora/runtime/error.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace praenora::detail {

// The step that the contract checks' instances are bound to.
inline constexpr const char* kCheckStep = "check";

// One tick of a test-vector case.
struct VectorTick {
    nlohmann::json inputs = nlohmann::json::object(); // port -> data
    std::optional<nlohmann::json> expected;           // port -> data, when the tick has an `expect`
    double relative = 0, absolute = 0;
};

// A test-vector case, or the case the lifecycle checks use without vectors (no ticks).
struct CheckCase {
    std::string label; // "case 'name'" or "case <n>"
    CapabilityRef capability;
    const CapabilityDef* definition = nullptr;
    nlohmann::json parameters = nlohmann::json::object(); // with the parameters_schema's defaults
    std::uint64_t seed = 0;
    std::string unit = "1";
    double start = 0.0, period = 1.0;
    std::vector<VectorTick> ticks;
    std::string fingerprint; // SHA-256 of the case's canonical JSON
};

// The test vectors as read: their cases, or what is wrong with them.
struct TestVectors {
    std::vector<CheckCase> cases;
    std::string invalid; // empty when the vectors are usable
};

// Reads the test vectors in `path` (a module-test-vectors document, its schema's defaults
// applied) and checks them for the module of `manifest`, whose capabilities and their ports'
// message types `catalog` defines: each case's capability is one the manifest lists, its
// parameters satisfy the parameters_schema (whose defaults are then applied), its ticks' inputs
// and expected outputs name ports of the capability, every required input is present, and every
// datum satisfies its port's message type. The first problem, located in the file, makes the
// vectors invalid. Throws praenora::Error(IO_ERROR) when the file cannot be read.
TestVectors read_test_vectors(const std::filesystem::path& path, const ModuleManifest& manifest,
                              const Catalog& catalog);

// The case of the lifecycle checks without usable test vectors: the manifest's first capability
// with the parameters_schema's defaults, seed 0 and the time base unit "1", start 0, period 1.
// nullopt, with `problem` saying why, when the defaults do not satisfy the schema;
// `vectors_invalid` tells the message whether there were vectors, which were invalid.
std::optional<CheckCase> default_case(const ModuleManifest& manifest, const Catalog& catalog,
                                      bool vectors_invalid, std::string& problem);

// The documents of a case's calls, as canonical JSON: create (the step kCheckStep, the case's
// capability and parameters, the module), initialize (the case's fingerprint, seed and time base)
// and the invocation of tick `t` (its logical time the end of window t, each input's data in an
// envelope of its port's message type with source inputs.<port>).
std::string create_document(const CheckCase& c, const ModuleManifest& manifest);
std::string initialize_document(const CheckCase& c);
std::string invocation_document(const CheckCase& c, std::size_t t);

// Message helpers of the contract checks.
namespace module_check_text {

// "<file>:<line>:<column>: <pointer>: ", leaving out the parts that are not set.
std::string where(const SourceLocation& location);

// An error's message, preceded by its file, line and column unless the message names the file.
std::string text_of(const Error& error);

// The first of `issues` (not empty), located in `document`, with the number of the others.
std::string first_issue(const LoadedDocument& document, const std::vector<SchemaIssue>& issues);

} // namespace module_check_text

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_API_MODULE_CHECK_CASES_HPP
