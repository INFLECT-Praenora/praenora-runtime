#include "isolation/sandbox.hpp"

#include "core/paths.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <stdlib.h> // mkdtemp() (glibc)
#include <unistd.h> // mkdtemp() (macOS)
#endif

#if defined(__APPLE__)
#include <sandbox.h>
#endif

namespace praenora::detail {

namespace fs = std::filesystem;

namespace {

// `text` as a string of the sandbox profile language: in double quotes, with backslashes and
// double quotes escaped.
std::string quoted(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '\\' || c == '"') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

} // namespace

std::string_view sandbox_profile_name(SandboxProfile profile) noexcept {
    return profile == SandboxProfile::restricted ? "restricted" : "none";
}

std::optional<SandboxProfile> sandbox_profile_from_name(std::string_view name) noexcept {
    if (name == "none") {
        return SandboxProfile::none;
    }
    if (name == "restricted") {
        return SandboxProfile::restricted;
    }
    return std::nullopt;
}

bool restricted_sandbox_available() noexcept {
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

std::string restricted_profile_text(std::string_view temporary_directory) {
    // Allowed by default; the denials follow, and the last rule that matches an operation decides,
    // so the writes allowed at the end are the only ones left. Besides fork() and exec(), new
    // processes could come from a launchd job (job-creation), from LaunchServices opening an
    // application (lsopen) or from scripting another application (appleevent-send).
    return "(version 1)\n"
           "(allow default)\n"
           "(deny network*)\n"
           "(deny process-fork)\n"
           "(deny process-exec)\n"
           "(deny job-creation)\n"
           "(deny lsopen)\n"
           "(deny appleevent-send)\n"
           "(deny file-write*)\n"
           "(allow file-write* (subpath " +
           quoted(temporary_directory) + ") (literal \"/dev/null\"))\n";
}

std::optional<std::string> apply_restricted_profile(std::string_view temporary_directory) {
#if defined(__APPLE__)
    const std::string profile = restricted_profile_text(temporary_directory);
    char* error = nullptr;
    // sandbox_init() is deprecated, and has no successor for a process that sandboxes itself; its
    // flags 0 takes the profile's text. The warning is silenced here only.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    errno = 0;
    const int status = ::sandbox_init(profile.c_str(), 0, &error);
    const int saved = errno;
    std::optional<std::string> problem;
    if (status != 0) {
        problem = error != nullptr && error[0] != '\0' ? std::string(error)
                  : saved != 0                         ? std::string(std::strerror(saved))
                                                       : std::string("sandbox_init() failed");
    }
    if (error != nullptr) {
        ::sandbox_free_error(error);
    }
#pragma GCC diagnostic pop
    return problem;
#else
    static_cast<void>(temporary_directory);
    return std::string("the restricted sandbox profile is available only on macOS in this version");
#endif
}

std::optional<PrivateTemporaryDirectory> PrivateTemporaryDirectory::make(std::string& problem) {
#ifndef _WIN32
    std::error_code ec;
    const fs::path system = fs::canonical(fs::temp_directory_path(ec), ec);
    if (ec) {
        problem = "the system's temporary directory cannot be found: " + ec.message();
        return std::nullopt;
    }
    std::string pattern = path_to_utf8(system / "praenora-module-host-XXXXXX");
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    if (::mkdtemp(name.data()) == nullptr) {
        problem = std::strerror(errno);
        return std::nullopt;
    }
    return PrivateTemporaryDirectory(std::string(name.data()));
#else
    problem = "not available on Windows in this version";
    return std::nullopt;
#endif
}

PrivateTemporaryDirectory::PrivateTemporaryDirectory(std::string path) noexcept
    : path_(std::move(path)) {}

PrivateTemporaryDirectory::PrivateTemporaryDirectory(PrivateTemporaryDirectory&& other) noexcept
    : path_(std::exchange(other.path_, std::string())) {}

PrivateTemporaryDirectory&
PrivateTemporaryDirectory::operator=(PrivateTemporaryDirectory&& other) noexcept {
    if (this != &other) {
        static_cast<void>(remove());
        path_ = std::exchange(other.path_, std::string());
    }
    return *this;
}

PrivateTemporaryDirectory::~PrivateTemporaryDirectory() { static_cast<void>(remove()); }

const std::string& PrivateTemporaryDirectory::path() const noexcept { return path_; }

std::optional<std::string> PrivateTemporaryDirectory::remove() noexcept {
    if (path_.empty()) {
        return std::nullopt;
    }
    std::optional<std::string> left;
    try {
        const fs::path root = path_from_utf8(path_);
        give_owner_rights(root);
        std::error_code ec;
        static_cast<void>(fs::remove_all(root, ec));
        if (ec) {
            left = path_ + ": " + ec.message();
        } else if (fs::exists(fs::symlink_status(root, ec))) {
            left = path_ + ": it is still there";
        }
    } catch (...) {
        left = path_ + ": out of memory";
    }
    path_.clear();
    return left;
}

void PrivateTemporaryDirectory::give_owner_rights(const fs::path& directory) {
    // Never through a symbolic link: a module may put one anywhere in its directory, the
    // directory itself included, and what it names is not the module host's.
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(directory, ec))) {
        return;
    }
    fs::permissions(directory, fs::perms::owner_all, fs::perm_options::add, ec);
    for (fs::directory_iterator entry(directory, ec), end; !ec && entry != end;
         entry.increment(ec)) {
        std::error_code status_error;
        if (fs::is_directory(entry->symlink_status(status_error))) {
            give_owner_rights(entry->path());
        }
    }
}

} // namespace praenora::detail
