// The AsyncAPI documents committed under spec/asyncapi/ against the official AsyncAPI 3.0.0 JSON
// Schema (schemas/3.0.0.json of github.com/asyncapi/spec-json-schemas). Every build compiles this
// file into an executable of its own, praenora_asyncapi_official_schema_tests; CTest runs its
// tests only when the cache variable PRAENORA_ASYNCAPI_SCHEMA names a local copy of that schema,
// and gives them the copy's path in the environment variable PRAENORA_TEST_ASYNCAPI_SCHEMA.
// Neither the build nor the tests download it (docs/testing.md says which release was checked
// and how to get it). The schema is compiled by the validator that checks Praenora's own
// documents (detail::CompiledSchema).
#include "document/schema.hpp"
#include "platform/platform.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::detail::CompiledSchema;
using praenora::detail::SchemaIssue;

namespace {

json read_json(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read " + file.u8string());
    }
    return json::parse(in);
}

// The local copy of the official schema, named by PRAENORA_TEST_ASYNCAPI_SCHEMA.
json official_schema_document() {
    const std::optional<std::string> path =
        praenora::detail::environment_variable("PRAENORA_TEST_ASYNCAPI_SCHEMA");
    if (!path || path->empty()) {
        throw std::runtime_error(
            "PRAENORA_TEST_ASYNCAPI_SCHEMA is empty or not set: it names a copy of the official "
            "AsyncAPI 3.0.0 JSON Schema, and CTest sets it when the build's "
            "PRAENORA_ASYNCAPI_SCHEMA names one (docs/testing.md)");
    }
    return read_json(praenora::test::utf8_path(*path));
}

// The official schema, compiled once.
const CompiledSchema& official_schema() {
    static const CompiledSchema schema(official_schema_document(), "AsyncAPI 3.0.0");
    return schema;
}

// The committed documents, in name order.
std::vector<fs::path> committed_documents() {
    const fs::path directory =
        praenora::test::utf8_path(PRAENORA_TEST_SOURCE_DIR) / "spec" / "asyncapi";
    std::vector<fs::path> documents;
    for (const auto& entry : fs::directory_iterator(directory)) {
        const std::string name = entry.path().filename().u8string();
        const std::string suffix = ".asyncapi.json";
        if (name.size() > suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            documents.push_back(entry.path());
        }
    }
    std::sort(documents.begin(), documents.end());
    return documents;
}

std::string described(const std::vector<SchemaIssue>& issues) {
    std::string text;
    for (const SchemaIssue& issue : issues) {
        text += "  " + (issue.pointer.empty() ? std::string("(root)") : issue.pointer) + ": " +
                issue.message + "\n";
    }
    return text;
}

// Whether `pointer` is `location` or one of its ancestors.
bool at_or_above(const std::string& pointer, const std::string& location) {
    return location.compare(0, pointer.size(), pointer) == 0 &&
           (location.size() == pointer.size() || location[pointer.size()] == '/');
}

} // namespace

TEST(GraphExport, OfficialSchemaIsTheAsyncApi300Schema) {
    const json schema = official_schema_document();
    EXPECT_EQ(schema.value("$id", ""), "http://asyncapi.com/definitions/3.0.0/asyncapi.json");
    EXPECT_EQ(schema.value(json::json_pointer("/properties/asyncapi/const"), ""), "3.0.0");
}

TEST(GraphExport, CommittedAsyncApiIsValidAgainstTheOfficialSchema) {
    const std::vector<fs::path> documents = committed_documents();
    ASSERT_FALSE(documents.empty()) << "no *.asyncapi.json in spec/asyncapi";
    for (const fs::path& document : documents) {
        SCOPED_TRACE(document.filename().u8string());
        const std::vector<SchemaIssue> issues = official_schema().validate(read_json(document));
        EXPECT_TRUE(issues.empty()) << "invalid against the official AsyncAPI 3.0.0 schema:\n"
                                    << described(issues);
    }
}

// Copies of a committed document, each broken in one place, are refused, with every issue at that
// place or at an object that encloses it: so the test above cannot pass on a schema compiled into
// accepting anything, or on a validator that skips formats.
TEST(GraphExport, OfficialSchemaRefusesBrokenAsyncApiCopies) {
    struct Break {
        const char* what;
        const char* pointer;
        std::optional<json> value; // none: the member is removed
    };
    const std::vector<Break> breaks = {
        {"another AsyncAPI version", "/asyncapi", json("2.6.0")},
        {"an info without its version", "/info/version", std::nullopt},
        {"a channel address that is not a string", "/channels/estimate.state/address", json(7)},
        {"an action that is neither send nor receive", "/operations/estimate.state.publish/action",
         json("publish")},
        {"a message field AsyncAPI does not define",
         "/components/messages/pmp.prediction-set/bogus", json(1)},
        {"a payload that is not a JSON Schema",
         "/components/messages/pmp.prediction-set/payload/type", json("strng")},
        {"a data schema that is not a JSON Schema",
         "/components/messages/pmp.prediction-set/payload/properties/data/required", json("yes")},
        {"terms of service that are not a URI (format uri)", "/info/termsOfService",
         json("not a uri")},
        {"a pattern that is not a regular expression (format regex)",
         "/components/messages/pmp.prediction-set/payload/pattern", json("(")},
    };
    const json original = read_json(praenora::test::utf8_path(PRAENORA_TEST_SOURCE_DIR) / "spec" /
                                    "asyncapi" / "example.estimate-predict-0.1.0.asyncapi.json");
    const std::vector<SchemaIssue> original_issues = official_schema().validate(original);
    ASSERT_TRUE(original_issues.empty()) << described(original_issues);
    for (const Break& item : breaks) {
        SCOPED_TRACE(item.what);
        const json::json_pointer pointer(item.pointer);
        ASSERT_TRUE(original.contains(pointer.parent_pointer())) << item.pointer;
        json broken = original;
        if (item.value) {
            broken[pointer] = *item.value;
        } else {
            ASSERT_EQ(broken[pointer.parent_pointer()].erase(pointer.back()), 1u) << item.pointer;
        }
        const std::vector<SchemaIssue> issues = official_schema().validate(broken);
        EXPECT_FALSE(issues.empty());
        for (const SchemaIssue& issue : issues) {
            EXPECT_TRUE(at_or_above(issue.pointer, item.pointer)) << described(issues);
        }
    }
}
