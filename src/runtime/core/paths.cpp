#include "core/paths.hpp"

#include <praenora/runtime/error.hpp>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <thread>
#else
#include <unistd.h>
#endif

namespace praenora::detail {

namespace fs = std::filesystem;

namespace {

std::uint64_t process_id() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

[[noreturn]] void io_error(const std::string& message) {
    throw Error(ErrorCode::IO_ERROR, message);
}

std::string quoted(const fs::path& path) { return "'" + path_to_utf8(path) + "'"; }

} // namespace

fs::path path_from_utf8(std::string_view utf8) { return fs::u8path(utf8.begin(), utf8.end()); }

std::string path_to_utf8(const fs::path& path) { return path.u8string(); }

fs::path resolve_against(const fs::path& base_dir, std::string_view utf8) {
    const fs::path path = path_from_utf8(utf8);
    if (path.is_absolute()) {
        return path.lexically_normal();
    }
    return (base_dir / path).lexically_normal();
}

std::vector<std::string> split_path_list(std::string_view list) {
#ifdef _WIN32
    constexpr char kSeparator = ';';
#else
    constexpr char kSeparator = ':';
#endif
    std::vector<std::string> entries;
    std::size_t start = 0;
    while (start <= list.size()) {
        auto end = list.find(kSeparator, start);
        if (end == std::string_view::npos) {
            end = list.size();
        }
        if (end > start) {
            entries.emplace_back(list.substr(start, end - start));
        }
        start = end + 1;
    }
    return entries;
}

std::string read_file_bytes(const fs::path& file) {
    std::error_code ec;
    if (fs::is_directory(file, ec)) {
        io_error("cannot read " + quoted(file) + ": it is a directory");
    }
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) {
        if (!fs::exists(file, ec)) {
            io_error("cannot read " + quoted(file) + ": no such file");
        }
        io_error("cannot open " + quoted(file));
    }
    std::string data;
    std::vector<char> chunk(64 * 1024);
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        data.append(chunk.data(), static_cast<std::size_t>(in.gcount()));
    }
    if (in.bad()) {
        io_error("cannot read " + quoted(file));
    }
    return data;
}

void write_file_atomically(const fs::path& file, std::string_view data) {
    std::error_code ec;
    const fs::path parent = file.parent_path();
    if (!parent.empty() && !fs::is_directory(parent, ec)) {
        fs::create_directories(parent, ec);
        if (ec) {
            io_error("cannot create directory " + quoted(parent) + ": " + ec.message());
        }
    }

    // The counter keeps concurrent writers of one file in one process apart.
    static std::atomic<std::uint64_t> counter{0};
    fs::path temporary = file;
    temporary += ".tmp-" + std::to_string(process_id()) + "-" + std::to_string(counter++);

    const auto discard = [&] { fs::remove(temporary, ec); };
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            io_error("cannot write " + quoted(file) + ": cannot create " + quoted(temporary));
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.close();
        if (out.fail()) {
            discard();
            io_error("cannot write " + quoted(file));
        }
    }

#ifdef _WIN32
    // A reader (for example `praenora status` or a virus scanner) may hold the file open for a
    // moment, which makes the replacement fail with a sharing or access error: retry briefly.
    DWORD error = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (MoveFileExW(temporary.c_str(), file.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = ERROR_SUCCESS;
            break;
        }
        error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (error != ERROR_SUCCESS) {
        discard();
        io_error("cannot replace " + quoted(file) + ": " +
                 std::system_category().message(static_cast<int>(error)));
    }
#else
    fs::rename(temporary, file, ec);
    if (ec) {
        const std::string reason = ec.message();
        discard();
        io_error("cannot replace " + quoted(file) + ": " + reason);
    }
#endif
}

} // namespace praenora::detail
