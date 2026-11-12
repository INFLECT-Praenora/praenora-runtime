// Core utilities: SHA-256, SemVer, durations, RFC 3339 time, seed derivation, UTF-8 paths and the
// comparison of JSON values with a tolerance.
#include "core/duration.hpp"
#include "core/json_near.hpp"
#include "core/paths.hpp"
#include "core/seed.hpp"
#include "core/semver.hpp"
#include "core/sha256.hpp"
#include "core/time.hpp"

#include "temp_dir.hpp"

#include <praenora/runtime/error.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using praenora::Error;
using praenora::ErrorCode;
namespace detail = praenora::detail;
using praenora::test::ScratchDir;

namespace {

template <typename Fn> ErrorCode error_code_of(Fn&& fn) {
    try {
        fn();
    } catch (const Error& e) {
        return e.code();
    }
    return ErrorCode::OK;
}

std::string read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST(Sha256, KnownVectors) {
    EXPECT_EQ(detail::sha256_hex(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(detail::sha256_hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string million(1000000, 'a');
    EXPECT_EQ(detail::sha256_hex(million),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // Two-block message from FIPS 180-4.
    EXPECT_EQ(detail::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256, IncrementalUpdatesMatchOneShot) {
    const std::string million(1000000, 'a');
    // Chunk sizes that straddle the 64-byte block boundary in different ways.
    for (const std::size_t chunk : {1u, 3u, 55u, 56u, 63u, 64u, 65u, 1000u, 4096u}) {
        detail::Sha256 hash;
        for (std::size_t offset = 0; offset < million.size(); offset += chunk) {
            const auto size = std::min(chunk, million.size() - offset);
            hash.update(million.data() + offset, size);
        }
        EXPECT_EQ(hash.finish_hex(),
                  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")
            << "chunk " << chunk;
    }
    detail::Sha256 reused;
    reused.update("abc", 3);
    EXPECT_EQ(reused.finish_hex(), detail::sha256_hex("abc"));
    // After finishing, the object starts over.
    EXPECT_EQ(reused.finish_hex(), detail::sha256_hex(""));
}

TEST(Sha256, FileMatchesString) {
    ScratchDir dir;
    std::string content;
    for (int i = 0; i < 100000; ++i) {
        content += static_cast<char>(i % 251);
    }
    const auto file = dir.path() / fs::u8path("d\xC3\xA9j\xC3\xA0 vu.bin");
    {
        std::ofstream out(file, std::ios::binary);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    EXPECT_EQ(detail::sha256_file_hex(file), detail::sha256_hex(content));
}

TEST(Sha256, MissingFileIsIoError) {
    ScratchDir dir;
    EXPECT_EQ(error_code_of([&] { detail::sha256_file_hex(dir.path() / "missing.bin"); }),
              ErrorCode::IO_ERROR);
}

TEST(SemVer, ParseAndCompare) {
    using detail::parse_semver;
    const auto alpha = parse_semver("1.0.0-alpha");
    const auto alpha1 = parse_semver("1.0.0-alpha.1");
    const auto beta = parse_semver("1.0.0-beta");
    const auto release = parse_semver("1.0.0");
    EXPECT_LT(alpha, alpha1);
    EXPECT_LT(alpha1, beta);
    EXPECT_LT(beta, release);
    EXPECT_GT(release, alpha);
    EXPECT_LE(alpha, alpha);
    EXPECT_GE(release, beta);

    // The full precedence example of SemVer 2.0.0, section 11.
    const std::vector<std::string> ordered = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta",
                                              "1.0.0-beta",  "1.0.0-beta.2",  "1.0.0-beta.11",
                                              "1.0.0-rc.1",  "1.0.0",         "1.0.1",
                                              "1.1.0",       "2.0.0",         "10.0.0"};
    for (std::size_t i = 1; i < ordered.size(); ++i) {
        EXPECT_LT(parse_semver(ordered[i - 1]), parse_semver(ordered[i]))
            << ordered[i - 1] << " < " << ordered[i];
    }

    // Build metadata is ignored in comparisons but kept.
    const auto built = parse_semver("1.0.0+build.5");
    EXPECT_EQ(built, release);
    EXPECT_FALSE(built < release || release < built);
    EXPECT_EQ(built.build, "build.5");
    EXPECT_EQ(parse_semver("1.2.3-rc.1+exp.sha.5114f85").prerelease, "rc.1");

    const auto parsed = parse_semver("12.34.56");
    EXPECT_EQ(parsed.major, 12u);
    EXPECT_EQ(parsed.minor, 34u);
    EXPECT_EQ(parsed.patch, 56u);

    for (const char* bad : {"01.0.0", "1.02.0", "1.0.03", "1", "1.0", "1.0.0.0", "", "a.b.c",
                            "1.0.0-", "1.0.0+", "1.0.0-01", "1.0.0-a..b", "1.0.0-a_b", " 1.0.0",
                            "1.0.0 ", "v1.0.0", "-1.0.0", "18446744073709551616.0.0"}) {
        EXPECT_EQ(error_code_of([&] { parse_semver(bad); }), ErrorCode::INVALID_ARGUMENT)
            << "'" << bad << "'";
    }
}

TEST(SemVer, ToStringRoundTrips) {
    for (const char* text : {"0.1.0", "1.2.3-rc.1", "1.2.3+build.7", "2.0.0-alpha.1+exp.sha"}) {
        EXPECT_EQ(detail::to_string(detail::parse_semver(text)), text);
    }
    EXPECT_STREQ(detail::kPmpVersion, "0.1.0");
}

TEST(SemVer, CompatibilityClass) {
    using detail::parse_semver;
    using detail::same_compatibility_class;
    EXPECT_TRUE(same_compatibility_class(parse_semver("0.1.0"), parse_semver("0.1.9")));
    EXPECT_FALSE(same_compatibility_class(parse_semver("0.1.0"), parse_semver("0.2.0")));
    EXPECT_TRUE(same_compatibility_class(parse_semver("1.2.0"), parse_semver("1.9.3")));
    EXPECT_FALSE(same_compatibility_class(parse_semver("1.0.0"), parse_semver("2.0.0")));
    EXPECT_FALSE(same_compatibility_class(parse_semver("0.1.0"), parse_semver("1.1.0")));
}

TEST(Duration, Parse) {
    using detail::parse_duration;
    using std::chrono::nanoseconds;
    EXPECT_EQ(parse_duration("30s"), nanoseconds(30000000000));
    EXPECT_EQ(parse_duration("250ms"), nanoseconds(250000000));
    EXPECT_EQ(parse_duration("1.5s"), nanoseconds(1500000000));
    EXPECT_EQ(parse_duration("2m"), std::chrono::seconds(120));
    EXPECT_EQ(parse_duration("1h"), std::chrono::seconds(3600));
    EXPECT_EQ(parse_duration("none"), nanoseconds(0));
    EXPECT_EQ(parse_duration("0"), nanoseconds(0));
    EXPECT_EQ(parse_duration("7ns"), nanoseconds(7));
    EXPECT_EQ(parse_duration("12us"), nanoseconds(12000));
    EXPECT_EQ(parse_duration("0.25h"), std::chrono::seconds(900));
    EXPECT_EQ(parse_duration("1.000s"), std::chrono::seconds(1));
    for (const char* bad : {"5", "-1s", "1d", "", "s", "1.s", ".5s", "1.5ns", "1 s", " 1s", "1s ",
                            "1S", "none ", "99999999999999999999h", "3000000h"}) {
        EXPECT_EQ(error_code_of([&] { parse_duration(bad); }), ErrorCode::CONFIG_INVALID)
            << "'" << bad << "'";
    }
}

TEST(Duration, FormatIsCanonicalAndParsesBack) {
    using detail::format_duration;
    using detail::parse_duration;
    using std::chrono::nanoseconds;
    EXPECT_EQ(format_duration(std::chrono::seconds(300)), "300s");
    EXPECT_EQ(format_duration(std::chrono::seconds(60)), "60s");
    EXPECT_EQ(format_duration(std::chrono::milliseconds(1500)), "1500ms");
    EXPECT_EQ(format_duration(std::chrono::microseconds(12)), "12us");
    EXPECT_EQ(format_duration(nanoseconds(7)), "7ns");
    EXPECT_EQ(format_duration(nanoseconds(0)), "none");
    EXPECT_EQ(format_duration(nanoseconds(-5)), "none");
    for (const char* text : {"30s", "1.5m", "2h", "250ms", "0.001ms", "7ns", "none"}) {
        EXPECT_EQ(parse_duration(format_duration(parse_duration(text))), parse_duration(text))
            << text;
    }
}

TEST(Time, Rfc3339) {
    EXPECT_EQ(detail::format_rfc3339_utc(0), "1970-01-01T00:00:00.000000000Z");
    EXPECT_EQ(detail::format_rfc3339_utc(1700000000123456789), "2023-11-14T22:13:20.123456789Z");
    EXPECT_EQ(detail::format_rfc3339_utc(1), "1970-01-01T00:00:00.000000001Z");
}

TEST(Time, NowIsAPlausibleWallClockInstant) {
    const auto now = detail::now_unix_ns();
    EXPECT_GT(now, 1700000000000000000); // after November 2023
    const std::regex rfc3339(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{9}Z$)");
    EXPECT_TRUE(std::regex_match(detail::format_rfc3339_utc(now), rfc3339));
}

TEST(Seed, DerivationMatchesSpec) {
    using detail::derive_step_seed;
    EXPECT_EQ(derive_step_seed(20261110, "estimate"), 4059344203658876u);
    EXPECT_EQ(derive_step_seed(20261110, "predict"), 6154932436776580u);
    EXPECT_EQ(derive_step_seed(0, "estimate"), 6194768065828698u);
    EXPECT_EQ(derive_step_seed(20261110, "\xC3\xA9tape"), 4589988129017875u);
    EXPECT_EQ(detail::kMaxJsonSafeInteger, 9007199254740991u);
    for (const std::uint64_t run_seed : {std::uint64_t{0}, std::uint64_t{1},
                                         std::uint64_t{20261110}, detail::kMaxJsonSafeInteger}) {
        for (const char* step : {"a", "estimate", "predict", "z_9"}) {
            EXPECT_LE(derive_step_seed(run_seed, step), detail::kMaxJsonSafeInteger);
        }
    }
}

TEST(Paths, Utf8RoundTrip) {
    const std::string utf8 = "dir with space/t\xC3\xABst";
    const auto path = detail::path_from_utf8(utf8);
    EXPECT_EQ(path.filename().u8string(), std::string("t\xC3\xABst"));
    EXPECT_EQ(path.parent_path().u8string(), std::string("dir with space"));
    EXPECT_EQ(detail::path_from_utf8(detail::path_to_utf8(path)), path);
    EXPECT_EQ(detail::path_to_utf8(path), path.u8string());
}

TEST(Paths, ResolveAgainstBaseDirectory) {
    const auto base = fs::temp_directory_path() / "base dir";
    EXPECT_EQ(detail::resolve_against(base, "data/obs.csv"),
              (base / "data" / "obs.csv").lexically_normal());
    EXPECT_EQ(detail::resolve_against(base, "../x/./y.json"),
              (base.parent_path() / "x" / "y.json").lexically_normal());
    const auto absolute = fs::temp_directory_path() / "elsewhere" / "f.yaml";
    EXPECT_EQ(detail::resolve_against(base, detail::path_to_utf8(absolute)),
              absolute.lexically_normal());
}

TEST(Paths, SplitPathList) {
#ifdef _WIN32
    const std::string list = "C:\\modules;;D:\\more modules;";
    const std::vector<std::string> expected = {"C:\\modules", "D:\\more modules"};
#else
    const std::string list = "/opt/modules::/home/me/more modules:";
    const std::vector<std::string> expected = {"/opt/modules", "/home/me/more modules"};
#endif
    EXPECT_EQ(detail::split_path_list(list), expected);
    EXPECT_TRUE(detail::split_path_list("").empty());
}

TEST(Paths, AtomicWriteCreatesParents) {
    ScratchDir dir;
    const auto file = dir.path() / fs::u8path("nested dir/\xC3\xA9tat") / "status.json";
    detail::write_file_atomically(file, "{\"a\":1}\n");
    EXPECT_EQ(read_all(file), "{\"a\":1}\n");
    // Replacing keeps exactly one file with the new content and leaves no temporary file.
    detail::write_file_atomically(file, "second");
    EXPECT_EQ(read_all(file), "second");
    std::vector<fs::path> entries;
    for (const auto& entry : fs::directory_iterator(file.parent_path())) {
        entries.push_back(entry.path().filename());
    }
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0], fs::path("status.json"));
    // Bytes are written unchanged: no newline translation.
    const std::string binary("a\r\nb\n\0c", 7);
    detail::write_file_atomically(file, binary);
    EXPECT_EQ(read_all(file), binary);
}

TEST(Paths, ReadFileBytes) {
    ScratchDir dir;
    const auto file = dir.path() / "data.txt";
    const std::string binary("x\r\ny\0z", 6);
    detail::write_file_atomically(file, binary);
    EXPECT_EQ(detail::read_file_bytes(file), binary);
    EXPECT_EQ(error_code_of([&] { detail::read_file_bytes(dir.path() / "missing.txt"); }),
              ErrorCode::IO_ERROR);
    EXPECT_EQ(error_code_of([&] { detail::read_file_bytes(dir.path()); }), ErrorCode::IO_ERROR)
        << "a directory is not a readable file";
}

TEST(Paths, AtomicWriteIntoAFileFails) {
    ScratchDir dir;
    const auto blocker = dir.path() / "blocker";
    detail::write_file_atomically(blocker, "x");
    EXPECT_EQ(error_code_of([&] { detail::write_file_atomically(blocker / "child.json", "y"); }),
              ErrorCode::IO_ERROR);
}

TEST(JsonNear, NumbersMatchWithinTheLargerTolerance) {
    using nlohmann::json;
    // |a - b| <= max(abs, rel * max(|a|, |b|)); zero tolerances ask for equal numbers.
    EXPECT_TRUE(detail::json_near(json(1.0), json(1.0), 0, 0, nullptr));
    EXPECT_FALSE(detail::json_near(json(1.0), json(1.0000001), 0, 0, nullptr));
    EXPECT_TRUE(detail::json_near(json(100.0), json(100.5), 0.01, 0, nullptr)); // rel 1.005
    EXPECT_FALSE(detail::json_near(json(100.0), json(102.0), 0.01, 0, nullptr));
    EXPECT_TRUE(detail::json_near(json(0.0), json(1e-13), 0, 1e-12, nullptr)); // abs
    EXPECT_TRUE(detail::json_near(json(2), json(2.0), 0, 0, nullptr)); // integers and floats
    EXPECT_FALSE(detail::json_near(json(1.0), json("1.0"), 1, 1, nullptr));
}

TEST(JsonNear, ReportsTheFirstDifference) {
    using nlohmann::json;
    std::string where;
    const json a = {{"data", {{"values", {1.0, 2.0, 3.0}}, {"unit", "d"}}}};
    json b = a;
    b["data"]["values"][1] = 2.5;
    EXPECT_FALSE(detail::json_near(a, b, 0.1, 0, &where));
    EXPECT_EQ(where, "/data/values/1: 2.0 differs from 2.5");
    EXPECT_TRUE(detail::json_near(a, b, 0.25, 0, &where));

    b = a;
    b["data"].erase("unit");
    EXPECT_FALSE(detail::json_near(a, b, 0, 0, &where));
    EXPECT_EQ(where, "/data/unit: only in the first");
    b = a;
    b["data"]["values"].push_back(4.0);
    EXPECT_FALSE(detail::json_near(a, b, 0, 0, &where));
    EXPECT_EQ(where, "/data/values: 3 items differ from 4 items");
    EXPECT_FALSE(detail::json_near(json{{"a/b", 1}}, json{{"a/b", true}}, 0, 0, &where));
    EXPECT_EQ(where, "/a~1b: a number (1) differs from a boolean (true)");
}
