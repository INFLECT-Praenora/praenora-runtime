// fx-sandbox-probe: tries what a module host's sandbox profile may refuse, and reports what each
// try got, for the tests of the sandbox profiles (docs/specs/process-isolation.md). Its parameters
// name a directory `outside` the module host's temporary directory, a file to `read` (outside it
// too, a scratch file of the test, which the tries below may append a byte to), and a TCP port on
// 127.0.0.1 to connect to (`connect_port`). Each invoke logs "fx-sandbox-probe: probing" at level
// info, then tries, in this order:
//   - connect: a TCP connection to 127.0.0.1:<connect_port>;
//   - fork: fork(), whose child ends at once;
//   - spawn: posix_spawn() of /usr/bin/true, waited for;
//   - write_outside: creating a file in `outside`;
//   - write_temp: creating and writing a file in std::filesystem::temp_directory_path();
//   - read: reading `read`;
//   - link_outside: a hard link to `read` made in the temporary directory;
//   - write_through_symlink: a symbolic link to `read` made in the temporary directory, opened for
//     writing (appending), and a byte written through it;
//   - stderr_is_null: whether descriptor 2 (standard error, which the module host has from the
//     runtime) is /dev/null, that is, has its device and inode numbers: true or false, or the errno
//     when either cannot be looked at (nothing is written);
//   - open_stderr: /dev/stderr opened for writing, by its path (nothing is written);
//   - write_stderr: with the parameter `write_stderr` true, the line "fx-sandbox-probe: written
//     through descriptor 2" written to descriptor 2 (for a test that gave the module host a file as
//     its standard error); -1 without it, and nothing is written;
//   - kept: with the parameter `keep` true, a file made in a directory "kept" of the temporary
//     directory and made immutable (UF_IMMUTABLE, macOS), so that nobody can remove it until the
//     flag is cleared; -1 without it;
// and returns {"outputs":{"out":{"data":{...}}}} with each try's errno (0 when it worked, the error
// number of posix_spawn() for spawn), "at_load" (the errno of creating a file, from a static
// initializer as the library loads, in the directory that PRAENORA_TEST_SANDBOX_PROBE_AT_LOAD
// names; -1 without it), "temporary_directory" (what temp_directory_path() names) and "tmpdir"
// (TMPDIR, or null). With PRAENORA_TEST_SANDBOX_PROBE_EXIT_AT_LOAD set too (to anything), a try at
// load that fails ends the process at once with exit code 70, as a library whose static
// initializer fails hard when it cannot write would. Links and files it made are removed again,
// but the kept one. POSIX only: on Windows every try reports -1.
#include <praenora/sdk/module.hpp>

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ; // NOLINT: the process environment, declared by POSIX
#endif

namespace {

namespace sdk = praenora::sdk;

#ifndef _WIN32
// The errno of creating (and removing again) a file named `name` in `directory`; 0 when it worked.
int create_file_in(const std::string& directory, const std::string& name) {
    const std::string file = directory + "/" + name;
    const int descriptor = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) {
        return errno;
    }
    const bool wrote = ::write(descriptor, "probe", 5) == 5;
    const int error = wrote ? 0 : errno;
    static_cast<void>(::close(descriptor));
    static_cast<void>(::unlink(file.c_str()));
    return error;
}
#endif

// What the static initializer got: -1 when it did not try.
int g_at_load = -1;

struct AtLoad {
    AtLoad() {
#ifndef _WIN32
        if (const char* directory = std::getenv("PRAENORA_TEST_SANDBOX_PROBE_AT_LOAD")) {
            g_at_load = create_file_in(directory, "at-load-" + std::to_string(::getpid()));
            if (g_at_load != 0 && std::getenv("PRAENORA_TEST_SANDBOX_PROBE_EXIT_AT_LOAD")) {
                std::_Exit(70);
            }
        }
#endif
    }
};

const AtLoad g_at_load_probe;

class SandboxProbeModule final : public sdk::Module {
public:
    explicit SandboxProbeModule(const sdk::CreateContext& context) : host_(context.host) {
        const auto document =
            nlohmann::json::parse(context.create_json.begin(), context.create_json.end());
        const nlohmann::json& parameters = document.at("parameters");
        outside_ = parameters.at("outside").get<std::string>();
        read_ = parameters.at("read").get<std::string>();
        port_ = parameters.at("connect_port").get<int>();
        keep_ = parameters.value("keep", false);
        write_stderr_ = parameters.value("write_stderr", false);
    }

    void initialize(const sdk::InitializeContext&) override {}

    std::string invoke(std::string_view) override {
        host_.log(sdk::LogLevel::info, "fx-sandbox-probe: probing");
        // In this order (a braced list is evaluated from left to right).
        nlohmann::json data = {{"connect", connect()},
                               {"fork", fork_child()},
                               {"spawn", spawn()},
                               {"write_outside", write_outside()},
                               {"write_temp", write_temp()},
                               {"read", read()},
                               {"link_outside", link_outside()},
                               {"write_through_symlink", write_through_symlink()},
                               {"stderr_is_null", stderr_is_null()},
                               {"open_stderr", open_stderr()},
                               {"write_stderr", write_stderr_ ? write_stderr() : -1},
                               {"kept", keep_ ? keep() : -1},
                               {"at_load", g_at_load}};
        std::error_code ec;
        data["temporary_directory"] = std::filesystem::temp_directory_path(ec).string();
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv is fine here: nothing changes the environment
#endif
        const char* tmpdir = std::getenv("TMPDIR");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        data["tmpdir"] = tmpdir != nullptr ? nlohmann::json(tmpdir) : nlohmann::json(nullptr);
        return nlohmann::json{{"outputs", {{"out", {{"data", data}}}}}}.dump();
    }

    void finalize() override {}

private:
#ifndef _WIN32
    int connect() const {
        const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor < 0) {
            return errno;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port_));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const int result =
            ::connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof address);
        const int error = result == 0 ? 0 : errno;
        static_cast<void>(::close(descriptor));
        return error;
    }

    static int fork_child() {
        const pid_t child = ::fork();
        if (child == 0) {
            ::_exit(0); // only async-signal-safe calls after fork() in a threaded process
        }
        if (child < 0) {
            return errno;
        }
        int status = 0;
        static_cast<void>(::waitpid(child, &status, 0));
        return 0;
    }

    static int spawn() {
        char program[] = "/usr/bin/true";
        char* argv[] = {program, nullptr};
        pid_t child = 0;
        const int error = ::posix_spawn(&child, program, nullptr, nullptr, argv, environ);
        if (error == 0) {
            int status = 0;
            static_cast<void>(::waitpid(child, &status, 0));
        }
        return error;
    }

    int write_outside() const {
        return create_file_in(outside_, "outside-" + std::to_string(::getpid()));
    }

    static int write_temp() {
        std::error_code ec;
        const std::filesystem::path directory = std::filesystem::temp_directory_path(ec);
        if (ec) {
            return ec.value();
        }
        return create_file_in(directory.string(), "temp-" + std::to_string(::getpid()));
    }

    int read() const {
        const int descriptor = ::open(read_.c_str(), O_RDONLY);
        if (descriptor < 0) {
            return errno;
        }
        char buffer[64];
        const ssize_t got = ::read(descriptor, buffer, sizeof buffer);
        const int error = got >= 0 ? 0 : errno;
        static_cast<void>(::close(descriptor));
        return error;
    }

    // A path in the temporary directory, for a link of this process named `name`.
    static std::string temporary(const std::string& name) {
        std::error_code ec;
        return (std::filesystem::temp_directory_path(ec) /
                (name + "-" + std::to_string(::getpid())))
            .string();
    }

    int link_outside() const {
        const std::string link = temporary("link");
        if (::link(read_.c_str(), link.c_str()) != 0) {
            return errno;
        }
        static_cast<void>(::unlink(link.c_str()));
        return 0;
    }

    int write_through_symlink() const {
        const std::string link = temporary("symlink");
        if (::symlink(read_.c_str(), link.c_str()) != 0) {
            return errno;
        }
        int error = 0;
        const int descriptor = ::open(link.c_str(), O_WRONLY | O_APPEND);
        if (descriptor < 0) {
            error = errno;
        } else {
            error = ::write(descriptor, "x", 1) == 1 ? 0 : errno;
            static_cast<void>(::close(descriptor));
        }
        static_cast<void>(::unlink(link.c_str()));
        return error;
    }

    static nlohmann::json stderr_is_null() {
        struct stat descriptor{};
        struct stat null{};
        if (::fstat(2, &descriptor) != 0 || ::stat("/dev/null", &null) != 0) {
            return errno;
        }
        return descriptor.st_dev == null.st_dev && descriptor.st_ino == null.st_ino;
    }

    static int write_stderr() {
        static const char line[] = "fx-sandbox-probe: written through descriptor 2\n";
        const ssize_t size = static_cast<ssize_t>(sizeof line - 1);
        return ::write(2, line, sizeof line - 1) == size ? 0 : errno;
    }

    static int open_stderr() {
        const int descriptor = ::open("/dev/stderr", O_WRONLY);
        if (descriptor < 0) {
            return errno;
        }
        static_cast<void>(::close(descriptor));
        return 0;
    }

    static int keep() {
        std::error_code ec;
        const std::filesystem::path directory = std::filesystem::temp_directory_path(ec) / "kept";
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            return ec.value();
        }
        const std::string file = (directory / "file").string();
        const int descriptor = ::open(file.c_str(), O_WRONLY | O_CREAT, 0600);
        if (descriptor < 0) {
            return errno;
        }
        static_cast<void>(::close(descriptor));
#if defined(__APPLE__)
        return ::chflags(file.c_str(), UF_IMMUTABLE) == 0 ? 0 : errno;
#else
        return -1;
#endif
    }
#else
    int connect() const { return -1; }
    static int fork_child() { return -1; }
    static int spawn() { return -1; }
    int write_outside() const { return -1; }
    static int write_temp() { return -1; }
    int read() const { return -1; }
    int link_outside() const { return -1; }
    int write_through_symlink() const { return -1; }
    static nlohmann::json stderr_is_null() { return -1; }
    static int open_stderr() { return -1; }
    static int write_stderr() { return -1; }
    static int keep() { return -1; }
#endif

    sdk::Host host_;
    std::string outside_, read_;
    int port_ = 0;
    bool keep_ = false;
    bool write_stderr_ = false;
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(SandboxProbeModule)
