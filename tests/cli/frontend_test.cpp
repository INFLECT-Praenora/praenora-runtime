// What the two front ends share (src/frontend/frontend.hpp): names taken in any letter case, folded
// the same way whatever the process's locale.
#include <frontend.hpp>

#include <praenora/runtime/options.hpp>

#include <gtest/gtest.h>

#include <cctype>
#include <clocale>
#include <optional>
#include <string>

namespace frontend = praenora::frontend;

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
