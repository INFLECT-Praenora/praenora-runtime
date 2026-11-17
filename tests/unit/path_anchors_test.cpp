// The directories a run knows and their placeholders in the error records of the files a run
// writes (observability/path_anchors.hpp): each placeholder, the whole-path boundary (dots that
// end a sentence included), the longest anchor and the precedence of equal directories,
// directories reached through symbolic links, directories that are never anchors, the parts of
// error records and status snapshots that are replaced, dyld's cryptex prefix on macOS and Windows
// paths; and the home directory (platform/platform.hpp).
#include "observability/path_anchors.hpp"

#include "core/paths.hpp"
#include "discovery/search_paths.hpp"
#include "platform/platform.hpp"

#include "directory_checks.hpp"
#include "scoped_environment.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::detail::path_to_utf8;
using praenora::detail::PathAnchors;
using praenora::detail::RunDirectories;
using praenora::detail::SearchPath;
using praenora::detail::SearchPathOrigin;
using praenora::test::named_path;
using praenora::test::ScratchDir;

namespace {

// The root directory: "/", or "C:\" on Windows.
fs::path root() {
#ifdef _WIN32
    return fs::path("C:\\");
#else
    return fs::path("/");
#endif
}

// `relative` ('/'-separated) below the root directory, with the platform's separators.
fs::path absolute(std::string_view relative) {
    fs::path path = root() / praenora::test::utf8_path(relative);
    path.make_preferred();
    return path;
}

std::string u(const fs::path& path) { return path_to_utf8(path); }

} // namespace

TEST(PathAnchors, NamesEachDirectoryByItsPlaceholder) {
    RunDirectories directories;
    directories.output = absolute("pa/run/out");
    directories.configuration = absolute("pa/run/config");
    directories.modules = {
        {absolute("pa/m/caller"), SearchPathOrigin::caller, 0},
        {absolute("pa/m/configured"), SearchPathOrigin::configuration, 1},
        {absolute("pa/m/environment"), SearchPathOrigin::environment, 2},
        {absolute("pa/m/installed"), SearchPathOrigin::installation, std::nullopt}};
    directories.providers = {
        {absolute("pa/p/caller"), SearchPathOrigin::caller, 3},
        {absolute("pa/p/environment"), SearchPathOrigin::environment, 0},
        {absolute("pa/p/installed"), SearchPathOrigin::installation, std::nullopt}};
    directories.home = absolute("pa/home/user");
    const PathAnchors anchors(directories);
    const std::vector<std::pair<fs::path, std::string>> cases = {
        {absolute("pa/run/out"), "<output>"},
        {absolute("pa/run/config"), "<configuration>"},
        {absolute("pa/m/caller"), "<modules:caller:0>"},
        {absolute("pa/m/configured"), "<modules:configuration:1>"},
        {absolute("pa/m/environment"), "<modules:environment:2>"},
        {absolute("pa/m/installed"), "<modules:installation>"},
        {absolute("pa/p/caller"), "<providers:caller:3>"},
        {absolute("pa/p/environment"), "<providers:environment:0>"},
        {absolute("pa/p/installed"), "<providers:installation>"},
        {absolute("pa/home/user"), "<home>"},
    };
    for (const auto& [directory, placeholder] : cases) {
        SCOPED_TRACE(u(directory));
        EXPECT_EQ(anchors.apply(u(directory)), placeholder);
        // The rest of a path in the directory is kept.
        const fs::path file = directory / "fx-echo" / "fx-echo.pmm.json";
        EXPECT_EQ(anchors.apply(u(file)), named_path(placeholder, directory, file));
    }
    // A directory given with a trailing separator, or with a "." segment, is the same directory.
    RunDirectories untidy;
    untidy.output = fs::path(u(absolute("pa/run/out")) + u(fs::path("/").make_preferred()));
    untidy.configuration = absolute("pa/run/./config");
    const PathAnchors tidied(untidy);
    EXPECT_EQ(tidied.apply(u(absolute("pa/run/out"))), "<output>");
    EXPECT_EQ(tidied.apply(u(absolute("pa/run/config"))), "<configuration>");
}

TEST(PathAnchors, ReplacesAPathInsideAQuotedMessage) {
    RunDirectories directories;
    directories.output = absolute("pa/run/out");
    const PathAnchors anchors(directories);
    const fs::path outputs = absolute("pa/run/out") / "outputs";
    EXPECT_EQ(anchors.apply("cannot create directory '" + u(outputs) + "': File exists"),
              "cannot create directory '" +
                  named_path("<output>", absolute("pa/run/out"), outputs) + "': File exists");
}

TEST(PathAnchors, ReplacesOnlyWholePaths) {
    RunDirectories directories;
    directories.output = absolute("pa/out");
    const PathAnchors anchors(directories);
    const std::string out = u(absolute("pa/out"));
    // Directories that merely share a beginning or an end with it stay as they are.
    for (const fs::path& other :
         {absolute("pa/out2"), absolute("pa/out-old"), absolute("pa/out.d"), absolute("pa/out_1"),
          absolute("pa/out~"), absolute("pa/out\xC3\xA9"), absolute("a/pa/out")}) {
        EXPECT_EQ(anchors.apply(u(other)), u(other));
    }
    for (const char* before : {"a", "0", ".", "_", "-", "~", "\xC3\xA9"}) {
        EXPECT_EQ(anchors.apply(before + out), before + out) << before;
    }
    // Characters that do not continue a path segment are boundaries, on either side.
    for (const char* boundary : {"'", "\"", " ", "\t", "\n", "(", ")", ",", ":", ";", "=", "[",
                                 "]", "<",  ">", "{",  "}",  "|", "!", "?", "*", "&", "@"}) {
        EXPECT_EQ(anchors.apply(boundary + out + boundary),
                  std::string(boundary) + "<output>" + boundary)
            << boundary;
    }
    EXPECT_EQ(anchors.apply("dlopen(" + out + ", 0x0006): x=" + out + "; [" + out + "]"),
              "dlopen(<output>, 0x0006): x=<output>; [<output>]");
}

TEST(PathAnchors, ReplacesEveryOccurrenceFromLeftToRight) {
    RunDirectories directories;
    directories.output = absolute("pa/out");
    directories.home = absolute("pa/home");
    const PathAnchors anchors(directories);
    const fs::path out = absolute("pa/out");
    const fs::path home = absolute("pa/home");
    EXPECT_EQ(anchors.apply("from '" + u(out) + "' to '" + u(out / "x") + "' (" + u(home / "y") +
                            ") and " + u(out)),
              "from '<output>' to '" + named_path("<output>", out, out / "x") + "' (" +
                  named_path("<home>", home, home / "y") + ") and <output>");
#ifndef _WIN32
    // Replaced text is never scanned again: the second "/pa/out" follows a "t" in the text, not
    // the ">" of a placeholder, so it is no whole path.
    EXPECT_EQ(anchors.apply("/pa/out/pa/out"), "<output>/pa/out");
#endif
}

TEST(PathAnchors, LongestAnchorWins) {
    RunDirectories directories;
    directories.home = absolute("pa/home/user");
    directories.output = absolute("pa/home/user/runs/out");
    directories.modules = {{absolute("pa/home/user/modules"), SearchPathOrigin::caller, 0}};
    const PathAnchors anchors(directories);
    const fs::path record = absolute("pa/home/user/runs/out/run-record.json");
    EXPECT_EQ(anchors.apply(u(record)),
              named_path("<output>", absolute("pa/home/user/runs/out"), record));
    const fs::path data = absolute("pa/home/user/data/x.csv");
    EXPECT_EQ(anchors.apply(u(data)), named_path("<home>", absolute("pa/home/user"), data));
    const fs::path manifest = absolute("pa/home/user/modules/fx/fx.pmm.json");
    EXPECT_EQ(anchors.apply(u(manifest)),
              named_path("<modules:caller:0>", absolute("pa/home/user/modules"), manifest));

    // A longer anchor wins over a shorter one that comes first in precedence and holds it: a module
    // search path inside the configuration's directory, and one inside the output directory.
    RunDirectories inside;
    inside.configuration = absolute("pa/proj");
    inside.modules = {{absolute("pa/proj/modules"), SearchPathOrigin::caller, 0}};
    const PathAnchors configured(inside);
    const fs::path module_file = absolute("pa/proj/modules/x");
    EXPECT_EQ(configured.apply(u(module_file)),
              named_path("<modules:caller:0>", absolute("pa/proj/modules"), module_file));
    const fs::path config_file = absolute("pa/proj/y");
    EXPECT_EQ(configured.apply(u(config_file)),
              named_path("<configuration>", absolute("pa/proj"), config_file));
    inside.configuration.clear();
    inside.output = absolute("pa/proj");
    const PathAnchors output(inside);
    EXPECT_EQ(output.apply(u(module_file)),
              named_path("<modules:caller:0>", absolute("pa/proj/modules"), module_file));
    EXPECT_EQ(output.apply(u(config_file)),
              named_path("<output>", absolute("pa/proj"), config_file));
}

TEST(PathAnchors, EqualDirectoriesTakeThePlaceholderThatComesFirst) {
    const fs::path same = absolute("pa/same");
    RunDirectories directories;
    directories.output = same;
    directories.configuration = same;
    directories.modules = {{absolute("pa/other"), SearchPathOrigin::caller, 0},
                           {same, SearchPathOrigin::environment, 1},
                           {same, SearchPathOrigin::installation, std::nullopt}};
    directories.providers = {{same, SearchPathOrigin::environment, 2},
                             {same, SearchPathOrigin::caller, 0}};
    directories.home = same;
    EXPECT_EQ(PathAnchors(directories).apply(u(same)), "<output>");
    directories.output.reset();
    EXPECT_EQ(PathAnchors(directories).apply(u(same)), "<configuration>");
    directories.configuration.clear();
    EXPECT_EQ(PathAnchors(directories).apply(u(same)), "<modules:environment:1>");
    directories.modules.clear();
    EXPECT_EQ(PathAnchors(directories).apply(u(same)), "<providers:environment:2>");
    directories.providers.clear();
    EXPECT_EQ(PathAnchors(directories).apply(u(same)), "<home>");
}

TEST(PathAnchors, MatchesADirectoryAlsoWithItsLinksResolved) {
    ScratchDir scratch;
    const fs::path real = scratch.path() / "real";
    fs::create_directories(real / "out");
    const fs::path link = scratch.path() / "link";
    std::error_code ec;
    fs::create_directory_symlink(real, link, ec);
    if (ec) {
        GTEST_SKIP() << "cannot create a symbolic link here: " << ec.message();
    }
    RunDirectories directories;
    directories.output = link / "out";
    const PathAnchors anchors(directories);
    // The directory as given, through the link, and as the system names it, with every link
    // resolved (on macOS the temporary directory itself lies behind /var -> /private/var).
    const fs::path resolved = fs::canonical(real / "out");
    for (const fs::path& directory : {link / "out", resolved}) {
        SCOPED_TRACE(u(directory));
        const fs::path record = directory / "run-record.json";
        EXPECT_EQ(anchors.apply("'" + u(record) + "'"),
                  "'" + named_path("<output>", directory, record) + "'");
    }
    // A directory that does not exist yet: the part of it that exists is resolved.
    directories.output = link / "later" / "out";
    const fs::path later = fs::canonical(real) / "later" / "out";
    EXPECT_EQ(PathAnchors(directories).apply(u(later)), "<output>");
}

TEST(PathAnchors, SkipsAHomeThatIsARootOrRelative) {
    const fs::path file = absolute("pa/x");
    RunDirectories directories;
    directories.home = root();
    EXPECT_EQ(PathAnchors(directories).apply(u(file)), u(file));
    EXPECT_EQ(PathAnchors(directories).apply(u(root())), u(root()));
    directories.home = fs::path("relative") / "home";
    const std::string relative = u(fs::path("relative") / "home" / "x");
    EXPECT_EQ(PathAnchors(directories).apply(relative), relative);
    directories.home = fs::path();
    EXPECT_EQ(PathAnchors(directories).apply(u(file)), u(file));
}

TEST(PathAnchors, NeverAnchorsARootDirectory) {
    RunDirectories directories;
    directories.configuration = root(); // a configuration file in the root directory
    directories.output = absolute("pa/..");
    directories.modules = {{root(), SearchPathOrigin::caller, 0}};
    directories.providers = {{root(), SearchPathOrigin::environment, 0}};
    const PathAnchors anchors(directories);
    const fs::path file = absolute("etc/x");
    EXPECT_EQ(anchors.apply(u(file)), u(file));
    EXPECT_EQ(anchors.apply(u(root())), u(root()));
}

TEST(PathAnchors, ReplacesTheMessageFileDetailsAndCauseOfAnErrorRecordButNoKey) {
    const fs::path out = absolute("pa/out");
    const fs::path home = absolute("pa/home");
    RunDirectories directories;
    directories.output = out;
    directories.home = home;
    const PathAnchors anchors(directories);
    const std::string key = u(home / "key");
    const auto in_out = [&](const fs::path& path) { return named_path("<output>", out, path); };
    const auto in_home = [&](const fs::path& path) { return named_path("<home>", home, path); };

    // A module's error document as the cause.
    json record = {{"code", 700},
                   {"name", "STEP_FAILED"},
                   {"message", "step 'a' failed: cannot read '" + u(home / "in.csv") + "'"},
                   {"tick", 3},
                   {"at", "2023-11-14T22:13:20.123456789Z"},
                   {"cause",
                    {{"code", 11},
                     {"message", "bad row in " + u(home / "in.csv")},
                     {"details", {{"files", {u(home / "a"), 2, u(out)}}}}}},
                   {"location", {{"file", u(out / "outputs")}, {"line", 2}, {"column", 5}}},
                   {"details",
                    {{key, u(home)},
                     {"nested", {{"list", {u(out), 7, true, nullptr, {{"deep", u(home / "x")}}}}}},
                     {"size", 12},
                     {"ratio", 0.5}}}};
    anchors.apply_to_error_record(record);
    EXPECT_EQ(record,
              json({{"code", 700},
                    {"name", "STEP_FAILED"},
                    {"message", "step 'a' failed: cannot read '" + in_home(home / "in.csv") + "'"},
                    {"tick", 3},
                    {"at", "2023-11-14T22:13:20.123456789Z"},
                    {"cause",
                     {{"code", 11},
                      {"message", "bad row in " + in_home(home / "in.csv")},
                      {"details", {{"files", {in_home(home / "a"), 2, "<output>"}}}}}},
                    {"location", {{"file", in_out(out / "outputs")}, {"line", 2}, {"column", 5}}},
                    {"details",
                     {{key, "<home>"},
                      {"nested",
                       {{"list", {"<output>", 7, true, nullptr, {{"deep", in_home(home / "x")}}}}}},
                      {"size", 12},
                      {"ratio", 0.5}}}}));

    // A cause that is not a JSON object is kept as {"text": ...}; null parts stay null.
    json text_cause = record;
    text_cause["cause"] = {{"text", "see " + u(out / "log")}};
    text_cause["location"] = nullptr;
    text_cause["details"] = nullptr;
    anchors.apply_to_error_record(text_cause);
    EXPECT_EQ(text_cause.at("cause"), json({{"text", "see " + in_out(out / "log")}}));
    EXPECT_EQ(text_cause.at("location"), nullptr);
    EXPECT_EQ(text_cause.at("details"), nullptr);
}

// The other members of an error record are not paths: its name, its time and the pointer of its
// location (a JSON pointer, which can read like a path under a directory at the root) stay as they
// are.
TEST(PathAnchors, LeavesTheNameTimeAndPointerOfAnErrorRecordAlone) {
    const fs::path library = absolute("library");
    RunDirectories directories;
    directories.modules = {{library, SearchPathOrigin::caller, 0}};
    const PathAnchors anchors(directories);
    const fs::path manifest = library / "fx" / "fx.provider.json";
    const fs::path file = library / "fx" / "fx.so";
    json record = {{"code", 9},
                   {"name", u(library)},
                   {"message", "cannot load '" + u(file) + "'"},
                   {"tick", nullptr},
                   {"at", u(library)},
                   {"cause", nullptr},
                   {"location", {{"file", u(manifest)}, {"pointer", "/library/path"}}},
                   {"details", nullptr}};
    json expected = record;
    expected["message"] = "cannot load '" + named_path("<modules:caller:0>", library, file) + "'";
    expected["location"]["file"] = named_path("<modules:caller:0>", library, manifest);
    anchors.apply_to_error_record(record);
    EXPECT_EQ(record, expected);
#ifndef _WIN32
    // As text, the pointer would be a path under the search path /library.
    EXPECT_EQ(anchors.apply("/library/path"), "<modules:caller:0>/path");
#endif
}

// A run of dots right after a directory ends the path when the text ends there, or goes on with a
// character that neither continues a segment nor is a separator: the end of a sentence. Dots that
// continue a name, or come before a separator, belong to another path.
TEST(PathAnchors, TakesDotsAfterADirectoryForPunctuationWhenThePathEndsThere) {
    RunDirectories directories;
    directories.output = absolute("pa/out");
    directories.home = absolute("pa/home/u");
    const PathAnchors anchors(directories);
    const std::string out = u(absolute("pa/out"));
    const std::string home = u(absolute("pa/home/u"));
    EXPECT_EQ(anchors.apply("... not found in " + home + "."), "... not found in <home>.");
    EXPECT_EQ(anchors.apply("'" + home + "'."), "'<home>'.");
    EXPECT_EQ(anchors.apply("in " + out + "... and " + out + ". Then (" + out + ".)"),
              "in <output>... and <output>. Then (<output>.)");
    EXPECT_EQ(anchors.apply(out + ".."), "<output>..");
    for (const std::string& other : {out + ".d", out + "./x", out + "...x", out + "..d", out + "._",
                                     out + ".\xC3\xA9", out + ".~"}) {
        EXPECT_EQ(anchors.apply(other), other);
        EXPECT_EQ(anchors.apply("in " + other + "."), "in " + other + ".");
    }
}

TEST(PathAnchors, ReplacesOnlyTheErrorRecordsOfAStatusSnapshot) {
    const fs::path out = absolute("pa/out");
    RunDirectories directories;
    directories.output = out;
    const PathAnchors anchors(directories);
    const auto record = [&] {
        return json({{"code", 9},
                     {"name", "IO_ERROR"},
                     {"message", "cannot write '" + u(out / "status.json") + "'"},
                     {"tick", nullptr},
                     {"at", nullptr},
                     {"cause", nullptr},
                     {"location", {{"file", u(out / "status.json")}}},
                     {"details", nullptr}});
    };
    json snapshot = {{"kind", "status-snapshot"},
                     {"workflow", {{"name", u(out)}, {"last_error", record()}}},
                     {"steps",
                      {{{"step", "a"}, {"module_name", u(out)}, {"last_error", record()}},
                       {{"step", "b"}, {"module_name", u(out)}, {"last_error", nullptr}}}}};
    json expected = snapshot;
    const std::string status = named_path("<output>", out, out / "status.json");
    for (json* error : {&expected["workflow"]["last_error"], &expected["steps"][0]["last_error"]}) {
        (*error)["message"] = "cannot write '" + status + "'";
        (*error)["location"]["file"] = status;
    }
    anchors.apply_to_status_snapshot(snapshot);
    EXPECT_EQ(snapshot, expected);
    // Not a snapshot: nothing to change, nothing thrown.
    json other = {{"workflow", u(out)}, {"steps", 3}};
    anchors.apply_to_status_snapshot(other);
    EXPECT_EQ(other, json({{"workflow", u(out)}, {"steps", 3}}));
}

#ifdef __APPLE__
// dyld (macOS 13 and later) lists every path it tried for a library it could not open, the
// library's own path also behind the OS cryptex's prefix: the directory is replaced there too, and
// the prefix kept.
TEST(PathAnchors, MatchesBehindDyldsCryptexPrefixOnMacOs) {
    RunDirectories directories;
    directories.modules = {{"/pa/modules", SearchPathOrigin::caller, 0}};
    directories.home = "/pa/home/user";
    const PathAnchors anchors(directories);
    EXPECT_EQ(anchors.apply(
                  "dlopen(/pa/modules/fx/lib.dylib, 0x0006): tried: '/pa/modules/fx/lib.dylib' "
                  "(slice is not valid mach-o file), "
                  "'/System/Volumes/Preboot/Cryptexes/OS/pa/modules/fx/lib.dylib' (no such file), "
                  "'/pa/modules/fx/lib.dylib' (slice is not valid mach-o file)"),
              "dlopen(<modules:caller:0>/fx/lib.dylib, 0x0006): tried: "
              "'<modules:caller:0>/fx/lib.dylib' (slice is not valid mach-o file), "
              "'/System/Volumes/Preboot/Cryptexes/OS<modules:caller:0>/fx/lib.dylib' (no such "
              "file), '<modules:caller:0>/fx/lib.dylib' (slice is not valid mach-o file)");
    // Any one segment after Cryptexes/, at the start of the text too.
    EXPECT_EQ(anchors.apply("/System/Volumes/Preboot/Cryptexes/App/pa/home/user/x.dylib"),
              "/System/Volumes/Preboot/Cryptexes/App<home>/x.dylib");
    EXPECT_EQ(anchors.apply("(/System/Volumes/Preboot/Cryptexes/OS/pa/home/user)"),
              "(/System/Volumes/Preboot/Cryptexes/OS<home>)");
    // The prefix must itself begin at a boundary, and one segment, no more and no less, must follow
    // it.
    for (const char* text : {"/x/System/Volumes/Preboot/Cryptexes/OS/pa/home/user",
                             "x/System/Volumes/Preboot/Cryptexes/OS/pa/home/user",
                             "/System/Volumes/Preboot/Cryptexes/OS/extra/pa/home/user",
                             "/System/Volumes/Preboot/Cryptexes//pa/home/user",
                             "/System/Volumes/Preboot/CryptexesOS/pa/home/user",
                             "/System/Volumes/Preboot/Cryptexes/pa/home/user"}) {
        EXPECT_EQ(anchors.apply(text), text);
    }
}
#endif

#ifdef _WIN32
TEST(PathAnchors, OnWindowsBothSeparatorsMatchAndCaseDoesNotMatter) {
    RunDirectories directories;
    directories.output = fs::path(L"C:\\Users\\Ann\\runs\\out");
    const PathAnchors anchors(directories);
    EXPECT_EQ(anchors.apply("c:/users/ann/RUNS/out/run-record.json"), "<output>/run-record.json");
    EXPECT_EQ(anchors.apply("'C:\\Users\\Ann\\runs\\out\\outputs'"), "'<output>\\outputs'");
    EXPECT_EQ(anchors.apply("C:\\Users\\Ann\\runs\\out2"), "C:\\Users\\Ann\\runs\\out2");
    EXPECT_EQ(anchors.apply("D:\\Users\\Ann\\runs\\out"), "D:\\Users\\Ann\\runs\\out");
}
#endif

#ifndef _WIN32
TEST(Platform, HomeDirectoryComesFromHomeThenThePasswordDatabase) {
    {
        const praenora::test::ScopedEnvironment home("HOME", std::string("/pa/home/user"));
        EXPECT_EQ(praenora::detail::home_directory(), std::optional<fs::path>("/pa/home/user"));
    }
    // Unset or empty: the password database's entry of the user.
    for (const std::optional<std::string>& value :
         {std::optional<std::string>(), std::optional<std::string>("")}) {
        const praenora::test::ScopedEnvironment home("HOME", value);
        // A password database without an entry for the user gives none.
        if (const std::optional<fs::path> found = praenora::detail::home_directory()) {
            EXPECT_TRUE(found->is_absolute()) << u(*found);
        }
    }
    // A value that is no anchor is returned as it is; PathAnchors skips it.
    const praenora::test::ScopedEnvironment relative("HOME", std::string("relative"));
    EXPECT_EQ(praenora::detail::home_directory(), std::optional<fs::path>("relative"));
}
#else
TEST(Platform, HomeDirectoryComesFromUserProfile) {
    const praenora::test::ScopedEnvironment profile("USERPROFILE", std::string("C:\\Users\\Ann"));
    EXPECT_EQ(praenora::detail::home_directory(), std::optional<fs::path>(L"C:\\Users\\Ann"));
    const praenora::test::ScopedEnvironment unset("USERPROFILE", std::nullopt);
    EXPECT_FALSE(praenora::detail::home_directory().has_value());
}
#endif
