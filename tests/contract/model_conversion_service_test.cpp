// The service checks of praenora.model-conversion@0 (model_conversion_service_checks.hpp) against
// the fixture provider test-conversion, which converts between two toy formats of one model:
// "test-decay", test-simulation's JSON model format, and "test-decay-text", a line format with
// comments, the directive "hold <milliseconds>" (which holds the provider's lock that long) and the
// rule "import" (a line that starts with the word import refuses the source). Beyond the checks,
// TestConversionProvider tests what the parameter set cannot hold: a second engine failure.
#include "model_conversion_service_checks.hpp"

#include "provider_fixtures.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace {

using praenora::test::ConversionExpectation;
using praenora::test::ConversionRefusal;
using praenora::test::ModelConversionCase;

// The translations: both directions, comments, a hold, a byte order mark and CR LF line ends, an
// explicit main module, non-ASCII text in a comment, and numbers in canonical JSON form.
std::vector<ConversionExpectation> toy_translations() {
    std::vector<ConversionExpectation> cases;

    ConversionExpectation text;
    text.name = "TextWithCommentsAndHold";
    text.from = "test-decay-text";
    text.to = "test-decay";
    text.source = "# Two states that decay on their own.\n"
                  "state x 10    # x starts at 10\n"
                  "\n"
                  "state y x0\n"
                  "parameter k_x 0.5\n"
                  "\tparameter  k_y\t1.5\n"
                  "parameter x0 4\n"
                  "hold 0\n";
    text.expected_module = "main";
    text.output = R"({"parameters":{"k_x":0.5,"k_y":1.5,"x0":4.0},"states":{"x":10.0,"y":"x0"}})";
    text.warnings = {"hold 0 ignored in output"};
    cases.push_back(text);

    ConversionExpectation model;
    model.name = "JsonToText";
    model.from = "test-decay";
    model.to = "test-decay-text";
    model.source = R"({"states": {"y": "x0", "x": 10},
                       "parameters": {"x0": 4, "k_y": 1.5, "k_x": 0.5}})";
    model.expected_module = "main";
    model.output = "state x 10.0\n"
                   "state y x0\n"
                   "parameter k_x 0.5\n"
                   "parameter k_y 1.5\n"
                   "parameter x0 4.0\n";
    cases.push_back(model);

    ConversionExpectation bom_text;
    bom_text.name = "TextWithBomAndCrLf";
    bom_text.from = "test-decay-text";
    bom_text.to = "test-decay";
    bom_text.source = "\xEF\xBB\xBFstate x 10\r\nparameter k_x 0.5 # rate\r\nhold 1\r\nhold 2";
    bom_text.module = "main";
    bom_text.expected_module = "main";
    bom_text.output = R"({"parameters":{"k_x":0.5},"states":{"x":10.0}})";
    bom_text.warnings = {"hold 1 ignored in output", "hold 2 ignored in output"};
    cases.push_back(bom_text);

    ConversionExpectation bom_model;
    bom_model.name = "JsonWithBomAndCrLf";
    bom_model.from = "test-decay";
    bom_model.to = "test-decay-text";
    bom_model.source = "\xEF\xBB\xBF{\"states\": {\"x\": 10},\r\n \"parameters\": {\"k_x\": 0.5},"
                       "\r\n \"events\": []}\r\n";
    bom_model.module = "main";
    bom_model.expected_module = "main";
    bom_model.output = "state x 10.0\nparameter k_x 0.5\n";
    cases.push_back(bom_model);

    ConversionExpectation comments;
    comments.name = "NonAsciiCommentsAndImportInComments";
    comments.from = "test-decay-text";
    comments.to = "test-decay";
    comments.source = "# d\xC3\xA9"
                      "croissance \xE2\x80\x94 import other\n"
                      "state x 1e-3   # import here is a comment\n"
                      "parameter k_x 2.5e+2\n";
    comments.expected_module = "main";
    comments.output = R"({"parameters":{"k_x":250.0},"states":{"x":0.001}})";
    cases.push_back(comments);

    ConversionExpectation numbers;
    numbers.name = "NumbersInCanonicalForm";
    numbers.from = "test-decay";
    numbers.to = "test-decay-text";
    numbers.source = R"({"states": {"a": 0.1, "b": 1e300, "c": -0.0},
                         "parameters": {"k_a": 1.5e-7, "k_b": 123456789012345680, "k_c": 0}})";
    numbers.expected_module = "main";
    numbers.output = "state a 0.1\n"
                     "state b 1e+300\n"
                     "state c -0.0\n"
                     "parameter k_a 1.5e-07\n"
                     "parameter k_b 1.2345678901234568e+17\n"
                     "parameter k_c 0.0\n";
    cases.push_back(numbers);
    return cases;
}

// The sources test-conversion refuses: the rule "import" (also where the line is indented, and
// before a line the engine would refuse: a refused source never reaches the engine), and a
// test-decay model with events, which test-decay-text cannot hold.
std::vector<ConversionRefusal> toy_refusals() {
    std::vector<ConversionRefusal> refusals;
    ConversionRefusal import;
    import.name = "Import";
    import.from = "test-decay-text";
    import.to = "test-decay";
    import.source = "state x 1\nimport other.txt\nparameter k_x 0.5\n";
    import.rule = "import";
    refusals.push_back(import);

    ConversionRefusal indented = import;
    indented.name = "IndentedImport";
    indented.source = "state x 1\nparameter k_x 0.5\n  \timport\n";
    refusals.push_back(indented);

    ConversionRefusal before_malformed = import;
    before_malformed.name = "ImportAfterAMalformedLine";
    before_malformed.source = "state x\r\nimport y\r\n";
    refusals.push_back(before_malformed);

    ConversionRefusal events;
    events.name = "Events";
    events.from = "test-decay";
    events.to = "test-decay-text";
    events.source = R"({"states": {"x": 1}, "parameters": {"k_x": 0.5},
                        "events": [{"time": 1, "state": "x", "value": 2}]})";
    events.status = PRAENORA_STATUS_UNSUPPORTED;
    events.pointer = "/events";
    refusals.push_back(events);
    return refusals;
}

ModelConversionCase toy_formats() {
    ModelConversionCase c;
    c.name = "ToyFormats";
    c.provider_manifest = praenora::test::fixture_provider_manifest("test-conversion");
    c.engine_json = R"({"name":"test-conversion-engine","version":"1.0.0"})";
    c.conversions = {{"test-decay", "test-decay-text"}, {"test-decay-text", "test-decay"}};
    c.cases = toy_translations();
    c.refusals = toy_refusals();
    // Line 2 has no value.
    c.failing_from = "test-decay-text";
    c.failing_source = "state x 10\nparameter k_x\n";
    c.failing_line = 2;
    // The formats have one module, "main".
    c.hold_from = "test-decay-text";
    c.hold_source = "state x 1\nparameter k_x 0.5\nhold 300\n";
    return c;
}

} // namespace

namespace praenora::test {

INSTANTIATE_TEST_SUITE_P(TestConversion, ModelConversionService, ::testing::Values(toy_formats()),
                         model_conversion_case_name);

} // namespace praenora::test

TEST(TestConversionProvider, NumberOverflowIsAnEngineFailure) {
    // A test-decay number too large for a double is malformed, as a syntax error is: DATA_INVALID
    // with details.engine_error. nlohmann/json reports it with another exception than a syntax
    // error, one without a byte offset, so the error has no line.
    const praenora_model_conversion_v0* t = praenora::test::load_model_conversion_table(
        praenora::test::fixture_provider_manifest("test-conversion"));
    ASSERT_NE(t, nullptr);
    const praenora::test::ConversionResult result =
        praenora::test::convert_source(t, R"({"from":"test-decay","to":"test-decay-text"})",
                                       R"({"states":{"x":1e400},"parameters":{"k_x":1}})");
    EXPECT_EQ(result.status, PRAENORA_STATUS_DATA_INVALID) << result.error;
    EXPECT_EQ(result.text, "");
    EXPECT_EQ(result.report, "");
    const nlohmann::json document = nlohmann::json::parse(result.error, nullptr, false);
    ASSERT_TRUE(document.is_object()) << result.error;
    const nlohmann::json details = document.value("details", nlohmann::json::object());
    EXPECT_EQ(details.value("service", ""), "praenora.model-conversion") << result.error;
    ASSERT_TRUE(details.contains("engine_error")) << result.error;
    EXPECT_EQ(details.at("engine_error").value("type", ""), "test-conversion") << result.error;
    EXPECT_FALSE(details.at("engine_error").value("message", "").empty()) << result.error;
    EXPECT_FALSE(details.contains("line")) << result.error;
    EXPECT_FALSE(details.contains("rule")) << result.error;
}
