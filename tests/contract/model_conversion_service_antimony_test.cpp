// The service checks of praenora.model-conversion@0 (model_conversion_service_checks.hpp) against
// the antimony provider (src/providers/antimony), built with PRAENORA_WITH_ANTIMONY: Antimony to
// SBML and SBML to Antimony, exactly, for the models of tests/fixtures/models/antimony (a decay,
// a function definition, events, every kind of rule, a submodule used twice) and the SBML documents
// of tests/fixtures/models/sbml (Levels 2 and 3); the sources its rules refuse, and a layout
// request, which it refuses as UNSUPPORTED; an Antimony syntax error; and a source of several
// modules.
//
// The expected outputs, tests/fixtures/models/antimony/to-sbml/*.xml, to-antimony/*.ant and
// from-sbml/*.ant, were written once from this provider's own conversions (with
// praenora_conversion_print, tests/providers/conversion_print.cpp, and, for the module option,
// libAntimony as the provider configures it) and checked by hand against their sources: the
// species, compartments, parameters, reactions and rates, rules, initial assignments, events and
// function definitions of each are there, with their values. They are fixtures: no test rewrites
// them. A new libAntimony or libSBML that writes other bytes changes them, as it changes the engine
// object.
#include "model_conversion_service_checks.hpp"

#include "test_support.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

using praenora::test::ConversionExpectation;
using praenora::test::ConversionRefusal;
using praenora::test::ModelConversionCase;

std::string read_fixture(const std::string& relative) {
    std::ifstream in(praenora::test::fixture_path("models/" + relative), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

ConversionExpectation translation(std::string name, std::string from, std::string to,
                                  const std::string& source, const std::string& output,
                                  std::string expected_module, std::string module = "") {
    ConversionExpectation e;
    e.name = std::move(name);
    e.from = std::move(from);
    e.to = std::move(to);
    e.source = read_fixture(source);
    e.module = std::move(module);
    e.expected_module = std::move(expected_module);
    e.output = read_fixture(output);
    return e;
}

// The translations. The first, decay.ant to SBML, is the one the checks of options, sinks, hosts
// and limits use.
std::vector<ConversionExpectation> antimony_translations() {
    std::vector<ConversionExpectation> cases;
    // Antimony to SBML: flattened SBML Level 3 Version 2.
    for (const auto& [name, model] :
         std::vector<std::pair<std::string, std::string>>{{"Decay", "decay"},
                                                          {"Functions", "functions"},
                                                          {"Events", "events"},
                                                          {"Rules", "rules"},
                                                          {"SubmodelFlattened", "pair"}}) {
        const std::string file = name == "SubmodelFlattened" ? "submodel" : model;
        cases.push_back(translation(name + "ToSbml", "antimony", "sbml",
                                    "antimony/" + file + ".ant",
                                    "antimony/to-sbml/" + file + ".xml", model));
    }
    // The module option: a submodule translated alone.
    cases.push_back(translation("SubmoduleToSbml", "antimony", "sbml", "antimony/submodel.ant",
                                "antimony/to-sbml/submodel-decay_unit.xml", "decay_unit",
                                "decay_unit"));
    // SBML to Antimony: the SBML written above, back (function definitions kept).
    for (const auto& [name, model] :
         std::vector<std::pair<std::string, std::string>>{{"Decay", "decay"},
                                                          {"Functions", "functions"},
                                                          {"Events", "events"},
                                                          {"Rules", "rules"},
                                                          {"Submodel", "pair"}}) {
        const std::string file = name == "Submodel" ? "submodel" : model;
        cases.push_back(translation(name + "ToAntimony", "sbml", "antimony",
                                    "antimony/to-sbml/" + file + ".xml",
                                    "antimony/to-antimony/" + file + ".ant", model));
    }
    // SBML documents of other levels and versions, written by hand.
    for (const auto& [file, model] :
         std::vector<std::pair<std::string, std::string>>{{"decay", "decay"},
                                                          {"decay-l3v1", "decay_l3v1"},
                                                          {"decay-l2v4", "decay_l2v4"},
                                                          {"decay-with-event", "decay_with_event"},
                                                          {"rules", "rules"}}) {
        std::string name = "Sbml_" + file;
        for (char& ch : name) {
            if (ch == '-') {
                ch = '_';
            }
        }
        cases.push_back(translation(name, "sbml", "antimony", "sbml/" + file + ".xml",
                                    "antimony/from-sbml/" + file + ".ant", model));
    }
    return cases;
}

ConversionRefusal refusal(std::string name, std::string from, std::string to, std::string source,
                          std::string rule) {
    ConversionRefusal r;
    r.name = std::move(name);
    r.from = std::move(from);
    r.to = std::move(to);
    r.source = std::move(source);
    r.rule = std::move(rule);
    return r;
}

// A Level 3 SBML document that declares package `prefix` at `uri`, with required="<required>".
std::string sbml_with_package(const std::string& prefix, const std::string& uri,
                              const std::string& required) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<sbml xmlns=\"http://www.sbml.org/sbml/level3/version1/core\" xmlns:" +
           prefix + "=\"" + uri + "\" level=\"3\" version=\"1\" " + prefix + ":required=\"" +
           required +
           "\">\n"
           "  <model id=\"m\">\n"
           "    <listOfParameters>\n"
           "      <parameter id=\"k\" value=\"1\" constant=\"true\"/>\n"
           "    </listOfParameters>\n"
           "  </model>\n"
           "</sbml>\n";
}

// A source that asks for an automatic layout, whose positions SBMLNetwork seeds from the wall
// clock: UNSUPPORTED, details.reason "layout", no rule.
ConversionRefusal layout_request() {
    ConversionRefusal r = refusal("LayoutRequest", "antimony", "sbml",
                                  "J0: S1 -> S2; k * S1;\nS1 = 10; S2 = 0; k = 0.1;\n"
                                  "model.layout = on\n",
                                  "");
    r.status = PRAENORA_STATUS_UNSUPPORTED;
    return r;
}

// The sources the provider's rules refuse, before libAntimony or libSBML read them, or (package
// and comp) after libSBML read them, before libAntimony does; and a layout request, refused once
// libAntimony has written its SBML.
std::vector<ConversionRefusal> antimony_refusals() {
    const std::string comp = "http://www.sbml.org/sbml/level3/version1/comp/version1";
    return {
        refusal("Import", "antimony", "sbml", "x = 1\nimport \"does-not-exist.txt\"\n", "import"),
        refusal("ImportAfterANumber", "antimony", "sbml",
                "model M()\nend\nM.sboTerm = 1import \"does-not-exist.txt\"\n", "import"),
        refusal("Unterminated", "antimony", "sbml", "x = 1 /* a comment that never ends\n",
                "unterminated"),
        refusal("XmlDocument", "antimony", "sbml", read_fixture("sbml/decay.xml"), "xml"),
        refusal("NotDocument", "sbml", "antimony", "tests/fixtures/models/sbml/decay.xml",
                "not_document"),
        refusal("DocumentType", "sbml", "antimony",
                "<?xml version=\"1.0\"?>\n<!DOCTYPE sbml [<!ENTITY e SYSTEM "
                "\"does-not-exist.xml\">]>\n<sbml>&e;</sbml>\n",
                "document_type"),
        refusal("RequiredPackage", "sbml", "antimony",
                sbml_with_package("qual", "http://www.sbml.org/sbml/level3/version1/qual/version1",
                                  "true"),
                "package"),
        refusal("CompRequired", "sbml", "antimony", sbml_with_package("comp", comp, "true"),
                "comp"),
        refusal("CompNotRequired", "sbml", "antimony", sbml_with_package("comp", comp, "false"),
                "comp"),
        layout_request(),
    };
}

// A chain of `reactions` reactions, S0 -> S1 -> ... with mass-action rates: a model whose
// translation keeps the provider's lock for a while. On the macOS arm64 machine where this was
// measured, libAntimony 3.3 (a release build in every build of the provider) translated 700
// reactions to SBML in about 0.55 s, and about 1.3 s in a ThreadSanitizer build of the tests: far
// more than the 20 ms between the polls of a call that waits for the lock, and short enough for
// the checks that hold it three times. (500 reactions took about 0.3 s, 1000 about 0.8 s.)
std::string chain_of_reactions(int reactions) {
    std::string text = "// A chain of reactions, generated: S0 -> S1 -> ... -> S" +
                       std::to_string(reactions) + ".\nmodel *chain()\n";
    for (int i = 0; i < reactions; ++i) {
        const std::string n = std::to_string(i);
        text += "  J" + n + ": S" + n + " => S" + std::to_string(i + 1) + "; k" + n + " * S" + n +
                ";\n";
    }
    for (int i = 0; i <= reactions; ++i) {
        text += "  S" + std::to_string(i) + " = " + std::to_string(i % 7 + 1) + ";\n";
    }
    for (int i = 0; i < reactions; ++i) {
        text += "  k" + std::to_string(i) + " = 0." + std::to_string(i % 9 + 1) + ";\n";
    }
    return text + "end\n";
}

ModelConversionCase antimony_provider() {
    ModelConversionCase c;
    c.name = "Antimony";
    c.provider_manifest = praenora::test::utf8_path(PRAENORA_TEST_ANTIMONY_MANIFEST);
    c.engine_json = std::string("{\"function_definitions\":\"kept\",\"libsbml\":\"") +
                    PRAENORA_TEST_ANTIMONY_LIBSBML_VERSION + "\",\"name\":\"libantimony\"," +
                    "\"sbmlnetwork\":\"" + PRAENORA_TEST_SBMLNETWORK_VERSION + "\",\"version\":\"" +
                    PRAENORA_TEST_ANTIMONY_VERSION + "\"}";
    c.conversions = {{"antimony", "sbml"}, {"sbml", "antimony"}};
    c.cases = antimony_translations();
    c.refusals = antimony_refusals();
    // A syntax error. Its error has no details.line: libAntimony's count of lines, which its
    // message names, is not the source's after a block that spans lines
    // (AntimonyProvider.EngineFailureHasNoLineAfterMultiLineBlocks).
    c.failing_from = "antimony";
    c.failing_source = "x = 1\ny = = 2\nz = 3\n";
    c.failing_line = 0;
    // Modules decay_unit and pair, pair the main one (and libAntimony's __main).
    c.multi_module_from = "antimony";
    c.multi_module_source = read_fixture("antimony/submodel.ant");
    // A translation that holds the provider's own lock for the checks of cancellation while a call
    // waits for it and of serialized conversions.
    c.hold_from = "antimony";
    c.hold_source = chain_of_reactions(700);
    return c;
}

} // namespace

namespace praenora::test {

INSTANTIATE_TEST_SUITE_P(Antimony, ModelConversionService, ::testing::Values(antimony_provider()),
                         model_conversion_case_name);

} // namespace praenora::test
