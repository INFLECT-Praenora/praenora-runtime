#include <praenora/runtime/version.hpp>

#include "document/schema_registry.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>

TEST(Version, ReportsConfiguredVersions) {
    const auto v = praenora::version_info();
    EXPECT_EQ(v.runtime, "0.2.0");
    EXPECT_EQ(v.sdk, "0.2.0");
    EXPECT_EQ(v.pmp, "0.1.0");
    EXPECT_EQ(v.abi_major, 0u);
    EXPECT_EQ(v.abi_minor, 0u);
    EXPECT_EQ(v.formats.at("module-manifest"), "0.2.0");
    EXPECT_EQ(v.formats.at("run-record"), "0.2.0");
    EXPECT_EQ(v.formats.at("workflow-config"), "0.1.0");
    EXPECT_EQ(v.formats.at("provider-manifest"), "0.1.0");
    EXPECT_FALSE(v.compiler.empty());
}

// Each document kind has a format version of its own, and the runtime reads documents of that
// version: the embedded schemas include its major.minor.
TEST(Version, ReportsOneFormatVersionPerKind) {
    const std::map<std::string, std::string> expected = {
        {"capability-definition", "0.1.0"}, {"message-type", "0.1.0"},
        {"module-check-report", "0.1.0"},   {"module-manifest", "0.2.0"},
        {"module-test-vectors", "0.1.0"},   {"provider-manifest", "0.1.0"},
        {"run-record", "0.2.0"},            {"status-snapshot", "0.1.0"},
        {"workflow-config", "0.1.0"},       {"workflow-definition", "0.1.0"},
    };
    const auto v = praenora::version_info();
    EXPECT_EQ(v.formats, expected);
    const auto& registry = praenora::detail::SchemaRegistry::builtin();
    for (const auto& [kind, version] : v.formats) {
        EXPECT_NO_THROW((void)registry.for_kind(kind, praenora::detail::parse_semver(version)))
            << kind;
    }
    // Documents of the earlier formats of the kinds that moved on are still read.
    for (const char* kind : {"module-manifest", "run-record"}) {
        EXPECT_NO_THROW((void)registry.for_kind(kind, praenora::detail::parse_semver("0.1.0")))
            << kind;
    }
}

TEST(Version, JsonContainsAllFields) {
    const auto j = praenora::version_info_json();
    for (auto k : {"\"runtime\"", "\"sdk\"", "\"pmp\"", "\"abi\"", "\"formats\""}) {
        EXPECT_NE(j.find(k), std::string::npos) << "missing key " << k << " in " << j;
    }
}
