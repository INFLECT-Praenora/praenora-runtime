// Numbers in target data, documents and canonical JSON do not depend on the process's locale: a
// German locale (decimal comma) changes nothing.
#include "catalog/catalog.hpp"
#include "config/workflow_config.hpp"
#include "core/semver.hpp"
#include "document/json_util.hpp"
#include "document/yaml_loader.hpp"
#include "io/input_source.hpp"

#include "module_fixtures.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <clocale>
#include <locale>
#include <stdexcept>
#include <string>

using nlohmann::json;
using praenora::test::ScratchDir;

namespace {

// Restores the global C++ locale and the C locale when the test ends.
class LocaleRestorer {
public:
    LocaleRestorer() : cpp_(std::locale()), c_(std::setlocale(LC_ALL, nullptr)) {}
    ~LocaleRestorer() {
        std::locale::global(cpp_);
        std::setlocale(LC_ALL, c_.c_str());
    }
    LocaleRestorer(const LocaleRestorer&) = delete;
    LocaleRestorer& operator=(const LocaleRestorer&) = delete;

private:
    std::locale cpp_;
    std::string c_;
};

} // namespace

TEST(Locale, NumericParsingIgnoresGlobalLocale) {
    const LocaleRestorer restore;
    bool changed = false;
    try {
        std::locale::global(std::locale("de_DE.UTF-8"));
        changed = true;
    } catch (const std::runtime_error&) {
        // Not installed; setlocale() below may still work.
    }
    if (std::setlocale(LC_ALL, "de_DE.UTF-8") != nullptr) {
        changed = true;
    }
    if (!changed) {
        GTEST_SKIP() << "the de_DE.UTF-8 locale is not available";
    }
    ASSERT_STREQ(std::localeconv()->decimal_point, ","); // the locale is really in effect

    // A CSV value.
    ScratchDir dir;
    praenora::test::write_text_file(dir.path() / "obs.csv",
                                    "series_id,variable_id,time,value\nS1,x,0.5,0.5\n");
    const auto catalog = praenora::detail::Catalog::load({});
    const auto* batch_type =
        catalog.find_message_type("pmp.observation-batch", praenora::detail::parse_semver("0.1.0"));
    ASSERT_NE(batch_type, nullptr);
    praenora::detail::InputBinding binding;
    binding.input = "observations";
    binding.source = "observation-csv";
    binding.path = dir.path() / "obs.csv";
    const auto source =
        praenora::detail::make_input_source(binding, *batch_type, praenora::detail::TimeBase{});
    const json rows = source->batch(0, 0.0, 1.0).at("observations");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.at(0).at("value").get<double>(), 0.5);
    EXPECT_EQ(rows.at(0).at("time").get<double>(), 0.5);
    EXPECT_EQ(praenora::detail::parse_decimal("0.5"), 0.5);
    EXPECT_FALSE(praenora::detail::parse_decimal("0,5").has_value());

    // A YAML document.
    const auto document = praenora::detail::parse_document_text("gain: 0.25\n", "p.yaml");
    EXPECT_EQ(document.json.at("gain").get<double>(), 0.25);

    // Canonical JSON.
    EXPECT_EQ(praenora::detail::canonical_json(json{{"x", 0.5}}), "{\"x\":0.5}");
}
