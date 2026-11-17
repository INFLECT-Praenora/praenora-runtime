// What the two front ends share (src/frontend/frontend.hpp): names taken in any letter case, folded
// the same way whatever the process's locale, and the JSON of a service provider.
#include <frontend.hpp>

#include <praenora/runtime/options.hpp>
#include <praenora/runtime/runtime.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <clocale>
#include <optional>
#include <stdexcept>
#include <string>

namespace frontend = praenora::frontend;
using nlohmann::json;

namespace {

// Sets LC_CTYPE for the life of the object, then restores the earlier setting.
class CtypeLocale {
public:
    explicit CtypeLocale(const char* name) {
        const char* current = std::setlocale(LC_CTYPE, nullptr);
        saved_ = current != nullptr ? current : "C";
        active_ = std::setlocale(LC_CTYPE, name) != nullptr;
    }
    ~CtypeLocale() { std::setlocale(LC_CTYPE, saved_.c_str()); }
    CtypeLocale(const CtypeLocale&) = delete;
    CtypeLocale& operator=(const CtypeLocale&) = delete;

    bool active() const { return active_; }

private:
    std::string saved_;
    bool active_ = false;
};

} // namespace

TEST(Frontend, NamesFoldOnlyAsciiLetters) {
    EXPECT_EQ(frontend::lower("InFo-JSON_0"), "info-json_0");
    EXPECT_EQ(frontend::log_level_from_name("WARN"), praenora::LogLevel::warn);
    EXPECT_EQ(frontend::log_format_from_name("Json"), praenora::LogFormat::json);
    EXPECT_EQ(frontend::log_level_from_name("verbose"), std::nullopt);

    // A locale whose letter case covers bytes beyond ASCII changes nothing: only A-Z fold. The
    // Python interpreter, for one, takes LC_CTYPE from the environment. The first of these
    // locales that the system has and that folds 0xC4 (A with diaeresis in ISO 8859-1, and the
    // character U+00C4 to some C libraries in UTF-8 locales) is used.
    for (const char* name : {"en_US.UTF-8", "C.UTF-8", "de_DE.ISO8859-1", "de_DE.ISO-8859-1",
                             "de_DE.iso88591", "en_US.ISO8859-1", "en_US.iso88591"}) {
        CtypeLocale locale(name);
        if (!locale.active() || std::tolower(0xC4) == 0xC4) {
            continue;
        }
        EXPECT_EQ(frontend::lower("INFO\xC4\xD6"), "info\xC4\xD6") << name;
        EXPECT_EQ(frontend::log_level_from_name("Debug"), praenora::LogLevel::debug) << name;
        return;
    }
    GTEST_SKIP() << "no locale on this system folds bytes beyond ASCII";
}

// The JSON of a provider has the keys of `praenora providers list --json` and list_providers():
// the services split into their name, major and minor, the engine as an object, and "problem"
// only for a provider that is not usable.
TEST(Frontend, ProviderJsonHasTheKeysOfTheListings) {
    praenora::ProviderInfo provider;
    provider.name = "test-echo";
    provider.version = "0.1.0";
    provider.manifest_path = "/p/test-echo/test-echo.provider.json";
    provider.library_path = "/p/test-echo/test-echo.so";
    provider.os = "macos";
    provider.arch = "arm64";
    provider.services = {"test.echo@0.0", "praenora.simulation@4294967295.12"};
    provider.engine_json = R"({"name":"echo","version":"1.0.0"})";
    provider.usable = true;
    EXPECT_EQ(frontend::provider_json(provider),
              json({{"name", "test-echo"},
                    {"version", "0.1.0"},
                    {"manifest", "/p/test-echo/test-echo.provider.json"},
                    {"library", "/p/test-echo/test-echo.so"},
                    {"platform", {{"os", "macos"}, {"arch", "arm64"}}},
                    {"services",
                     {{{"name", "test.echo"}, {"major", 0}, {"minor", 0}},
                      {{"name", "praenora.simulation"}, {"major", 4294967295u}, {"minor", 12}}}},
                    {"engine", {{"name", "echo"}, {"version", "1.0.0"}}},
                    {"usable", true}}));

    provider.usable = false;
    provider.problem = "built for runtime 0.0.1; this runtime is 0.2.0";
    const json unusable = frontend::provider_json(provider);
    EXPECT_EQ(unusable.at("usable"), false);
    EXPECT_EQ(unusable.at("problem"), provider.problem);
    EXPECT_EQ(unusable.size(), 9u);
}

TEST(Frontend, ServiceJsonRefusesTextOfAnotherForm) {
    EXPECT_EQ(frontend::service_json("a.b-c@1.2"),
              json({{"name", "a.b-c"}, {"major", 1}, {"minor", 2}}));
    for (const char* text : {"test.echo", "@0.0", "test.echo@0", "test.echo@0.", "test.echo@.0",
                             "test.echo@x.0", "test.echo@0.0x", "test.echo@-1.0",
                             "test.echo@4294967296.0"}) {
        EXPECT_THROW(frontend::service_json(text), std::invalid_argument) << text;
    }
}
