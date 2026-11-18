// Document loading: YAML and JSON to JSON with a JSON-pointer source map, the YAML subset rules,
// document limits, canonical JSON and JSON parse errors.
#include "document/json_util.hpp"
#include "document/yaml_loader.hpp"

#include "core/paths.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <praenora/runtime/error.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Error;
using praenora::ErrorCode;
namespace detail = praenora::detail;
using praenora::test::ScratchDir;

namespace {

detail::LoadedDocument parse(const std::string& text) {
    return detail::parse_document_text(text, "test.yaml");
}

// Parses `text` expecting PARSE_ERROR and returns the error.
Error parse_error(const std::string& text, const std::string& origin = "test.yaml") {
    try {
        detail::parse_document_text(text, origin);
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::PARSE_ERROR) << e.what();
        return e;
    }
    ADD_FAILURE() << "no error for:\n" << text;
    return Error(ErrorCode::OK, "no error");
}

Error json_text_error(const std::string& text) {
    try {
        detail::parse_json_text(text, "result.json");
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::PARSE_ERROR) << e.what();
        return e;
    }
    ADD_FAILURE() << "no error for:\n" << text;
    return Error(ErrorCode::OK, "no error");
}

std::string nested_yaml_mappings(int depth) {
    std::string text;
    for (int i = 0; i < depth; ++i) {
        text += std::string(static_cast<std::size_t>(2 * i), ' ') + "a:\n";
    }
    text += std::string(static_cast<std::size_t>(2 * depth), ' ') + "1\n";
    return text;
}

std::string nested_json_arrays(int depth) {
    return std::string(static_cast<std::size_t>(depth), '[') + "1" +
           std::string(static_cast<std::size_t>(depth), ']');
}

// The offset of the first bracket that the nesting pre-scan finds beyond one level, or nullopt.
std::optional<std::size_t> beyond_one_level(const std::string& text) {
    return detail::find_excessive_nesting(text, 1);
}

std::string read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST(Yaml, ScalarResolution) {
    struct Case {
        const char* yaml;
        json expected;
    };
    const std::vector<Case> cases = {
        // null
        {"v: null", json(nullptr)},
        {"v: Null", json(nullptr)},
        {"v: NULL", json(nullptr)},
        {"v: ~", json(nullptr)},
        {"v:", json(nullptr)},
        // booleans: only true and false (YAML 1.2 core schema)
        {"v: true", json(true)},
        {"v: True", json(true)},
        {"v: TRUE", json(true)},
        {"v: false", json(false)},
        {"v: False", json(false)},
        {"v: FALSE", json(false)},
        {"v: no", json("no")},
        {"v: yes", json("yes")},
        {"v: on", json("on")},
        {"v: y", json("y")},
        // integers
        {"v: 0", json(0)},
        {"v: 42", json(42)},
        {"v: -17", json(-17)},
        {"v: +17", json(17)},
        {"v: 007", json(7)},
        {"v: 0o17", json(15)},
        {"v: 0x1F", json(31)},
        {"v: 0xff", json(255)},
        {"v: 18446744073709551615", json(18446744073709551615ULL)},
        {"v: -9223372036854775808", json(INT64_MIN)},
        {"v: 9007199254740991", json(9007199254740991ULL)},
        // floats
        {"v: 1e3", json(1000.0)},
        {"v: 1.5", json(1.5)},
        {"v: -0.25", json(-0.25)},
        {"v: .5", json(0.5)},
        {"v: -.5", json(-0.5)},
        {"v: +1.5e-3", json(0.0015)},
        {"v: 1.", json(1.0)},
        {"v: 2.5E2", json(250.0)},
        // strings
        {"v: \"1.0\"", json("1.0")},
        {"v: '42'", json("42")},
        {"v: \"true\"", json("true")},
        {"v: \"null\"", json("null")},
        {"v: \"\"", json("")},
        {"v: 0.1.0", json("0.1.0")},
        {"v: 1_000", json("1_000")},
        {"v: 0b101", json("0b101")},
        {"v: 0x", json("0x")},
        {"v: 1e", json("1e")},
        {"v: hello world", json("hello world")},
        {"v: example.estimate-predict", json("example.estimate-predict")},
        {"v: 30s", json("30s")},
        {"v: inf", json("inf")},
        {"v: nan", json("nan")},
        {"v: |\n  a\n  b\n", json("a\nb\n")},
        {"v: >\n  a\n  b\n", json("a b\n")},
        {"v: \"caf\\u00e9\"", json("caf\xC3\xA9")},
        {"v: caf\xC3\xA9", json("caf\xC3\xA9")},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.yaml);
        const auto doc = parse(c.yaml);
        ASSERT_TRUE(doc.json.is_object());
        const auto& value = doc.json.at("v");
        EXPECT_EQ(value, c.expected);
        EXPECT_EQ(value.type_name(), c.expected.type_name()) << value.dump();
        EXPECT_EQ(value.is_number_float(), c.expected.is_number_float()) << value.dump();
    }
    // Non-negative integers are unsigned and negative ones signed, as the JSON parser makes them.
    EXPECT_TRUE(parse("v: 42").json.at("v").is_number_unsigned());
    EXPECT_TRUE(parse("v: 1.0").json.at("v").is_number_float());
    EXPECT_TRUE(parse("v: 1e3").json.at("v").is_number_float());
}

TEST(Yaml, ExplicitTagsAreHonored) {
    EXPECT_EQ(parse("v: !!str 12").json.at("v"), json("12"));
    EXPECT_EQ(parse("v: !!str").json.at("v"), json(""));
    EXPECT_EQ(parse("v: !!str null").json.at("v"), json("null"));
    EXPECT_EQ(parse("v: !!int \"12\"").json.at("v"), json(12));
    EXPECT_EQ(parse("v: !!int 0x10").json.at("v"), json(16));
    EXPECT_EQ(parse("v: !!float 1").json.at("v"), json(1.0));
    EXPECT_TRUE(parse("v: !!float 1").json.at("v").is_number_float());
    EXPECT_EQ(parse("v: !!bool true").json.at("v"), json(true));
    EXPECT_EQ(parse("v: !!null").json.at("v"), json(nullptr));
    EXPECT_EQ(parse("v: !!null ~").json.at("v"), json(nullptr));
    EXPECT_EQ(parse("v: ! 12").json.at("v"), json("12")) << "the non-specific tag means string";

    for (const char* bad : {"v: !!int 1.5", "v: !!int abc", "v: !!float abc", "v: !!bool yes",
                            "v: !!null 0", "v: !!binary aGk=", "v: !local x", "v: !!timestamp 1",
                            "v: !!map {a: 1}", "v: !!seq [1]", "v: !!str [1]"}) {
        SCOPED_TRACE(bad);
        const auto error = parse_error(bad);
        EXPECT_EQ(error.location().line, 1);
        EXPECT_EQ(error.location().pointer, "/v");
    }
}

TEST(Yaml, DuplicateKeyIsParseErrorWithPosition) {
    const auto error = parse_error("a: 1\nb: 2\na: 3\n");
    EXPECT_EQ(error.location().file, "test.yaml");
    EXPECT_EQ(error.location().line, 3);
    EXPECT_EQ(error.location().column, 1);
    EXPECT_EQ(error.location().pointer, "/a");
    EXPECT_NE(std::string(error.what()).find("duplicate key 'a'"), std::string::npos)
        << error.what();

    // Duplicates are found after quoting is resolved, and in nested and flow mappings too.
    EXPECT_EQ(parse_error("a: 1\n\"a\": 2\n").location().line, 2);
    const auto nested = parse_error("top:\n  x: 1\n  y: {k: 1, k: 2}\n");
    EXPECT_EQ(nested.location().line, 3);
    EXPECT_EQ(nested.location().column, 13);
    EXPECT_EQ(nested.location().pointer, "/top/y/k");
}

TEST(Yaml, InfAndNanRejected) {
    for (const char* bad :
         {"v: .inf", "v: .Inf", "v: .INF", "v: +.inf", "v: -.inf", "v: .nan", "v: .NaN", "v: .NAN",
          "v: !!float .inf", "v: [1, .nan]", "v: 1e999", "v: -1e999"}) {
        SCOPED_TRACE(bad);
        const auto error = parse_error(bad);
        EXPECT_EQ(error.location().line, 1);
    }
}

TEST(Yaml, IntegerOverflowRejected) {
    for (const char* bad :
         {"v: 18446744073709551616", "v: -9223372036854775809", "v: 0x10000000000000000",
          "v: 0o2000000000000000000000", "v: !!int 99999999999999999999"}) {
        SCOPED_TRACE(bad);
        EXPECT_EQ(parse_error(bad).location().pointer, "/v");
    }
}

TEST(Yaml, MergeKeyRejected) {
    const auto error = parse_error("base: &b {x: 1}\nd:\n  <<: *b\n  y: 2\n");
    EXPECT_EQ(error.location().line, 3);
    EXPECT_EQ(error.location().column, 3);
    EXPECT_NE(std::string(error.what()).find("<<"), std::string::npos) << error.what();
    // A quoted "<<" is an ordinary key.
    EXPECT_EQ(parse("\"<<\": 1").json.at("<<"), json(1));
}

TEST(Yaml, MultiDocumentRejected) {
    const auto error = parse_error("a: 1\n---\nb: 2\n");
    EXPECT_EQ(error.location().line, 3);
    // Explicit markers around a single document are fine.
    EXPECT_EQ(parse("---\na: 1\n...\n").json, json::parse(R"({"a":1})"));
}

TEST(Yaml, EmptyDocumentRejected) {
    for (const char* empty : {"", "\n\n", "# only a comment\n", "---\n"}) {
        SCOPED_TRACE(empty);
        parse_error(empty);
    }
}

TEST(Yaml, NonScalarKeyRejected) {
    const auto error = parse_error("ok: 1\n? [a, b]\n: c\n");
    EXPECT_EQ(error.location().line, 2);
    EXPECT_EQ(parse_error("{a: 1}: x\n").location().line, 1);
    EXPECT_EQ(parse_error("~: x\n").location().line, 1) << "null keys are not strings";
    // Scalar keys keep their text: they are not resolved to numbers or booleans.
    EXPECT_EQ(parse("1: a\ntrue: b\n").json, json::parse(R"({"1":"a","true":"b"})"));
}

TEST(Yaml, AnchorsAndAliasesResolve) {
    const auto doc = parse("base: &b\n  x: 1\n  y: [1, 2]\ncopy: *b\nlist: [*b, *b]\n");
    EXPECT_EQ(doc.json.at("copy"), doc.json.at("base"));
    EXPECT_EQ(doc.json.at("list")[1], doc.json.at("base"));
    EXPECT_EQ(doc.json.at("copy").at("y"), json::parse("[1,2]"));
    ASSERT_EQ(doc.positions.count("/copy/y/1"), 1u);
    ASSERT_EQ(doc.positions.count("/list/1/x"), 1u);
    // An alias of a scalar.
    EXPECT_EQ(parse("a: &v 3\nb: *v\n").json.at("b"), json(3));
}

TEST(Yaml, RecursiveAliasIsTooDeep) {
    const auto error = parse_error("a: &x [1, *x]\n");
    EXPECT_NE(std::string(error.what()).find("nesting too deep"), std::string::npos)
        << error.what();
}

TEST(Yaml, PositionsForNestedPointers) {
    const std::string config = "kind: workflow-config\n"                      // 1
                               "format_version: \"0.1.0\"\n"                  // 2
                               "workflow: {name: example.estimate-predict}\n" // 3
                               "steps:\n"                                     // 4
                               "  estimate:\n"                                // 5
                               "    parameters:\n"                            // 6
                               "      gain: 0.6\n"                            // 7
                               "    module: {name: example-estimator}\n";     // 8
    const auto doc = detail::parse_document_text(config, "workflow.yaml");
    EXPECT_EQ(doc.origin, "workflow.yaml");
    const auto at = [&](const std::string& pointer) {
        const auto it = doc.positions.find(pointer);
        EXPECT_NE(it, doc.positions.end()) << pointer;
        return it == doc.positions.end() ? detail::SourcePosition{0, 0} : it->second;
    };
    // Scalars are located where their value starts.
    EXPECT_EQ(at("/steps/estimate/parameters/gain").line, 7);
    EXPECT_EQ(at("/steps/estimate/parameters/gain").column, 13);
    EXPECT_EQ(at("/workflow/name").line, 3);
    EXPECT_EQ(at("/workflow/name").column, 18);
    EXPECT_EQ(at("/format_version").column, 17);
    EXPECT_EQ(at("/steps/estimate/module/name").line, 8);
    // Mapping entries whose value is a mapping or sequence are located at their key.
    EXPECT_EQ(at("/steps/estimate/parameters").line, 6);
    EXPECT_EQ(at("/steps/estimate/parameters").column, 5);
    EXPECT_EQ(at("/steps").line, 4);
    EXPECT_EQ(at("/steps").column, 1);
    EXPECT_EQ(at("/workflow").column, 1);
    EXPECT_EQ(at("").line, 1);
    EXPECT_EQ(at("").column, 1);

    // Every pointer of the document has a position.
    std::vector<std::string> missing;
    const auto flat = doc.json.flatten();
    for (const auto& item : flat.items()) {
        if (doc.positions.count(item.key()) == 0) {
            missing.push_back(item.key());
        }
    }
    EXPECT_TRUE(missing.empty()) << missing.front();

    const auto location = detail::locate(doc, "/steps/estimate/parameters/gain");
    EXPECT_EQ(location.file, "workflow.yaml");
    EXPECT_EQ(location.line, 7);
    EXPECT_EQ(location.column, 13);
    EXPECT_EQ(location.pointer, "/steps/estimate/parameters/gain");
}

TEST(Yaml, LocateWalksUpToTheNearestAncestor) {
    const auto doc = parse("steps:\n  estimate:\n    parameters: {gain: 0.6}\n");
    // A pointer that is not in the document (for example a missing required property) is located
    // at its nearest ancestor that is.
    const auto location = detail::locate(doc, "/steps/estimate/module/name");
    EXPECT_EQ(location.line, 2);
    EXPECT_EQ(location.column, 3);
    EXPECT_EQ(location.pointer, "/steps/estimate/module/name");
    EXPECT_EQ(detail::locate(doc, "/nothing").line, 1);
    EXPECT_EQ(detail::locate(doc, "").column, 1);
}

TEST(Yaml, EmptyValuesAreLocatedAtTheirKey) {
    const auto doc = parse("a: 1\nb:\nc: ~\n");
    EXPECT_EQ(doc.positions.at("/b").line, 2);
    EXPECT_EQ(doc.positions.at("/b").column, 1);
    EXPECT_EQ(doc.positions.at("/c").line, 3);
    EXPECT_EQ(doc.positions.at("/c").column, 4);
}

TEST(Yaml, KeysWithSlashAndTildeAreEscapedInPointers) {
    const auto doc = parse("\"a/b\":\n  \"c~d\": 1\n");
    EXPECT_EQ(doc.json.at("a/b").at("c~d"), json(1));
    EXPECT_EQ(doc.positions.at("/a~1b/c~0d").line, 2);
}

TEST(Yaml, InvalidUtf8Rejected) {
    const auto error = parse_error("a: ok\nb: \"x\xFFy\"\n");
    EXPECT_EQ(error.location().line, 2);
    EXPECT_EQ(error.location().pointer, "/b");
    EXPECT_EQ(parse_error("\xC3: 1\n").location().line, 1) << "keys are checked too";
}

TEST(Yaml, SyntaxErrorHasPosition) {
    const auto error = parse_error("a: [1, 2\nb: 3\n");
    EXPECT_EQ(error.location().file, "test.yaml");
    EXPECT_GT(error.location().line, 0);
    EXPECT_GT(error.location().column, 0);
}

TEST(Yaml, JsonInputParsesWithPositions) {
    const std::string text = "{\n"
                             "  \"kind\": \"module-manifest\",\n"
                             "  \"module\": {\"name\": \"m\", \"version\": \"0.1.0\"},\n"
                             "  \"capabilities\": [\n"
                             "    {\"id\": \"test.echo\", \"version\": \"0.1.0\"}\n"
                             "  ],\n"
                             "  \"big\": 18446744073709551615,\n"
                             "  \"text\": \"caf\\u00e9 \\ud83d\\ude00 \\/\"\n"
                             "}\n";
    const auto doc = detail::parse_document_text(text, "m.pmm.json");
    EXPECT_EQ(doc.json, json::parse(text));
    EXPECT_EQ(doc.json.at("text"), json("caf\xC3\xA9 \xF0\x9F\x98\x80 /"));
    EXPECT_EQ(doc.positions.at("").line, 1);
    EXPECT_EQ(doc.positions.at("/kind").line, 2);
    EXPECT_EQ(doc.positions.at("/kind").column, 11);
    EXPECT_EQ(doc.positions.at("/module/version").line, 3);
    EXPECT_EQ(doc.positions.at("/module/version").column, 38);
    EXPECT_EQ(doc.positions.at("/module").column, 3) << "an object entry is located at its key";
    EXPECT_EQ(doc.positions.at("/capabilities/0").line, 5);
    EXPECT_EQ(doc.positions.at("/capabilities/0").column, 5);
    EXPECT_EQ(doc.positions.at("/capabilities/0/id").column, 12);
    EXPECT_EQ(doc.positions.at("/big").line, 7);
    const auto flat = doc.json.flatten();
    for (const auto& item : flat.items()) {
        EXPECT_EQ(doc.positions.count(item.key()), 1u) << item.key();
    }
}

TEST(Yaml, JsonInputDuplicateKeyRejected) {
    const auto error = parse_error("{\n  \"a\": 1,\n  \"b\": {\"x\": 1, \"x\": 2}\n}\n", "d.json");
    EXPECT_EQ(error.location().file, "d.json");
    EXPECT_EQ(error.location().line, 3);
    EXPECT_EQ(error.location().column, 17);
    EXPECT_EQ(error.location().pointer, "/b/x");
}

TEST(Yaml, FlowYamlThatIsNotJsonStillParses) {
    // Starts like JSON but is YAML flow style.
    EXPECT_EQ(parse("{a: 1, b: [x, y]}").json, json::parse(R"({"a":1,"b":["x","y"]})"));
}

TEST(Yaml, MalformedJsonFileReportsTheJsonError) {
    const auto error = parse_error("{\n  \"a\": 1\n  \"b\": 2\n}\n", "broken.json");
    EXPECT_EQ(error.location().file, "broken.json");
    EXPECT_EQ(error.location().line, 3);
    EXPECT_GT(error.location().column, 2);
    EXPECT_NE(std::string(error.what()).find("expected '}'"), std::string::npos) << error.what();
}

TEST(Yaml, BomAndCrlfParseIdentically) {
    for (const char* name : {"workflow.yaml", "manifest.json"}) {
        SCOPED_TRACE(name);
        const auto windows =
            praenora::test::fixture_path(std::string("documents/bom-crlf/") + name);
        const auto unix_lf = praenora::test::fixture_path(std::string("documents/lf/") + name);
        // Guard against the fixtures being normalized by a checkout.
        const auto raw = read_bytes(windows);
        ASSERT_EQ(raw.rfind("\xEF\xBB\xBF", 0), 0u) << "the bom-crlf fixture lost its BOM";
        ASSERT_NE(raw.find("\r\n"), std::string::npos) << "the bom-crlf fixture lost its CRLF";
        const auto lf_raw = read_bytes(unix_lf);
        ASSERT_EQ(lf_raw.find('\r'), std::string::npos);
        ASSERT_NE(lf_raw.rfind("\xEF\xBB\xBF", 0), 0u);

        const auto a = detail::load_document_file(windows);
        const auto b = detail::load_document_file(unix_lf);
        EXPECT_EQ(a.json, b.json);
        EXPECT_EQ(a.positions, b.positions);
        EXPECT_EQ(a.origin, praenora::detail::path_to_utf8(windows));
        EXPECT_GT(a.positions.size(), 10u);
    }
    const auto doc = detail::load_document_file(
        praenora::test::fixture_path("documents/bom-crlf/workflow.yaml"));
    EXPECT_EQ(doc.json.at("steps").at("estimate").at("parameters").at("note"),
              json("first line\nsecond line\n"));
    EXPECT_EQ(doc.json.at("steps").at("estimate").at("parameters").at("label"),
              json("r\xC3\xA9sum\xC3\xA9"));
    EXPECT_EQ(doc.positions.at("/kind").line, 3);
    EXPECT_EQ(doc.positions.at("/kind").column, 7);
}

TEST(Yaml, RejectsExcessiveNesting) {
    EXPECT_EQ(parse(nested_yaml_mappings(128)).json.flatten().size(), 1u);
    const auto error = parse_error(nested_yaml_mappings(129));
    EXPECT_NE(std::string(error.what()).find("nesting too deep"), std::string::npos)
        << error.what();
    EXPECT_EQ(error.location().line, 129);
    // JSON-shaped documents follow the same limit.
    EXPECT_NO_THROW(parse(nested_json_arrays(128)));
    const auto json_error = parse_error(nested_json_arrays(129), "deep.json");
    EXPECT_NE(std::string(json_error.what()).find("nesting too deep"), std::string::npos);
    EXPECT_EQ(json_error.location().column, 129);
    // Far deeper, also beyond the YAML parser's own recursion limit: block and flow nesting, in
    // YAML and in JSON shape, are all reported as nesting too deep.
    std::string block_sequences;
    for (int i = 0; i < 5000; ++i) {
        block_sequences += "- ";
    }
    std::string flow_yaml; // flow mappings with plain scalars: YAML, not JSON
    for (int i = 0; i < 600; ++i) {
        flow_yaml += "{a: ";
    }
    flow_yaml += "x" + std::string(600, '}');
    const std::vector<std::string> deep = {
        std::string(5000, '[') + std::string(5000, ']'),
        block_sequences + "x\n",
        nested_yaml_mappings(600),
        flow_yaml,
        "v: " + std::string(300, '[') + std::string(300, ']') + "\n",
    };
    for (const auto& text : deep) {
        SCOPED_TRACE(text.substr(0, 40));
        const auto deep_error = parse_error(text);
        EXPECT_NE(std::string(deep_error.what()).find("nesting too deep"), std::string::npos)
            << deep_error.what();
        EXPECT_GT(deep_error.location().line, 0);
    }
    // Brackets in comments and inside quoted strings do not count.
    EXPECT_EQ(
        parse("# " + std::string(200, '[') + "\nv: '" + std::string(200, '{') + "'\n").json.at("v"),
        json(std::string(200, '{')));
    EXPECT_EQ(parse("v: don't [x]\n").json.at("v"), json("don't [x]"));
}

TEST(Yaml, JsonFilesAreStrictJson) {
    // Text that YAML would accept is an error in a *.json document.
    for (const char* text : {"{a: 1}", "{\"a\": 1,}", "{\"v\": 0.1.0}", "a: 1\n", "{'a': 1}",
                             "[1, 2,]", "{\"a\": 1} # note\n", "", "---\n{\"a\": 1}\n"}) {
        SCOPED_TRACE(text);
        const auto error = parse_error(text, "m.pmm.json");
        EXPECT_EQ(error.location().file, "m.pmm.json");
        EXPECT_GT(error.location().line, 0);
    }
    EXPECT_EQ(parse_error("{\"a\": 1,}", "CONFIG.JSON").location().column, 9)
        << "the extension matches in any case";
    // The same texts are YAML in other files.
    EXPECT_EQ(detail::parse_document_text("{a: 1}", "x.yaml").json, json::parse(R"({"a":1})"));
    EXPECT_EQ(detail::parse_document_text("a: 1\n", "x.yml").json, json::parse(R"({"a":1})"));
    // Valid JSON loads, with positions.
    const auto doc = detail::parse_document_text("{\"v\": \"0.1.0\"}", "m.pmm.json");
    EXPECT_EQ(doc.json.at("v"), json("0.1.0"));
    EXPECT_EQ(doc.positions.at("/v").column, 7);
}

TEST(Yaml, AliasExpansionIsLimited) {
    // Nine levels of ten aliases each: 10^9 values from a few hundred bytes.
    const auto laughs = [](int levels) {
        std::string text = "l0: &l0 [x, x, x, x, x, x, x, x, x, x]\n";
        for (int level = 1; level < levels; ++level) {
            const std::string previous = "*l" + std::to_string(level - 1);
            text += "l" + std::to_string(level) + ": &l" + std::to_string(level) + " [";
            for (int i = 0; i < 10; ++i) {
                text += (i == 0 ? "" : ", ") + previous;
            }
            text += "]\n";
        }
        return text;
    };
    const std::string bomb = laughs(9);
    ASSERT_LT(bomb.size(), 1024u);
    const auto start = std::chrono::steady_clock::now();
    const auto error = parse_error(bomb);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_NE(std::string(error.what()).find("document too large"), std::string::npos)
        << error.what();
    EXPECT_LT(elapsed, std::chrono::seconds(10)) << "rejected before the expansion is built";
    // Within the limit, aliases expand as usual: 10^4 values.
    const auto small = parse(laughs(4));
    EXPECT_EQ(small.json.at("l3").size(), 10u);
    EXPECT_EQ(small.json.at("l3")[9][9][9][9], json("x"));
    EXPECT_EQ(small.positions.count("/l3/9/9/9/9"), 1u);
}

TEST(Yaml, DocumentTooLarge) {
    ScratchDir dir;
    const auto file = dir.path() / "big.yaml";
    {
        std::ofstream out(file, std::ios::binary);
        const std::string line(1023, '#');
        for (std::size_t i = 0; i < 16 * 1024; ++i) {
            out << line << '\n';
        }
        out << "a: 1\n";
    }
    ASSERT_GT(fs::file_size(file), detail::kMaxDocumentBytes);
    try {
        detail::load_document_file(file);
        FAIL() << "no error";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::PARSE_ERROR);
        EXPECT_NE(std::string(e.what()).find("document too large"), std::string::npos) << e.what();
        EXPECT_EQ(e.location().file, praenora::detail::path_to_utf8(file));
    }
    // The same limit applies to text.
    EXPECT_NE(std::string(parse_error(std::string(detail::kMaxDocumentBytes + 1, ' ')).what())
                  .find("document too large"),
              std::string::npos);
}

TEST(Yaml, CheckDocumentSizeRefusesLargeFiles) {
    ScratchDir dir;
    const auto big = dir.path() / "big.json";
    {
        std::ofstream out(big, std::ios::binary);
    }
    fs::resize_file(big, detail::kMaxDocumentBytes + 1);
    try {
        detail::check_document_size(big);
        FAIL() << "no error";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::PARSE_ERROR);
        EXPECT_NE(std::string(e.what()).find("document too large"), std::string::npos) << e.what();
        EXPECT_EQ(e.location().file, praenora::detail::path_to_utf8(big));
    }
    const auto small = dir.path() / "small.json";
    {
        std::ofstream out(small, std::ios::binary);
        out << "{}";
    }
    EXPECT_NO_THROW(detail::check_document_size(small));
    // A file that cannot be looked at is left to the reader, which reports it.
    EXPECT_NO_THROW(detail::check_document_size(dir.path() / "absent.json"));
}

TEST(Yaml, MissingFileIsIoError) {
    ScratchDir dir;
    try {
        detail::load_document_file(dir.path() / "missing.yaml");
        FAIL() << "no error";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::IO_ERROR);
    }
}

TEST(Json, RejectsExcessiveNesting) {
    EXPECT_NO_THROW(detail::parse_json_text(nested_json_arrays(128), "ok.json"));
    const auto error = json_text_error("\n" + nested_json_arrays(129));
    EXPECT_NE(std::string(error.what()).find("nesting too deep"), std::string::npos)
        << error.what();
    EXPECT_EQ(error.location().line, 2);
    EXPECT_EQ(error.location().column, 129);
    // Brackets inside strings do not count.
    EXPECT_NO_THROW(detail::parse_json_text("[\"" + std::string(500, '[') + "\"]", "s.json"));
    json_text_error(std::string(100000, '[') + std::string(100000, ']'));
}

TEST(Json, CanonicalIsSortedAndCompact) {
    EXPECT_EQ(detail::canonical_json(json::parse(R"({"b":1,"a":[2,{"d":3,"c":4}]})")),
              R"({"a":[2,{"c":4,"d":3}],"b":1})");
    EXPECT_EQ(detail::canonical_json(json::parse(R"({"x": 0.5, "y": 1e3, "z": "\u00e9"})")),
              "{\"x\":0.5,\"y\":1000.0,\"z\":\"\xC3\xA9\"}");
    try {
        detail::canonical_json(json(std::string("bad \xFF")));
        FAIL() << "invalid UTF-8 must not be serialized";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::DATA_INVALID);
    }
}

TEST(Json, ParseErrorHasLineAndColumn) {
    const auto error = json_text_error("{\n  \"a\": 1,\n  \"b\": x\n}");
    EXPECT_EQ(error.location().file, "result.json");
    EXPECT_EQ(error.location().line, 3);
    EXPECT_EQ(error.location().column, 8);
    EXPECT_EQ(std::string(error.what()).find("[json.exception"), std::string::npos) << error.what();
    EXPECT_EQ(json_text_error("{\"a\":1,}").location().column, 8);
    EXPECT_EQ(json_text_error("").location().line, 1);
    EXPECT_EQ(detail::parse_json_text(R"({"a":[1,2]})", "x").at("a")[1], json(2));
}

TEST(Json, PointerEscaping) {
    EXPECT_EQ(detail::json_pointer_escape("a/b~c"), "a~1b~0c");
    EXPECT_EQ(detail::json_pointer_escape("plain"), "plain");
}

// An integer is read by its value, however nlohmann/json stores it: -0 is a signed integer, and
// so is a value assigned from a signed type.
TEST(Json, Uint32OfReadsIntegersByValue) {
    EXPECT_EQ(detail::uint32_of(json::parse("0")), std::optional<std::uint32_t>(0));
    EXPECT_EQ(detail::uint32_of(json::parse("-0")), std::optional<std::uint32_t>(0));
    EXPECT_EQ(detail::uint32_of(json::parse("7")), std::optional<std::uint32_t>(7));
    EXPECT_EQ(detail::uint32_of(json::parse("4294967295")),
              std::optional<std::uint32_t>(4294967295U));
    EXPECT_EQ(detail::uint32_of(json(std::int64_t{7})), std::optional<std::uint32_t>(7));
    for (const char* text : {"4294967296", "-1", "0.0", "-0.0", "1e0", "\"0\"", "null", "true"}) {
        EXPECT_EQ(detail::uint32_of(json::parse(text)), std::nullopt) << text;
    }
    EXPECT_EQ(detail::uint32_of(json(std::int64_t{4294967296})), std::nullopt);
}

TEST(NestingScan, QuoteInsidePlainScalarIsText) {
    // A quote opens a string only where a scalar starts. Inside a plain scalar, continuation lines
    // included, it is text and hides nothing after it.
    const std::vector<std::string> plain_scalars = {
        "k: a:'x\n", // after a colon that no space follows
        "k: a:\"x\n",
        "k: don't\n  'stop\n",   // on a continuation line
        "k:\n  value\n 'more\n", // indented past the mapping, though less than the scalar
        "k:\n- - a\n   'b\n",    // indented past the inner sequence
        "&x k: a\n 'b\n",        // the mapping starts at the anchor
        "k: [a:'x, b]\n",        // in a flow collection
        "k: [a\n  'b, c]\n",     // on a continuation line in a flow collection
        "k: {a:'x}\n",
    };
    for (const auto& prefix : plain_scalars) {
        const std::string text = prefix + "v: [[1]]\n";
        EXPECT_EQ(beyond_one_level(text), text.rfind("[[") + 1) << text;
        EXPECT_NO_THROW(parse(text)) << text;
    }
}

TEST(NestingScan, PlainScalarEndsWhereYamlEndsIt) {
    // A line indented no further than the innermost collection starts a new token, so its quote
    // opens a string.
    const std::vector<std::string> new_tokens = {
        "k: a\n'x [[': 1\n",     // back at the mapping
        "k: a\n'x: [[': 1\n",    // the same, with ": " inside the string
        "- a: b\n  'x [[': 1\n", // back at the inner mapping
        "k: a # 'x\nv: '[['\n",  // after a comment
        "a\n#x\n'[['\n",         // after a comment line, even below a top-level scalar
        "a\n--- '[['\n",         // after a document marker
    };
    for (const auto& text : new_tokens) {
        EXPECT_EQ(beyond_one_level(text), std::nullopt) << text;
    }
    // A '#' inside a plain scalar is text; in a flow collection a bracket ends the scalar.
    EXPECT_EQ(beyond_one_level("a#b: [[1]]\n"), 6u);
    EXPECT_EQ(beyond_one_level("v: [x[1]]\n"), 5u);
    EXPECT_EQ(beyond_one_level("v: {x: a[1]}\n"), 8u);
}

TEST(NestingScan, FollowsYamlCppPossibleKeys) {
    // yaml-cpp opens a level for a node that may be a key before it knows, so a scalar after the
    // node continues only on lines indented past it. Each prefix is followed by the line "'q # c":
    // where that line starts a token, its quote opens a string that hides the brackets after it.
    struct Case {
        const char* prefix;
        bool hidden;
    };
    const Case cases[] = {
        {"&a x\n", true},                 // the anchor starts a possible key
        {"\t&a x\n", false},              // but not after a tab
        {"k: &a x\n ", false},            // nor right after a key
        {"&a |\n  x\n y\n", true},        // the key outlasts a block scalar
        {"{a: 1}x\n", true},              // and a colon inside a flow collection
        {"&a x\n- z\nw\n", true},         // and a sequence entry
        {"{a: 1}: v\n ", false},          // only the colon after the collection confirms it
        {"&a x\n- z # c\nw\n", false},    // the line break after a comment ends it
        {"&a\nx\n", false},               // a line break between tokens ends it
        {"k:\n  a\nb: x\n ", false},      // and so does a line indented less than the key
        {"  k:\n  v\n &b x\n ", false},   // a key without a level of its own stays, and blocks &b
        {"&a |\n  y\n : \n  x\n ", true}, // a colon after a key on an earlier line opens a level
        {"&a |\n  y\n : \n x\n&b w\n", true}, // ending a possible key's level ends &a, so &b is one
        {"k:\n%YAML 1.2\na\n", false},        // a directive ends every level
    };
    for (const auto& c : cases) {
        const std::string text = std::string(c.prefix) + "'q # c\nj: [[1]]\n'";
        const std::optional<std::size_t> expected =
            c.hidden ? std::nullopt : std::optional<std::size_t>(text.find("[[") + 1);
        EXPECT_EQ(beyond_one_level(text), expected) << text;
    }
}

TEST(NestingScan, FollowsYamlCppLineEnds) {
    // "\r\n" and a lone "\r" end a line too.
    for (const std::string text : {"k: a\r'x [[': 1\r", "k: a\r\n'x [[': 1\r\n"}) {
        EXPECT_EQ(beyond_one_level(text), std::nullopt) << text;
    }
    // Only the kind of the first line end starts a new column count, though: after a "\n", the
    // columns go on past a lone "\r", so the line after it continues the scalar.
    const std::string text = "\nk: a\r'q # c\nj: [[1]]\n'";
    EXPECT_EQ(beyond_one_level(text), text.find("[[") + 1);
}

TEST(NestingScan, QuoteInsideBlockScalarIsText) {
    // The content ends at the first line indented no further than the innermost collection.
    for (const std::string block :
         {"v: |\n  'x\n", "v: |\n  # x\n  'y\n", "v: |2\n    a\n  'x\n", "v: >\n\n"}) {
        const std::string text = block + "w: [[1]]\n";
        EXPECT_EQ(beyond_one_level(text), text.rfind("[[") + 1) << text;
        EXPECT_NO_THROW(parse(text)) << text;
    }
}

TEST(NestingScan, QuoteInsideAnchorOrTagIsText) {
    for (const std::string prefix : {"k: &a'x\n", "k: !t'x\n"}) {
        const std::string text = prefix + "v: [[1]]\n";
        EXPECT_EQ(beyond_one_level(text), text.rfind("[[") + 1) << text;
    }
}

TEST(NestingScan, QuotedScalarsAndCommentsHideBrackets) {
    const std::vector<std::string> texts = {
        "v: '[['\n",          // single-quoted
        "v: 'it''s [['\n",    // with an escaped quote
        "v: \"[[\\\"[[\"\n",  // double-quoted, with an escaped quote
        "v: '[[\n  [['\n",    // over two lines
        "{\"a\":'[[x'}\n",    // right after a JSON-like key and its colon
        "[\"[[\", \"]][[\"]", // JSON
        "# [[\nv: 1 # [[\n",  // comments
        "v: [1]#[[\n",        // a comment right after a token
    };
    for (const auto& text : texts) {
        EXPECT_EQ(beyond_one_level(text), std::nullopt) << text;
    }
}

TEST(NestingScan, CountsBracketsInsidePlainAndBlockScalars) {
    // yaml-cpp reads these as text, but one bracket too many is safe where one too few is not.
    EXPECT_EQ(beyond_one_level("v: x[[ y]]\n"), 5u);
    EXPECT_EQ(beyond_one_level("v: |\n  [[\n"), 8u);
    EXPECT_EQ(beyond_one_level("v: !<t:[[x]]> y\n"), 8u);
    // Closing brackets there balance them.
    EXPECT_EQ(beyond_one_level("v: x[a] y[b]\nw: [1]\n"), std::nullopt);
}

TEST(NestingScan, CountsEveryOpeningBracketInTextItCannotSplit) {
    // Past 4096 levels of block collections the scan stops following them, so that its memory
    // stays small: the document nests too deep anyway.
    std::string deep;
    for (int i = 0; i < 5000; ++i) {
        deep += "- ";
    }
    EXPECT_EQ(beyond_one_level(deep + "'[]['\n"), deep.size() + 3);
    EXPECT_EQ(beyond_one_level(deep.substr(0, 200) + "'[]['\n"), std::nullopt);
    // Text with a NUL byte is not UTF-8, but yaml-cpp reads UTF-16 and UTF-32. In UTF-16LE for
    // "[\u2C41\u4E27, [[]]]" the bytes ",'" would open a string.
    const char utf16[] = "[\0A,'N,\0 \0[\0[\0]\0]\0]\0";
    EXPECT_EQ(beyond_one_level(std::string(utf16, sizeof utf16 - 1)), 10u);
    EXPECT_EQ(beyond_one_level(std::string("[]\0[]", 5)), 3u);
}
