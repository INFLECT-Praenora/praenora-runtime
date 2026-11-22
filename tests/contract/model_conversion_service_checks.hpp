// The service checks of the model conversion service praenora.model-conversion@0
// (include/praenora/sdk/model_conversion.h, docs/specs/model-conversion-service.md): a
// parameterized GoogleTest suite, ModelConversionService, that loads a provider of the service
// through the runtime's provider loader and exercises its table: the header, describe against the
// manifest, exact translations and their reports, repeated and concurrent calls, the provider's
// rule refusals, engine failures, empty sources, the options and their pointers, unsupported
// formats and pairs, unknown modules, the source's UTF-8 and NUL checks, the size limits, NULL and
// refusing sinks, NULL and short host APIs, cancellation before the work and while a call waits
// for the provider's lock, and serialized conversions.
//
// The checks (model_conversion_service_checks.cpp) know nothing of a particular provider or format.
// Each parameter set, a ModelConversionCase, names a provider manifest, the conversions its
// describe() lists, translations it must produce exactly and sources it must refuse. A provider is
// checked by instantiating the suite in a source file of its own:
//
//   INSTANTIATE_TEST_SUITE_P(MyProvider, ModelConversionService, ::testing::Values(my_case()),
//                            praenora::test::model_conversion_case_name);
//
// model_conversion_service_test.cpp instantiates it for the fixture provider test-conversion.
#ifndef PRAENORA_TESTS_CONTRACT_MODEL_CONVERSION_SERVICE_CHECKS_HPP
#define PRAENORA_TESTS_CONTRACT_MODEL_CONVERSION_SERVICE_CHECKS_HPP

#include <praenora/sdk/model_conversion.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace praenora::test {

// One translation the provider must produce exactly.
struct ConversionExpectation {
    // Shown in test failures.
    std::string name;
    // The options' "from" and "to": a conversion that describe() lists.
    std::string from, to;
    // The source text.
    std::string source;
    // The options' "module"; empty: the option is left out (the source's main module).
    std::string module;
    // The report's "module": the module translated.
    std::string expected_module;
    // The translated text, byte for byte.
    std::string output;
    // The report's "warnings", in order.
    std::vector<std::string> warnings;
};

// A source the provider refuses: by one of its format rules (DATA_INVALID with details.rule), or
// for another reason that its format states (UNSUPPORTED with a pointer, for example).
struct ConversionRefusal {
    // Shown in test failures.
    std::string name;
    std::string from, to;
    std::string source;
    // The status of the refusal.
    praenora_status status = PRAENORA_STATUS_DATA_INVALID;
    // details.rule; empty: the refusal names no rule (details has no "rule").
    std::string rule;
    // details.pointer, when the refusal has one.
    std::optional<std::string> pointer;
};

// One parameter set of the service checks: a provider and what it must do.
struct ModelConversionCase {
    // The parameter set's name in the test names: letters, digits and '_' only.
    std::string name;
    // The manifest of the provider of praenora.model-conversion@0 that is checked.
    std::filesystem::path provider_manifest;
    // The engine object, as the manifest holds it and describe() reports it (JSON text).
    std::string engine_json;
    // describe()'s "conversions" as {from, to} pairs, in its order (sorted by from, then to).
    std::vector<std::pair<std::string, std::string>> conversions;
    // Translations the provider must produce exactly: at least one for each conversion. The first
    // is the one the checks of options, sinks, hosts and limits use.
    std::vector<ConversionExpectation> cases;
    // Sources the provider refuses by its format rules.
    std::vector<ConversionRefusal> refusals;
    // A source of format `failing_from` that the engine cannot read or translate: DATA_INVALID with
    // details.engine_error. It is converted to the first format the conversions pair with
    // failing_from. failing_line: the details.line expected, or 0 when it is not checked.
    std::string failing_from, failing_source;
    int failing_line = 0;
    // Optional: a source of format `multi_module_from` that has a module besides its main one; the
    // checks convert it without "module" and ask it for a module it does not have. Empty when the
    // provider's formats have one module only.
    std::string multi_module_from, multi_module_source;
    // Optional: a source of format `hold_from` whose translation holds the provider's lock for a
    // while: at least 300 ms, and several times the interval at which the provider polls
    // cancel_requested() while it waits for its lock. The checks of cancellation while waiting for
    // the lock and of serialized conversions use it. Empty: those checks are skipped.
    std::string hold_from, hold_source;
};

// What one call of convert() returned and wrote.
struct ConversionResult {
    praenora_status status = -1;
    std::string text, report, error;
};

// For tests of a provider beyond the checks: loads the provider of `provider_manifest` (once per
// process, as the runtime always does) and returns its praenora.model-conversion@0 table, or NULL
// when it offers none. Throws praenora::Error when the provider does not load.
const praenora_model_conversion_v0*
load_model_conversion_table(const std::filesystem::path& provider_manifest);

// Calls `table`'s convert() with `options_json` and `source`, without a host.
ConversionResult convert_source(const praenora_model_conversion_v0* table,
                                std::string_view options_json, std::string_view source);

// Shown in test failures instead of the bytes of the struct.
void PrintTo(const ModelConversionCase& conversion_case, std::ostream* out);

// The test-name suffix of a parameter set: its name.
std::string model_conversion_case_name(const ::testing::TestParamInfo<ModelConversionCase>& info);

// The suite. SetUp() loads the parameter set's provider (once per process, as the runtime always
// does) and takes its praenora.model-conversion@0 table.
class ModelConversionService : public ::testing::TestWithParam<ModelConversionCase> {
protected:
    void SetUp() override;

    // The table under test.
    const praenora_model_conversion_v0* table() const { return table_; }

private:
    const praenora_model_conversion_v0* table_ = nullptr;
};

} // namespace praenora::test

#endif // PRAENORA_TESTS_CONTRACT_MODEL_CONVERSION_SERVICE_CHECKS_HPP
