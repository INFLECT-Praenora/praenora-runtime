#include <praenora/runtime/version.hpp>

#include <gtest/gtest.h>

#include <string>

TEST(Version, ReportsConfiguredVersions) {
    const auto v = praenora::version_info();
    EXPECT_EQ(v.runtime, "0.1.0");
    EXPECT_EQ(v.sdk, "0.1.0");
    EXPECT_EQ(v.pmp, "0.1.0");
    EXPECT_EQ(v.abi_major, 0u);
    EXPECT_EQ(v.abi_minor, 0u);
    EXPECT_EQ(v.formats.at("module-manifest"), "0.1.0");
    EXPECT_EQ(v.formats.at("workflow-config"), "0.1.0");
    EXPECT_FALSE(v.compiler.empty());
}

TEST(Version, JsonContainsAllFields) {
    const auto j = praenora::version_info_json();
    for (auto k : {"\"runtime\"", "\"sdk\"", "\"pmp\"", "\"abi\"", "\"formats\""}) {
        EXPECT_NE(j.find(k), std::string::npos) << "missing key " << k << " in " << j;
    }
}
