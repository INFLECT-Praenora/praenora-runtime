// What the antimony provider (src/providers/antimony) does beyond the rules every provider of
// praenora.model-conversion@0 follows (model_conversion_service_checks.hpp), through libAntimony
// itself: its check of imports, which keeps libAntimony from opening files; the round trip of the
// fixture models; its main module and its module option; function definitions; libSBML's and
// libAntimony's errors and warnings; its engine identity; byte order marks, CR LF line ends and
// text that is not ASCII; SBML that names other files; and layout (docs/specs/
// model-conversion-service.md, the antimony provider's section). Built with PRAENORA_WITH_ANTIMONY
// only.
#include "model_conversion_service_checks.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::test::ConversionResult;

std::string read_fixture(const std::string& relative) {
    std::ifstream in(praenora::test::fixture_path("models/" + relative), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

const praenora_model_conversion_v0* provider() {
    static const praenora_model_conversion_v0* const table =
        praenora::test::load_model_conversion_table(
            praenora::test::utf8_path(PRAENORA_TEST_ANTIMONY_MANIFEST));
    return table;
}

ConversionResult convert(const std::string& from, const std::string& to, const std::string& source,
                         const std::string& module = "") {
    json options = {{"from", from}, {"to", to}};
    if (!module.empty()) {
        options["module"] = module;
    }
    return praenora::test::convert_source(provider(), options.dump(), source);
}

std::string shown(const ConversionResult& result) {
    return "status " + std::to_string(result.status) + ", error '" + result.error + "', report '" +
           result.report + "', text of " + std::to_string(result.text.size()) + " bytes";
}

json document(const std::string& text) { return json::parse(text, nullptr, false); }

json details_of(const ConversionResult& result) {
    const json error = document(result.error);
    return error.is_object() ? error.value("details", json::object()) : json::object();
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// The source with a UTF-8 byte order mark, with CR LF line ends, or both.
std::string with_byte_order_mark(const std::string& source) { return "\xEF\xBB\xBF" + source; }

std::string with_crlf(const std::string& source) {
    std::string result;
    for (const char ch : source) {
        if (ch == '\n') {
            result += '\r';
        }
        result += ch;
    }
    return result;
}

// The fixture models of tests/fixtures/models/antimony.
const std::vector<std::string>& fixture_models() {
    static const std::vector<std::string> models = {"decay", "functions", "events", "rules",
                                                    "submodel"};
    return models;
}

// A directory of its own below the system's temporary directory, removed when it goes. Its path
// holds no space, so that it can be named in a file: URI as it is.
class OutsideDirectory {
public:
    OutsideDirectory() {
        std::random_device device;
        path_ = fs::temp_directory_path() /
                ("praenora-antimony-" + std::to_string(device()) + std::to_string(device()));
        fs::create_directories(path_);
    }
    OutsideDirectory(const OutsideDirectory&) = delete;
    OutsideDirectory& operator=(const OutsideDirectory&) = delete;
    ~OutsideDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    // Writes `text` to file `name` in the directory and returns its absolute path, as UTF-8.
    std::string write(const std::string& name, const std::string& text) const {
        const fs::path file = path_ / name;
        std::ofstream(file, std::ios::binary) << text;
        const auto utf8 = file.u8string();
        return std::string(utf8.begin(), utf8.end());
    }

private:
    fs::path path_;
};

const std::string kSbmlStart =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<sbml xmlns=\"http://www.sbml.org/sbml/level3/version2/core\" level=\"3\" version=\"2\">\n";

} // namespace

TEST(AntimonyProvider, ImportOfAMissingFileIsTheServicesRefusal) {
    // Each of these makes libAntimony open the file it names (and report "Could not open" when it
    // is missing); the provider refuses them before libAntimony reads them.
    for (const std::string source : {
             "import \"does-not-exist.txt\"\n",
             "x = 1\nimport \"does-not-exist.txt\"\n",
             "x = 1; import \"does-not-exist.txt\"\n",
             "x = 1;import ```does-not-exist.txt```\n",
             "x = 1;import ``\n",
             "model M()\nend\nM.sboTerm = 1import \"does-not-exist.txt\"\n",
             "x is \"a\\\" // \"; import \"does-not-exist.txt\"\n",
             "// a comment\rimport \"does-not-exist.txt\"\n",
             "\xEF\xBB\xBFimport \"does-not-exist.txt\"\r\n",
         }) {
        SCOPED_TRACE(source);
        const ConversionResult result = convert("antimony", "sbml", source);
        EXPECT_EQ(result.status, PRAENORA_STATUS_DATA_INVALID) << shown(result);
        const json details = details_of(result);
        EXPECT_EQ(details.value("rule", ""), "import") << result.error;
        EXPECT_FALSE(details.contains("engine_error")) << result.error;
        EXPECT_FALSE(contains(result.error, "Could not open")) << result.error;
    }
}

TEST(AntimonyProvider, CommentedAndQuotedImportsReachLibAntimonyHarmlessly) {
    // Every form the check of imports lets pass, with a file name that does not exist, converts:
    // libAntimony ignores what the check takes for a comment, a string or a block.
    for (const std::string source : {
             "// import \"does-not-exist.txt\"\nx = 1\n",
             "# import \"does-not-exist.txt\"\nx = 1\n",
             "x = 1 // import \"does-not-exist.txt\"\n",
             "x = 1 # import \"does-not-exist.txt\"\r\n",
             "/* import \"does-not-exist.txt\" */\nx = 1\n",
             "/*\nimport \"does-not-exist.txt\"\n*/\nx = 1\n",
             "x = 1\nx is \"import \\\"does-not-exist.txt\\\"\"\n",
             "x = 1\nx is \"a // b\"\n",
             "x = 1\nx notes ```\nimport \"does-not-exist.txt\"\n```\n",
             "x = 1\nx notes ```import ``x`` \"does-not-exist.txt\"```\n",
             "importance = 1\nk_import = 2\nimport2 = 3\nx1import = 4\n",
         }) {
        SCOPED_TRACE(source);
        const ConversionResult result = convert("antimony", "sbml", source);
        EXPECT_EQ(result.status, PRAENORA_STATUS_OK) << shown(result);
    }
    // Around an import of a missing file, each of these beginnings and ends gives a source that
    // the provider refuses by a rule, converts, or that libAntimony cannot read for another reason:
    // never one that makes libAntimony look for the file.
    const std::vector<std::string> before = {
        "",           "x = 1;",    "x = 1\n",      "1",        "x = 1.5", ".5",   "0x1", "1e5",
        "x.",         "_",         "k_",           "-",        "\\ ",     "// ",  "# ",  "/* ",
        "/* */",      "\"",        "x is \"a\\\"", "\"\\\\\"", "```",     "````", "``",  "`",
        "x = 1 //\r", "x = 1 #\r", "/*/ */",       "\xC3\xA9"};
    const std::vector<std::string> after = {"", " */", "\"", "```", "\n*/", "\\\"", "x"};
    for (const std::string& start : before) {
        for (const std::string& end : after) {
            const std::string source = start + "import \"does-not-exist.txt\"" + end + "\n";
            const ConversionResult result = convert("antimony", "sbml", source);
            EXPECT_FALSE(contains(result.error, "Could not open"))
                << source << ": " << shown(result);
            if (result.status == PRAENORA_STATUS_DATA_INVALID) {
                const json details = details_of(result);
                EXPECT_TRUE(details.contains("rule") || details.contains("engine_error"))
                    << source << ": " << result.error;
            } else {
                EXPECT_EQ(result.status, PRAENORA_STATUS_OK) << source << ": " << shown(result);
            }
        }
    }
}

TEST(AntimonyProvider, RoundTripIsAFixpoint) {
    // Antimony to SBML, back to Antimony and to SBML again: the second SBML is the first, byte for
    // byte.
    for (const std::string& model : fixture_models()) {
        SCOPED_TRACE(model);
        const ConversionResult first =
            convert("antimony", "sbml", read_fixture("antimony/" + model + ".ant"));
        ASSERT_EQ(first.status, PRAENORA_STATUS_OK) << shown(first);
        const ConversionResult back = convert("sbml", "antimony", first.text);
        ASSERT_EQ(back.status, PRAENORA_STATUS_OK) << shown(back);
        const ConversionResult second = convert("antimony", "sbml", back.text);
        ASSERT_EQ(second.status, PRAENORA_STATUS_OK) << shown(second);
        EXPECT_EQ(second.text, first.text);
        EXPECT_EQ(document(second.report).at("module"), document(first.report).at("module"));
    }
}

TEST(AntimonyProvider, MainModuleFollowsTheRule) {
    // The module marked with '*'; else __main, when statements stand outside every module; else
    // the last module.
    for (const auto& [file, main] : std::vector<std::pair<std::string, std::string>>{
             {"marked", "b"}, {"top-level", "__main"}, {"last", "c"}}) {
        SCOPED_TRACE(file);
        const ConversionResult result =
            convert("antimony", "sbml", read_fixture("antimony/three-mains/" + file + ".ant"));
        ASSERT_EQ(result.status, PRAENORA_STATUS_OK) << shown(result);
        EXPECT_EQ(document(result.report).value("module", ""), main) << result.report;
        EXPECT_TRUE(contains(result.text, "<model metaid=\"" + main + "\" id=\"" + main + "\">"))
            << result.text;
    }
}

TEST(AntimonyProvider, ModuleOptionSelectsSubmodule) {
    const std::string source = read_fixture("antimony/submodel.ant");
    // The submodule alone.
    const ConversionResult unit = convert("antimony", "sbml", source, "decay_unit");
    ASSERT_EQ(unit.status, PRAENORA_STATUS_OK) << shown(unit);
    EXPECT_EQ(document(unit.report).value("module", ""), "decay_unit");
    EXPECT_EQ(unit.text, read_fixture("antimony/to-sbml/submodel-decay_unit.xml"));
    EXPECT_FALSE(contains(unit.text, "A_J0")) << unit.text;
    // The main module, named or not: flattened, with the submodules' elements prefixed.
    const ConversionResult main = convert("antimony", "sbml", source);
    const ConversionResult named = convert("antimony", "sbml", source, "pair");
    ASSERT_EQ(main.status, PRAENORA_STATUS_OK) << shown(main);
    EXPECT_EQ(named.text, main.text);
    EXPECT_EQ(named.report, main.report);
    EXPECT_TRUE(contains(main.text, "id=\"A_J0\"")) << main.text;
    // An instance of a module is not a module.
    const ConversionResult instance = convert("antimony", "sbml", source, "A");
    EXPECT_EQ(instance.status, PRAENORA_STATUS_NOT_FOUND) << shown(instance);
    EXPECT_EQ(details_of(instance).value("module", ""), "A") << instance.error;
    // An SBML document has the module of its model.
    const ConversionResult sbml =
        convert("sbml", "antimony", read_fixture("sbml/decay-l2v4.xml"), "decay_l2v4");
    ASSERT_EQ(sbml.status, PRAENORA_STATUS_OK) << shown(sbml);
    EXPECT_EQ(sbml.text, read_fixture("antimony/from-sbml/decay-l2v4.ant"));
}

TEST(AntimonyProvider, FunctionDefinitionsAreKept) {
    // SBML to Antimony keeps a function definition and the calls of it: libAntimony would expand
    // the calls and drop the definition by default.
    const ConversionResult sbml =
        convert("antimony", "sbml", read_fixture("antimony/functions.ant"));
    ASSERT_EQ(sbml.status, PRAENORA_STATUS_OK) << shown(sbml);
    EXPECT_TRUE(contains(sbml.text, "<functionDefinition id=\"hill\"")) << sbml.text;
    const ConversionResult antimony = convert("sbml", "antimony", sbml.text);
    ASSERT_EQ(antimony.status, PRAENORA_STATUS_OK) << shown(antimony);
    EXPECT_TRUE(contains(antimony.text, "function hill(s, vmax, km, n)")) << antimony.text;
    EXPECT_TRUE(contains(antimony.text, "hill(S, vmax, km, 2)")) << antimony.text;
}

TEST(AntimonyProvider, SbmlErrorsHaveLineAndColumn) {
    // libSBML's first error, located in the source as given: a byte order mark and the white space
    // before the document, which the provider leaves out of what libSBML reads, count too.
    const std::string missing_attribute = kSbmlStart + "  <model id=\"m\">\n"
                                                       "    <listOfParameters>\n"
                                                       "      <parameter id=\"k\" value=\"1\"/>\n"
                                                       "    </listOfParameters>\n"
                                                       "  </model>\n"
                                                       "</sbml>\n";
    const std::string not_well_formed = kSbmlStart +
                                        "  <model id=\"m\">\n"
                                        "    <listOfParameters>\n"
                                        "      <parameter id=\"k\" value=\"1\" constant=\"true\">\n"
                                        "    </listOfParameters>\n"
                                        "  </model>\n"
                                        "</sbml>\n";
    struct Expected {
        std::string source;
        int line, column;
    };
    // Without its XML declaration, and on one line after white space.
    const std::string undeclared = not_well_formed.substr(not_well_formed.find('\n') + 1);
    std::string one_line = missing_attribute.substr(missing_attribute.find('\n') + 1);
    one_line.erase(std::remove(one_line.begin(), one_line.end(), '\n'), one_line.end());
    const std::size_t parameter = one_line.find("<parameter");
    for (const Expected& e : std::vector<Expected>{
             {missing_attribute, 5, 7},
             {not_well_formed, 6, 7},
             {"\xEF\xBB\xBF\n\n" + not_well_formed, 8, 7},
             {"  " + with_crlf(missing_attribute), 5, 7},
             {undeclared, 5, 7},
             {"\xEF\xBB\xBF\r\n\t " + undeclared, 6, 7},
             {"   " + one_line, 1, static_cast<int>(parameter + 4)},
             {"\n \t" + one_line, 2, static_cast<int>(parameter + 3)},
         }) {
        SCOPED_TRACE(e.source);
        const ConversionResult result = convert("sbml", "antimony", e.source);
        EXPECT_EQ(result.status, PRAENORA_STATUS_DATA_INVALID) << shown(result);
        const json details = details_of(result);
        ASSERT_TRUE(details.contains("engine_error")) << result.error;
        EXPECT_EQ(details.at("engine_error").value("type", ""), "libsbml") << result.error;
        EXPECT_FALSE(details.at("engine_error").value("message", "").empty()) << result.error;
        EXPECT_EQ(details.value("line", 0), e.line) << result.error;
        EXPECT_EQ(details.value("column", 0), e.column) << result.error;
        EXPECT_FALSE(details.contains("rule")) << result.error;
    }
}

TEST(AntimonyProvider, WarningsAreReported) {
    // Two modules marked as the main one: libAntimony warns, and takes the last.
    const ConversionResult warned =
        convert("antimony", "sbml", "model *a()\n  x = 1\nend\nmodel *c()\n  z = 3\nend\n");
    ASSERT_EQ(warned.status, PRAENORA_STATUS_OK) << shown(warned);
    EXPECT_EQ(document(warned.report),
              json({{"from", "antimony"},
                    {"to", "sbml"},
                    {"module", "c"},
                    {"warnings",
                     {"Warning: changing main module for this file to be c instead of 'a'."}}}))
        << warned.report;
    // Without a warning, the list is empty.
    const ConversionResult quiet = convert("antimony", "sbml", read_fixture("antimony/decay.ant"));
    ASSERT_EQ(quiet.status, PRAENORA_STATUS_OK) << shown(quiet);
    EXPECT_EQ(document(quiet.report).at("warnings"), json::array()) << quiet.report;
}

TEST(AntimonyProvider, EngineObjectMatchesTheLibraries) {
    // describe() reports the engine object of the manifest, with the versions that the build's
    // probe had the libraries report (cmake/PraenoraAntimony.cmake); the provider checks them
    // against the libraries it links when it starts.
    praenora_model_conversion_v0 const* t = provider();
    std::string described;
    praenora_sink sink{
        &described,
        [](void* context, const char* data, std::size_t size) noexcept -> praenora_status {
            static_cast<std::string*>(context)->append(data, size);
            return PRAENORA_STATUS_OK;
        }};
    ASSERT_EQ(t->describe(t->context, &sink), PRAENORA_STATUS_OK);
    const json engine = document(described).at("engine");
    EXPECT_EQ(engine, json({{"name", "libantimony"},
                            {"version", PRAENORA_TEST_ANTIMONY_VERSION},
                            {"libsbml", PRAENORA_TEST_ANTIMONY_LIBSBML_VERSION},
                            {"sbmlnetwork", PRAENORA_TEST_SBMLNETWORK_VERSION},
                            {"function_definitions", "kept"}}))
        << described;
    // What libAntimony and libSBML write into their outputs, at run time, names the same versions.
    const ConversionResult sbml = convert("antimony", "sbml", read_fixture("antimony/decay.ant"));
    ASSERT_EQ(sbml.status, PRAENORA_STATUS_OK) << shown(sbml);
    EXPECT_TRUE(contains(sbml.text, std::string("<!-- Created by libAntimony version v") +
                                        PRAENORA_TEST_ANTIMONY_VERSION + " with libSBML version " +
                                        PRAENORA_TEST_ANTIMONY_LIBSBML_VERSION + ". -->"))
        << sbml.text.substr(0, 200);
    const ConversionResult antimony = convert("sbml", "antimony", sbml.text);
    ASSERT_EQ(antimony.status, PRAENORA_STATUS_OK) << shown(antimony);
    EXPECT_EQ(antimony.text.rfind(std::string("// Created by libAntimony v") +
                                      PRAENORA_TEST_ANTIMONY_VERSION + "\n",
                                  0),
              0u)
        << antimony.text.substr(0, 200);
}

TEST(AntimonyProvider, BomAndCrLfSourcesConvertLikeTheirPlainForms) {
    // A UTF-8 byte order mark and CR LF line ends change nothing in Antimony and in SBML, also in
    // the text of a block that spans lines (a note), where libAntimony itself would keep the CR.
    std::vector<std::pair<std::string, std::string>> sources; // {from, source}
    for (const std::string& model : fixture_models()) {
        sources.emplace_back("antimony", read_fixture("antimony/" + model + ".ant"));
        sources.emplace_back("sbml", read_fixture("antimony/to-sbml/" + model + ".xml"));
    }
    sources.emplace_back("sbml", read_fixture("sbml/decay-l2v4.xml"));
    sources.emplace_back("antimony", "model *m()\n  x = 1;\n  x notes ```\n    First line.\n"
                                     "    Second line.\n  ```\nend\n");
    for (const auto& [from, source] : sources) {
        SCOPED_TRACE(from + ": " + source.substr(0, 120));
        const std::string to = from == "antimony" ? "sbml" : "antimony";
        const ConversionResult plain = convert(from, to, source);
        ASSERT_EQ(plain.status, PRAENORA_STATUS_OK) << shown(plain);
        for (const std::string& variant : {with_byte_order_mark(source), with_crlf(source),
                                           with_byte_order_mark(with_crlf(source))}) {
            const ConversionResult result = convert(from, to, variant);
            ASSERT_EQ(result.status, PRAENORA_STATUS_OK) << shown(result);
            EXPECT_EQ(result.text, plain.text);
            EXPECT_EQ(result.report, plain.report);
        }
    }
    // White space before an SBML document's XML declaration, which libSBML alone would refuse.
    const std::string decay = read_fixture("sbml/decay.xml");
    const ConversionResult plain = convert("sbml", "antimony", decay);
    const ConversionResult spaced = convert("sbml", "antimony", " \r\n\t\n" + decay);
    ASSERT_EQ(spaced.status, PRAENORA_STATUS_OK) << shown(spaced);
    EXPECT_EQ(spaced.text, plain.text);
}

TEST(AntimonyProvider, NonAsciiTextSurvivesRoundTrip) {
    // A display name, a note and a comment that are not ASCII: their bytes reach SBML and come
    // back.
    const std::string name = "d\xC3\xA9"
                             "croissance \xE2\x80\x94 x";
    const std::string note = "\xC3\xBCn\xC3\xAF"
                             "c\xC3\xB6"
                             "d\xC3\xA9";
    const std::string source = "// \xCE\xBB model\nmodel *m()\n  x = 1;\n  x is \"" + name +
                               "\";\n  x notes ```" + note + "```\nend\n";
    const ConversionResult sbml = convert("antimony", "sbml", source);
    ASSERT_EQ(sbml.status, PRAENORA_STATUS_OK) << shown(sbml);
    EXPECT_TRUE(contains(sbml.text, "name=\"" + name + "\"")) << sbml.text;
    EXPECT_TRUE(contains(sbml.text, note)) << sbml.text;
    const ConversionResult antimony = convert("sbml", "antimony", sbml.text);
    ASSERT_EQ(antimony.status, PRAENORA_STATUS_OK) << shown(antimony);
    EXPECT_TRUE(contains(antimony.text, "x is \"" + name + "\"")) << antimony.text;
    EXPECT_TRUE(contains(antimony.text, note)) << antimony.text;
    // An SBML name attribute that is not ASCII.
    const std::string named = kSbmlStart + "  <model id=\"m\" name=\"" + name +
                              "\">\n    <listOfParameters>\n      <parameter id=\"k\" name=\"" +
                              note +
                              "\" value=\"1\" constant=\"true\"/>\n    </listOfParameters>\n"
                              "  </model>\n</sbml>\n";
    const ConversionResult from_sbml = convert("sbml", "antimony", named);
    ASSERT_EQ(from_sbml.status, PRAENORA_STATUS_OK) << shown(from_sbml);
    EXPECT_TRUE(contains(from_sbml.text, "k is \"" + note + "\"")) << from_sbml.text;
}

TEST(AntimonyProvider, EngineErrorsAreValidUtf8) {
    // libAntimony quotes the first byte of a character it cannot read, which is not UTF-8 alone:
    // the error document has U+FFFD in its place, and is valid JSON.
    const ConversionResult result = convert("antimony", "sbml", "x = 1\n\xC3\xA9 = 2\n");
    EXPECT_EQ(result.status, PRAENORA_STATUS_DATA_INVALID) << shown(result);
    const json error = json::parse(result.error); // throws on text that is not UTF-8
    const json& details = error.at("details");
    EXPECT_EQ(details.at("engine_error").at("type"), "libantimony") << result.error;
    EXPECT_TRUE(
        contains(details.at("engine_error").at("message").get<std::string>(), "\xEF\xBF\xBD"))
        << result.error;
    EXPECT_FALSE(details.contains("line")) << result.error;
    EXPECT_TRUE(contains(error.at("message").get<std::string>(), "\xEF\xBF\xBD")) << result.error;
}

TEST(AntimonyProvider, EngineFailureHasNoLineAfterMultiLineBlocks) {
    // libAntimony counts no line end inside a block comment or a triple-backtick block, so the line
    // its message names is not the source's after such a block: the error document has no
    // details.line, and its engine_error.message is libAntimony's message, which names
    // libAntimony's own line. Here the syntax error is on line 9 of the source; libAntimony says
    // line 4.
    const std::string source = "/* A model\n"
                               "   with a header\n"
                               "   on three lines. */\n"
                               "x = 1\n"
                               "x notes ```\n"
                               "  First line.\n"
                               "  Second line.\n"
                               "```\n"
                               "y = = 2\n";
    const ConversionResult result = convert("antimony", "sbml", source);
    EXPECT_EQ(result.status, PRAENORA_STATUS_DATA_INVALID) << shown(result);
    const json details = details_of(result);
    EXPECT_FALSE(details.contains("line")) << result.error;
    EXPECT_FALSE(details.contains("column")) << result.error;
    ASSERT_TRUE(details.contains("engine_error")) << result.error;
    EXPECT_EQ(details.at("engine_error").value("type", ""), "libantimony") << result.error;
    EXPECT_EQ(details.at("engine_error").value("message", ""),
              "Error in model string, line 4:  syntax error, unexpected number, expecting '='")
        << result.error;
}

TEST(AntimonyProvider, SbmlNeverOpensReferencedFiles) {
    // SBML that names files, which exist: no conversion ever shows their content. An entity
    // reference without a declaration, XInclude and a style sheet are refused by libSBML or
    // ignored; comp, which would read other documents, is refused (on the sbml element) or not
    // interpreted (below it); and an SBML document given as Antimony is refused before libAntimony,
    // which would read it as SBML.
    const OutsideDirectory outside;
    const std::string secret = "outside_secret_value";
    const std::string file =
        outside.write("outside.xml", kSbmlStart +
                                         "  <model id=\"inner\">\n    <listOfParameters>\n"
                                         "      <parameter id=\"" +
                                         secret +
                                         "\" value=\"42\" constant=\"true\"/>\n"
                                         "    </listOfParameters>\n  </model>\n</sbml>\n");
    const std::string comp = "http://www.sbml.org/sbml/level3/version1/comp/version1";
    const std::string model = "  <model id=\"m\">\n    <listOfParameters>\n      <parameter "
                              "id=\"k\" value=\"1\" constant=\"true\"/>\n    </listOfParameters>\n"
                              "  </model>\n";
    const std::string comp_document =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<sbml xmlns=\"http://www.sbml.org/sbml/level3/version1/core\" xmlns:comp=\"" +
        comp +
        "\" level=\"3\" version=\"1\" comp:required=\"true\">\n"
        "  <comp:listOfExternalModelDefinitions>\n"
        "    <comp:externalModelDefinition comp:id=\"ext\" comp:source=\"file:" +
        file +
        "\" comp:modelRef=\"inner\"/>\n  </comp:listOfExternalModelDefinitions>\n"
        "  <model id=\"outer\">\n    <comp:listOfSubmodels>\n"
        "      <comp:submodel comp:id=\"sub\" comp:modelRef=\"ext\"/>\n"
        "    </comp:listOfSubmodels>\n  </model>\n</sbml>\n";
    const std::string nested_comp =
        kSbmlStart + "  <comp:listOfExternalModelDefinitions xmlns:comp=\"" + comp + "\">\n" +
        "    <comp:externalModelDefinition comp:id=\"ext\" comp:source=\"file:" + file +
        "\" comp:modelRef=\"inner\"/>\n  </comp:listOfExternalModelDefinitions>\n" + model +
        "</sbml>\n";
    const std::string entity = kSbmlStart + "  <model id=\"m\" name=\"&outside;\"/>\n</sbml>\n";
    const std::string xinclude =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<sbml xmlns=\"http://www.sbml.org/sbml/level3/version2/core\" "
        "xmlns:xi=\"http://www.w3.org/2001/XInclude\" level=\"3\" version=\"2\">\n"
        "  <xi:include href=\"" +
        file + "\"/>\n" + model + "</sbml>\n";
    const std::string stylesheet = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                   "<?xml-stylesheet type=\"text/xsl\" href=\"" +
                                   file + "\"?>\n" + kSbmlStart.substr(kSbmlStart.find('\n') + 1) +
                                   model + "</sbml>\n";
    struct Case {
        std::string name, from, source;
        praenora_status status;
        std::string rule, engine;
    };
    for (const Case& c : std::vector<Case>{
             {"comp on the sbml element", "sbml", comp_document, PRAENORA_STATUS_DATA_INVALID,
              "comp", ""},
             {"comp below the sbml element", "sbml", nested_comp, PRAENORA_STATUS_OK, "", ""},
             {"an entity reference", "sbml", entity, PRAENORA_STATUS_DATA_INVALID, "", "libsbml"},
             {"XInclude", "sbml", xinclude, PRAENORA_STATUS_OK, "", ""},
             {"a style sheet", "sbml", stylesheet, PRAENORA_STATUS_OK, "", ""},
             {"SBML as Antimony", "antimony", comp_document, PRAENORA_STATUS_DATA_INVALID, "xml",
              ""},
         }) {
        SCOPED_TRACE(c.name);
        const std::string to = c.from == "sbml" ? "antimony" : "sbml";
        for (const std::string& source : {c.source, with_byte_order_mark(with_crlf(c.source))}) {
            const ConversionResult result = convert(c.from, to, source);
            EXPECT_EQ(result.status, c.status) << shown(result);
            for (const std::string* text : {&result.text, &result.report, &result.error}) {
                EXPECT_FALSE(contains(*text, secret)) << *text;
                EXPECT_FALSE(contains(*text, "Could not open")) << *text;
            }
            const json details = details_of(result);
            EXPECT_EQ(details.value("rule", ""), c.rule) << result.error;
            EXPECT_EQ(details.value("engine_error", json::object()).value("type", ""), c.engine)
                << result.error;
        }
    }
    // The same names, of files that do not exist: libSBML's error or a clean conversion, never
    // libAntimony's error about a file.
    for (std::string source : {nested_comp, xinclude, stylesheet}) {
        const std::size_t at = source.find(file);
        source.replace(at, file.size(), "/does/not/exist.xml");
        const ConversionResult result = convert("sbml", "antimony", source);
        EXPECT_EQ(result.status, PRAENORA_STATUS_OK) << shown(result);
        EXPECT_FALSE(contains(result.error, "Could not open")) << result.error;
    }
}

TEST(AntimonyProvider, DistributionsDoNotConvertBack) {
    // libAntimony writes a model that uses a distribution with the package distrib, which the SBML
    // document requires; the rule package refuses that SBML, so such a model converts to SBML but
    // not back.
    const ConversionResult sbml =
        convert("antimony", "sbml", "model *m()\n  x = normal(0, 1);\n  y = 2;\nend\n");
    ASSERT_EQ(sbml.status, PRAENORA_STATUS_OK) << shown(sbml);
    EXPECT_TRUE(contains(sbml.text, "distrib:required=\"true\"")) << sbml.text.substr(0, 600);
    const ConversionResult back = convert("sbml", "antimony", sbml.text);
    EXPECT_EQ(back.status, PRAENORA_STATUS_DATA_INVALID) << shown(back);
    const json details = details_of(back);
    EXPECT_EQ(details.value("rule", ""), "package") << back.error;
    EXPECT_TRUE(contains(details.value("package", ""), "/distrib/")) << back.error;
    EXPECT_FALSE(details.contains("engine_error")) << back.error;
}

namespace {

// Sleeps until just past the next second of the wall clock, so that the calls before and after it
// fall in different seconds.
void sleep_past_the_next_second() {
    using std::chrono::system_clock;
    const system_clock::time_point next =
        std::chrono::time_point_cast<std::chrono::seconds>(system_clock::now()) +
        std::chrono::seconds(1) + std::chrono::milliseconds(20);
    std::this_thread::sleep_until(next);
}

} // namespace

TEST(AntimonyProvider, LayoutRequestsAreRefused) {
    // A source that asks for a layout, by model.layout, the autolayout settings, an element's
    // position or color, or a default style of an element type, gets SBMLNetwork's automatic
    // layout, which seeds its random positions from the wall clock: the SBML would change from one
    // second to the next. The provider refuses it (UNSUPPORTED, details.reason "layout"), the same
    // way in different seconds.
    const std::string chain = "J0: S1 -> S2; k * S1;\nJ1: S2 -> S3; k * S2;\n"
                              "S1 = 10; S2 = 0; S3 = 0; k = 0.1;\n";
    const std::vector<std::string> requests = {
        "model.layout = on\n",  "model.autolayout.maxNumConnectedEdges = 4\n",
        "S1.x = 28\n",          "S1.position = {40, 30}\n",
        "S1.color = \"red\"\n", "species.color = \"blue\"\n"};
    std::vector<ConversionResult> first;
    for (const std::string& request : requests) {
        SCOPED_TRACE(request);
        first.push_back(convert("antimony", "sbml", chain + request));
        const ConversionResult& result = first.back();
        EXPECT_EQ(result.status, PRAENORA_STATUS_UNSUPPORTED) << shown(result);
        EXPECT_EQ(result.text, "");
        EXPECT_EQ(result.report, "");
        const json details = details_of(result);
        EXPECT_EQ(details.value("reason", ""), "layout") << result.error;
        EXPECT_FALSE(details.contains("rule")) << result.error;
    }
    sleep_past_the_next_second();
    for (std::size_t i = 0; i < requests.size(); ++i) {
        SCOPED_TRACE(requests[i]);
        const ConversionResult second = convert("antimony", "sbml", chain + requests[i]);
        EXPECT_EQ(second.status, first[i].status);
        EXPECT_EQ(second.error, first[i].error);
    }
    // Without a request, the same model converts.
    const ConversionResult plain = convert("antimony", "sbml", chain);
    ASSERT_EQ(plain.status, PRAENORA_STATUS_OK) << shown(plain);
    EXPECT_FALSE(contains(plain.text, "layout")) << plain.text.substr(0, 600);
}

TEST(AntimonyProvider, LaidOutSbmlConvertsTheSameAcrossSeconds) {
    // SBML that holds a layout (sbml/laid-out.xml, written by libAntimony and SBMLNetwork) converts
    // to Antimony with its positions: libAntimony reads them and writes them out, and the text is
    // the same bytes in different seconds of the wall clock (loading it runs SBMLNetwork's layout,
    // whose random positions do not reach the text).
    const std::string source = read_fixture("sbml/laid-out.xml");
    const ConversionResult first = convert("sbml", "antimony", source);
    ASSERT_EQ(first.status, PRAENORA_STATUS_OK) << shown(first);
    EXPECT_EQ(first.text, read_fixture("antimony/from-sbml/laid-out.ant"));
    EXPECT_TRUE(contains(first.text, "model.layout = on")) << first.text;
    EXPECT_TRUE(contains(first.text, "S1.position = {40.24, 30}")) << first.text;
    sleep_past_the_next_second();
    const ConversionResult second = convert("sbml", "antimony", source);
    EXPECT_EQ(second.text, first.text);
    EXPECT_EQ(second.report, first.report);
    // That text, back to SBML, asks for a layout: refused.
    const ConversionResult back = convert("antimony", "sbml", first.text);
    EXPECT_EQ(back.status, PRAENORA_STATUS_UNSUPPORTED) << shown(back);
    EXPECT_EQ(details_of(back).value("reason", ""), "layout") << back.error;
}
