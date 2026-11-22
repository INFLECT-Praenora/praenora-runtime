// The status-file writer: it replaces the file atomically at construction, on notify(), every
// interval and at destruction, creating its directory, and reports write errors without throwing.
#include "observability/status_file.hpp"

#include "core/paths.hpp"

#include "module_fixtures.hpp"

#include <praenora/runtime/error.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using praenora::ErrorCode;
using praenora::detail::StatusFileWriter;
using praenora::test::ScratchDir;

namespace {

std::string read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The file's text, or nullopt when it cannot be opened: on Windows, while the writer replaces the
// file, a reader may be refused it for a moment with a sharing violation, and sees no file then,
// never part of one.
std::optional<std::string> read_text_if_open(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Waits up to five seconds for `condition`.
bool eventually(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return condition();
}

// A JSON document of about 64 KiB naming `n`, so a torn write could not go unnoticed.
std::string big_document(int n) {
    return nlohmann::json{{"n", n}, {"padding", std::string(64 * 1024, 'x')}}.dump();
}

} // namespace

TEST(StatusFile, AtomicReplaceAndParentCreation) {
    ScratchDir dir;
    const fs::path file = dir.path() / "a" / "b" / "status.json";
    ASSERT_FALSE(fs::exists(dir.path() / "a"));
    std::atomic<int> counter{0};
    {
        StatusFileWriter writer(file, std::chrono::hours(1),
                                [&counter] { return big_document(counter.load()); });
        // Written once the constructor returns.
        ASSERT_TRUE(fs::exists(file));
        EXPECT_EQ(nlohmann::json::parse(read_text(file)).at("n"), 0);

        // A reader never sees a partial file while the writer keeps replacing it. On Windows a read
        // that could not open the file, during a replacement, read nothing and is not counted;
        // elsewhere rename() replaces the name at once, so a reader always finds the file.
        std::atomic<bool> done{false};
        std::atomic<int> torn{0}, reads{0};
        std::thread reader([&] {
            while (!done.load()) {
                const std::optional<std::string> text = read_text_if_open(file);
                if (!text) {
#ifndef _WIN32
                    ++torn;
#endif
                    continue;
                }
                if (!nlohmann::json::accept(*text)) {
                    ++torn;
                }
                ++reads;
            }
        });
        for (int i = 1; i <= 100; ++i) {
            counter = i;
            writer.notify();
            std::this_thread::sleep_for(200us);
        }
        EXPECT_TRUE(eventually([&] {
            const std::optional<std::string> text = read_text_if_open(file);
            return text && nlohmann::json::parse(*text).at("n") == 100;
        }));
        done = true;
        reader.join();
        EXPECT_EQ(torn.load(), 0);
        EXPECT_GT(reads.load(), 0);
    }
    // Only the status file is left: no temporary files.
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(file.parent_path())) {
        names.push_back(entry.path().filename().u8string());
    }
    EXPECT_EQ(names, (std::vector<std::string>{"status.json"}));
}

TEST(StatusFile, FinalWriteOnDestruction) {
    ScratchDir dir;
    const fs::path file = dir.path() / "status.json";
    std::atomic<int> value{1};
    {
        StatusFileWriter writer(file, std::chrono::hours(1),
                                [&value] { return std::to_string(value.load()); });
        EXPECT_EQ(read_text(file), "1");
        value = 2;
        std::this_thread::sleep_for(20ms);
        EXPECT_EQ(read_text(file), "1"); // no write without a reason
    }
    EXPECT_EQ(read_text(file), "2");
}

TEST(StatusFile, WritesOnNotifyAndEveryInterval) {
    ScratchDir dir;
    // Periodic writes.
    {
        const fs::path file = dir.path() / "periodic.json";
        std::atomic<int> value{0};
        StatusFileWriter writer(file, 10ms, [&value] { return std::to_string(value.load()); });
        value = 7;
        EXPECT_TRUE(eventually([&] { return read_text(file) == "7"; }));
    }
    // No interval: only notify() (and the end) write.
    {
        const fs::path file = dir.path() / "notified.json";
        std::atomic<int> value{0};
        StatusFileWriter writer(file, std::chrono::nanoseconds(0),
                                [&value] { return std::to_string(value.load()); });
        value = 3;
        std::this_thread::sleep_for(20ms);
        EXPECT_EQ(read_text(file), "0");
        writer.notify();
        EXPECT_TRUE(eventually([&] { return read_text(file) == "3"; }));
    }
}

TEST(StatusFile, WriteErrorsAreReported) {
    ScratchDir dir;
    // The status file's directory would have to be where a regular file is.
    praenora::test::write_text_file(dir.path() / "occupied", "not a directory");
    std::atomic<int> errors{0};
    std::atomic<int> io_errors{0};
    {
        StatusFileWriter writer(
            dir.path() / "occupied" / "status.json", std::chrono::hours(1),
            [] { return std::string("{}"); },
            [&](const praenora::Error& error) {
                ++errors;
                if (error.code() == ErrorCode::IO_ERROR) {
                    ++io_errors;
                }
            });
        EXPECT_EQ(errors.load(), 1); // the construction's write
        writer.notify();
        EXPECT_TRUE(eventually([&] { return errors.load() == 2; }));
    }
    EXPECT_EQ(errors.load(), 3); // and the final one
    EXPECT_EQ(io_errors.load(), 3);
    // A producer that throws is reported too, and the writer keeps going.
    std::atomic<int> producer_errors{0};
    {
        StatusFileWriter writer(
            dir.path() / "status.json", std::chrono::hours(1),
            []() -> std::string { throw std::runtime_error("no snapshot"); },
            [&](const praenora::Error&) { ++producer_errors; });
    }
    EXPECT_EQ(producer_errors.load(), 2);
}
