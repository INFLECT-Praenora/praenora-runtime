// The OpenAPI document of the control API (src/server/openapi.hpp) and its committed copy
// spec/openapi/praenora-control-api.openapi.json, which must equal the generator's output byte for
// byte. PRAENORA_UPDATE_GOLDEN=1 makes OpenApi.DocumentMatchesCommittedFile rewrite it instead of
// comparing.
#include "control_api.hpp"
#include "openapi.hpp"

#include "module_fixtures.hpp"
#include "test_support.hpp"

#include <praenora/runtime/runtime.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Runtime;
using praenora::server::openapi_document;
using praenora::server::routes;

namespace {

const Runtime& runtime() {
    static const Runtime instance([] {
        praenora::RuntimeOptions options;
        options.use_environment = false;
        options.logging.to_stderr = false;
        return options;
    }());
    return instance;
}

std::string read_bytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Whether the committed file is to be rewritten (PRAENORA_UPDATE_GOLDEN=1) instead of compared.
bool update_golden() {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv is fine here: nothing changes the environment
#endif
    const char* value = std::getenv("PRAENORA_UPDATE_GOLDEN");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return value != nullptr && std::string(value) == "1";
}

fs::path committed_file() {
    return praenora::test::utf8_path(PRAENORA_TEST_SOURCE_DIR) / "spec" / "openapi" /
           "praenora-control-api.openapi.json";
}

// Every "$ref" value in `value`, depth first.
void collect_refs(const json& value, std::vector<std::string>& refs) {
    if (value.is_object()) {
        for (const auto& item : value.items()) {
            if (item.key() == "$ref" && item.value().is_string()) {
                refs.push_back(item.value().get<std::string>());
            } else {
                collect_refs(item.value(), refs);
            }
        }
    } else if (value.is_array()) {
        for (const json& item : value) {
            collect_refs(item, refs);
        }
    }
}

// Whether `value` or any value inside it is an object with the key `key`.
bool has_key(const json& value, const std::string& key) {
    if (value.is_object()) {
        for (const auto& item : value.items()) {
            if (item.key() == key || has_key(item.value(), key)) {
                return true;
            }
        }
    } else if (value.is_array()) {
        for (const json& item : value) {
            if (has_key(item, key)) {
                return true;
            }
        }
    }
    return false;
}

std::string lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

} // namespace

TEST(OpenApi, DocumentMatchesCommittedFile) {
    const std::string generated = openapi_document(runtime());
    const fs::path committed = committed_file();
    if (update_golden()) {
        praenora::test::write_text_file(committed, generated);
        std::cout << "refreshed " << committed.u8string() << "\n";
        return;
    }
    ASSERT_TRUE(fs::is_regular_file(committed))
        << committed.u8string() << " is missing; PRAENORA_UPDATE_GOLDEN=1 writes it";
    EXPECT_TRUE(read_bytes(committed) == generated)
        << committed.u8string()
        << " differs from the generated document; refresh it with PRAENORA_UPDATE_GOLDEN=1 "
           "(docs/testing.md) if the change is intended";
}

TEST(OpenApi, DocumentHasTheLayoutOfTheCommittedDocuments) {
    const std::string generated = openapi_document(runtime());
    const json document = json::parse(generated);
    // Keys sorted, two-space indentation, a final newline: as spec/asyncapi/'s files.
    EXPECT_EQ(generated, document.dump(2) + "\n");
    EXPECT_EQ(document.at("openapi"), "3.1.0");
    EXPECT_EQ(document.at("info").at("version"), "0.1.0");
    EXPECT_EQ(document.at("info").at("version"), std::string(praenora::server::kApiVersion));
    EXPECT_EQ(document.at("components").at("securitySchemes").at("bearer"),
              json({{"type", "http"}, {"scheme", "bearer"}}));
}

TEST(OpenApi, EveryRouteIsDocumented) {
    const json document = json::parse(openapi_document(runtime()));
    std::set<std::pair<std::string, std::string>> documented;
    for (const auto& path : document.at("paths").items()) {
        for (const auto& operation : path.value().items()) {
            if (operation.key() == "parameters") {
                continue;
            }
            documented.emplace(operation.key(), path.key());
        }
    }
    std::set<std::pair<std::string, std::string>> table;
    std::set<std::string> operation_ids;
    for (const praenora::server::Route& route : routes()) {
        SCOPED_TRACE(std::string(route.method) + " " + std::string(route.path));
        const std::string method = lower(std::string(route.method));
        table.emplace(method, std::string(route.path));
        EXPECT_TRUE(operation_ids.insert(std::string(route.operation_id)).second)
            << "operationId used twice";
        const json& operation = document.at("paths").at(std::string(route.path)).at(method);
        EXPECT_EQ(operation.at("operationId"), std::string(route.operation_id));
        EXPECT_EQ(operation.at("summary"), std::string(route.summary));
        EXPECT_EQ(operation.at("x-praenora-scope"),
                  std::string(praenora::server::scope_name(route.scope)));
        EXPECT_TRUE(operation.at("responses").contains(std::to_string(route.success_status)));
        for (const int status : route.error_statuses) {
            EXPECT_TRUE(operation.at("responses").contains(std::to_string(status))) << status;
        }
        EXPECT_EQ(operation.contains("requestBody"), !route.request_schema.empty());
        if (route.scope == praenora::server::RouteScope::none) {
            EXPECT_EQ(operation.at("security"), json::array());
        } else {
            EXPECT_FALSE(operation.contains("security"));
        }
    }
    // Every route, and nothing else.
    EXPECT_EQ(documented, table);
    EXPECT_EQ(table.size(), routes().size());
    EXPECT_EQ(routes().size(), 12u);
}

TEST(OpenApi, SchemasResolveAndEmbedTheRuntimesSchemas) {
    const json document = json::parse(openapi_document(runtime()));
    const json& schemas = document.at("components").at("schemas");
    for (const char* kind : {"status-snapshot", "run-record", "pmp-envelope"}) {
        SCOPED_TRACE(kind);
        EXPECT_EQ(schemas.at(kind), json::parse(runtime().schema(kind)));
        EXPECT_TRUE(schemas.at(kind).contains("$schema"));
        EXPECT_TRUE(schemas.at(kind).contains("$id"));
    }
    // The API's own schemas use nothing that draft-07 and 2020-12 read differently.
    for (const auto& schema : schemas.items()) {
        if (schemas.at(schema.key()).contains("$id")) {
            continue; // the runtime's, verbatim
        }
        SCOPED_TRACE(schema.key());
        EXPECT_FALSE(has_key(schema.value(), "definitions"));
        EXPECT_FALSE(has_key(schema.value(), "$defs"));
        EXPECT_FALSE(has_key(schema.value(), "dependencies"));
        EXPECT_FALSE(has_key(schema.value(), "$schema"));
    }
    // Every reference outside the embedded schemas names a component that exists.
    json outside = document;
    for (const char* kind : {"status-snapshot", "run-record", "pmp-envelope"}) {
        outside["components"]["schemas"].erase(kind);
    }
    std::vector<std::string> refs;
    collect_refs(outside, refs);
    EXPECT_FALSE(refs.empty());
    for (const std::string& ref : refs) {
        SCOPED_TRACE(ref);
        const std::string prefix = "#/components/";
        ASSERT_EQ(ref.rfind(prefix, 0), 0u);
        EXPECT_NO_THROW(document.at(json::json_pointer(ref.substr(1))));
    }
}

// Every POST names Content-Type application/json, its charset, if any, utf-8, stop's with its
// empty body too (the server answers 415 otherwise): the document says so on each request body and
// on each 415. Each POST lists 413 too, for a body over the listener's limit.
TEST(OpenApi, EveryPostNeedsTheJsonMediaType) {
    const json document = json::parse(openapi_document(runtime()));
    int posts = 0;
    for (const auto& path : document.at("paths").items()) {
        if (!path.value().contains("post")) {
            continue;
        }
        SCOPED_TRACE(path.key());
        ++posts;
        const json& post = path.value().at("post");
        const json& body = post.at("requestBody");
        EXPECT_EQ(body.at("content").size(), 1u);
        EXPECT_TRUE(body.at("content").contains("application/json"));
        const std::string description = body.at("description");
        EXPECT_NE(description.find("Content-Type: application/json"), std::string::npos)
            << description;
        if (!body.at("required").get<bool>()) {
            EXPECT_NE(description.find("empty body"), std::string::npos) << description;
        }
        const std::string refusal = post.at("responses").at("415").at("description");
        EXPECT_NE(refusal.find("Content-Type application/json"), std::string::npos) << refusal;
        EXPECT_NE(refusal.find("empty body"), std::string::npos) << refusal;
        EXPECT_NE(refusal.find("charset"), std::string::npos) << refusal;
        // A listener refuses a body over 64 KiB before the route sees it.
        const std::string too_large = post.at("responses").at("413").at("description");
        EXPECT_NE(too_large.find("64 KiB"), std::string::npos) << too_large;
    }
    EXPECT_EQ(posts, 2);
    const json& stop = document.at("paths").at("/v0/runs/{id}/stop").at("post");
    EXPECT_EQ(stop.at("requestBody").at("required"), false);
    EXPECT_NE(document.at("components")
                  .at("schemas")
                  .at("StopRequest")
                  .at("description")
                  .get<std::string>()
                  .find("application/json"),
              std::string::npos);
}
