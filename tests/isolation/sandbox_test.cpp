// The sandbox profiles of a module host, white-box (docs/specs/process-isolation.md#the-restricted-
// profile), with the fixture module fx-sandbox-probe, which tries to connect, fork, spawn, write
// outside and inside its temporary directory and read, as its library loads and in an invoke:
// without a profile (none) a module host may do what the runtime's process may; the restricted
// profile refuses the network, new processes and writes outside the module host's own temporary
// directory, which its TMPDIR names and which is removed once it has ended, and leaves reads, those
// writes, the channel (records, outputs) and the loading of libraries alone. A process that runs in
// a sandbox already (macOS applies no second one) cannot give a module host the profile: the load
// fails with MODULE_HOST_UNAVAILABLE, reason "sandbox"; the tests that need the profile applied
// skip there, saying "nested sandbox:" (sandbox_support.hpp).
#include "core/paths.hpp"
#include "discovery/manifest.hpp"
#include "instance/module_instance.hpp"
#include "isolation/child_process.hpp"
#include "isolation/child_settings.hpp"
#include "isolation/sandbox.hpp"
#include "loader/loaded_module.hpp"

#include "instance_fixtures.hpp"
#include "isolation_support.hpp"
#include "module_fixtures.hpp"
#include "sandbox_support.hpp"
#include "scoped_environment.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <praenora/runtime/error.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::Error;
using praenora::ErrorCode;
using praenora::detail::CallOutcome;
using praenora::detail::ChildSettings;
using praenora::detail::LoadedModule;
using praenora::detail::ModuleInstance;
using praenora::detail::SandboxProfile;
using praenora::test::CapturedLogs;
using praenora::test::ScratchDir;

#ifndef _WIN32

namespace {

std::string utf8(const fs::path& path) { return praenora::detail::path_to_utf8(path); }

// What fx-sandbox-probe reports from a module host with `profile`, and the module host's records.
struct Probed {
    json data;
    CapturedLogs logs;
};

// The line fx-sandbox-probe writes through its descriptor 2 when its parameter write_stderr is
// true.
constexpr const char* kStandardErrorLine = "fx-sandbox-probe: written through descriptor 2\n";

// Points this process's descriptor 2 at `file` (made, or emptied) while it lives, and back at what
// it was then: a module host started meanwhile inherits the file as its standard error, as it
// inherits the runtime's (a module host takes descriptors 1 and 2 as they are when it starts).
// Whatever this process writes to its standard error meanwhile goes there too.
class StandardErrorTo {
public:
    explicit StandardErrorTo(const fs::path& file) {
        static_cast<void>(std::fflush(stderr));
        saved_ = ::fcntl(2, F_DUPFD_CLOEXEC, 0);
        const int descriptor = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (saved_ >= 0 && descriptor >= 0) {
            redirected_ = ::dup2(descriptor, 2) == 2;
        }
        if (descriptor >= 0) {
            static_cast<void>(::close(descriptor));
        }
    }
    ~StandardErrorTo() {
        static_cast<void>(std::fflush(stderr));
        if (redirected_) {
            static_cast<void>(::dup2(saved_, 2));
        }
        if (saved_ >= 0) {
            static_cast<void>(::close(saved_));
        }
    }
    StandardErrorTo(const StandardErrorTo&) = delete;
    StandardErrorTo& operator=(const StandardErrorTo&) = delete;

    bool redirected() const noexcept { return redirected_; }

private:
    int saved_ = -1;
    bool redirected_ = false;
};

// Loads fx-sandbox-probe in a module host with `profile`, its static initializer writing into
// `outside` too, and has it probe: writing in `outside`, reading `read` (and linking to it) and
// connecting to `port`; with `keep`, it leaves an immutable file in its temporary directory. With
// `standard_error`, the module host starts with that file as its standard error (this process's
// descriptor 2 points at it only while the module host starts), and the probe writes
// kStandardErrorLine through its descriptor 2. The module host has ended, and its temporary
// directory has been removed, when this returns.
void probe(SandboxProfile profile, const fs::path& outside, const fs::path& read, int port,
           Probed& probed, bool keep = false, const fs::path* standard_error = nullptr) {
    const praenora::test::ScopedEnvironment at_load("PRAENORA_TEST_SANDBOX_PROBE_AT_LOAD",
                                                    utf8(outside));
    ChildSettings settings = praenora::test::child_settings(probed.logs);
    settings.sandbox = profile;
    std::optional<StandardErrorTo> redirect;
    if (standard_error != nullptr) {
        redirect.emplace(*standard_error);
        ASSERT_TRUE(redirect->redirected()) << *standard_error;
    }
    const auto module = LoadedModule::load_in_child(
        praenora::detail::load_manifest(praenora::test::fixture_manifest("fx-sandbox-probe")),
        settings);
    redirect.reset();
    ModuleInstance instance(module, praenora::test::instance_options(probed.logs));
    const praenora::detail::CallResult created = instance.create(praenora::test::create_document(
        "fx-sandbox-probe", {{"outside", utf8(outside)},
                             {"read", utf8(read)},
                             {"connect_port", port},
                             {"keep", keep},
                             {"write_stderr", standard_error != nullptr}}));
    ASSERT_EQ(created.outcome, CallOutcome::succeeded) << created.error_json;
    ASSERT_EQ(instance.initialize(praenora::test::initialize_document()).outcome,
              CallOutcome::succeeded);
    const praenora::detail::CallResult invoked =
        instance.invoke(praenora::test::invocation_document(0), 0);
    ASSERT_EQ(invoked.outcome, CallOutcome::succeeded) << invoked.error_json;
    probed.data = json::parse(invoked.output).at("outputs").at("out").at("data");
}

// The whole text of `file`.
std::string text_of(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The errno this process gets when it opens /dev/stderr for writing; 0 when it can.
int own_open_stderr() {
    const int descriptor = ::open("/dev/stderr", O_WRONLY);
    if (descriptor < 0) {
        return errno;
    }
    static_cast<void>(::close(descriptor));
    return 0;
}

// The records of `logs` whose event is `event`.
std::vector<json> events_named(const CapturedLogs& logs, const std::string& event) {
    std::vector<json> found;
    for (const json& record : logs.events()) {
        if (record.at("attributes").value("event", std::string()) == event) {
            found.push_back(record);
        }
    }
    return found;
}

// Whether `logs` hold a record whose body is `body`.
bool recorded(const CapturedLogs& logs, const std::string& body) {
    for (const json& record : logs.events()) {
        if (record.at("body") == body) {
            return true;
        }
    }
    return false;
}

} // namespace

// Without a profile (none, the default) a module host may do what the runtime's process may: it
// connects and opens /dev/stderr as this process does (where a sandbox around the tests refuses
// one, it is refused alike), forks, spawns, writes anywhere this process may, also through a hard
// link or a symbolic link to a file outside, and reads; its standard error is this process's, and
// its temporary directory too.
TEST(Isolation, AModuleHostWithoutASandboxMayDoWhatTheRuntimesProcessMay) {
    const ScratchDir outside;
    const fs::path read = outside.path() / "read.txt";
    praenora::test::write_text_file(read, "read me");
    const praenora::test::LoopbackListener listener;
    Probed probed;
    probe(SandboxProfile::none, outside.path(), read, listener.port(), probed);
    ASSERT_TRUE(probed.data.is_object());
    EXPECT_EQ(probed.data.at("connect"), praenora::test::own_connect(listener.port()))
        << probed.data;
    EXPECT_EQ(probed.data.at("fork"), 0) << probed.data;
    EXPECT_EQ(probed.data.at("spawn"), 0) << probed.data;
    EXPECT_EQ(probed.data.at("write_outside"), 0) << probed.data;
    EXPECT_EQ(probed.data.at("at_load"), 0) << probed.data;
    EXPECT_EQ(probed.data.at("write_temp"), 0) << probed.data;
    EXPECT_EQ(probed.data.at("read"), 0) << probed.data;
    EXPECT_EQ(probed.data.at("link_outside"), 0) << probed.data;
    EXPECT_EQ(probed.data.at("write_through_symlink"), 0) << probed.data;
    EXPECT_EQ(probed.data.at("stderr_is_null"), praenora::test::stderr_is_null()) << probed.data;
    EXPECT_EQ(probed.data.at("open_stderr"), own_open_stderr()) << probed.data;
    EXPECT_EQ(probed.data.at("kept"), -1) << probed.data;
    EXPECT_EQ(text_of(read), "read mex"); // the byte through the symbolic link
    EXPECT_EQ(probed.data.at("temporary_directory"), fs::temp_directory_path().string());
    const char* tmpdir = std::getenv("TMPDIR");
    EXPECT_EQ(probed.data.at("tmpdir"), tmpdir != nullptr ? json(tmpdir) : json(nullptr));
}

// The restricted profile refuses a connection (also to a port this process listens on), a fork, a
// spawn and a write outside the module host's temporary directory, from the library's static
// initializer as from a call, also through a hard link made in the directory to a file outside or
// through a symbolic link there, and opening /dev/stderr by its path (EPERM): the file outside is
// unchanged. It allows writing in that directory, reading, and the channel: the module's record and
// its outputs reach the runtime. The module host's standard error is the runtime's (this process's)
// again once the profile is applied: it is /dev/null only around sandbox_init(). The directory is
// the module host's own, a real path below the system's temporary directory's, which TMPDIR and
// temp_directory_path() name; it is removed once the module host has ended.
TEST(Isolation, TheRestrictedProfileRefusesTheNetworkProcessesAndWritesOutsideItsDirectory) {
    if (const char* why = praenora::test::restricted_profile_skip()) {
        GTEST_SKIP() << why;
    }
    const ScratchDir outside;
    const fs::path read = outside.path() / "read.txt";
    praenora::test::write_text_file(read, "read me");
    const praenora::test::LoopbackListener listener;
    if (listener.listening()) {
        EXPECT_EQ(praenora::test::own_connect(listener.port()), 0);
    }
    Probed probed;
    probe(SandboxProfile::restricted, outside.path(), read, listener.port(), probed);
    ASSERT_TRUE(probed.data.is_object());
    EXPECT_EQ(probed.data.at("connect"), EPERM) << probed.data;
    EXPECT_EQ(probed.data.at("fork"), EPERM) << probed.data;
    EXPECT_EQ(probed.data.at("spawn"), EPERM) << probed.data;
    EXPECT_EQ(probed.data.at("write_outside"), EPERM) << probed.data;
    EXPECT_EQ(probed.data.at("at_load"), EPERM) << probed.data;
    EXPECT_EQ(probed.data.at("write_temp"), 0) << probed.data;
    EXPECT_EQ(probed.data.at("read"), 0) << probed.data;
    EXPECT_EQ(probed.data.at("link_outside"), EPERM) << probed.data;
    EXPECT_EQ(probed.data.at("write_through_symlink"), EPERM) << probed.data;
    EXPECT_EQ(probed.data.at("stderr_is_null"), praenora::test::stderr_is_null()) << probed.data;
    EXPECT_EQ(probed.data.at("open_stderr"), EPERM) << probed.data;
    EXPECT_EQ(text_of(read), "read me");
    EXPECT_TRUE(recorded(probed.logs, "fx-sandbox-probe: probing"));
    EXPECT_TRUE(events_named(probed.logs, "module_host.directory_left").empty());

    const fs::path directory = probed.data.at("temporary_directory").get<std::string>();
    EXPECT_EQ(probed.data.at("tmpdir"), utf8(directory));
    EXPECT_EQ(directory.parent_path(), fs::canonical(fs::temp_directory_path()));
    EXPECT_EQ(directory.filename().string().rfind("praenora-module-host-", 0), 0u) << directory;
    std::error_code ec;
    EXPECT_FALSE(fs::exists(directory, ec)) << directory;
}

// A module host writes to the standard error it inherits, as an instance in the runtime's process
// writes to the runtime's: with this process's descriptor 2 pointed at a scratch file while the
// module host starts, the probe's line lands in that file, not on this process's standard error.
TEST(Isolation, AModuleHostWritesThroughTheStandardErrorItInherits) {
    const ScratchDir outside;
    const fs::path read = outside.path() / "read.txt";
    praenora::test::write_text_file(read, "read me");
    const fs::path standard_error = outside.path() / "standard error.txt";
    const praenora::test::LoopbackListener listener;
    Probed probed;
    probe(SandboxProfile::none, outside.path(), read, listener.port(), probed, false,
          &standard_error);
    ASSERT_TRUE(probed.data.is_object());
    EXPECT_EQ(probed.data.at("write_stderr"), 0) << probed.data;
    EXPECT_NE(text_of(standard_error).find(kStandardErrorLine), std::string::npos)
        << text_of(standard_error);
}

// Under the restricted profile a module host still writes through the descriptor 2 it inherits,
// although that descriptor names a file outside its temporary directory, in a directory where the
// profile refuses it to make a file (EPERM): the profile refuses opening files for writing, not
// writing to descriptors the module host already has.
TEST(Isolation, TheRestrictedProfileLetsAModuleHostWriteThroughTheStandardErrorItInherits) {
    if (const char* why = praenora::test::restricted_profile_skip()) {
        GTEST_SKIP() << why;
    }
    const ScratchDir outside;
    const fs::path read = outside.path() / "read.txt";
    praenora::test::write_text_file(read, "read me");
    const fs::path standard_error = outside.path() / "standard error.txt";
    const praenora::test::LoopbackListener listener;
    Probed probed;
    probe(SandboxProfile::restricted, outside.path(), read, listener.port(), probed, false,
          &standard_error);
    ASSERT_TRUE(probed.data.is_object());
    EXPECT_EQ(probed.data.at("write_outside"), EPERM) << probed.data;
    EXPECT_EQ(probed.data.at("write_stderr"), 0) << probed.data;
    EXPECT_NE(text_of(standard_error).find(kStandardErrorLine), std::string::npos)
        << text_of(standard_error);
}

// The profile names the module host's temporary directory as it is: a system temporary directory
// whose path holds a double quote, a backslash and a character beyond ASCII (escaped in the
// profile's text as its language needs) still gives a module host that may write in its own
// directory and nowhere else.
TEST(Isolation, TheRestrictedProfileNamesADirectoryWithQuotesBackslashesAndNonAscii) {
    if (const char* why = praenora::test::restricted_profile_skip()) {
        GTEST_SKIP() << why;
    }
    const ScratchDir scratch;
    const fs::path system =
        scratch.path() / praenora::test::utf8_path("sys \"q\" back\\slash \xC3\xA9");
    fs::create_directories(system);
    const praenora::test::ScopedEnvironment tmpdir("TMPDIR", utf8(system));
    const fs::path read = scratch.path() / "read.txt";
    praenora::test::write_text_file(read, "read me");
    const praenora::test::LoopbackListener listener;
    Probed probed;
    probe(SandboxProfile::restricted, scratch.path(), read, listener.port(), probed);
    ASSERT_TRUE(probed.data.is_object());
    EXPECT_EQ(probed.data.at("write_temp"), 0) << probed.data;
    EXPECT_EQ(probed.data.at("write_outside"), EPERM) << probed.data;
    EXPECT_EQ(probed.data.at("at_load"), EPERM) << probed.data;
    EXPECT_EQ(probed.data.at("link_outside"), EPERM) << probed.data;
    const fs::path directory =
        praenora::test::utf8_path(probed.data.at("temporary_directory").get<std::string>());
    EXPECT_EQ(directory.parent_path(), fs::canonical(system)) << directory;
    std::error_code ec;
    EXPECT_FALSE(fs::exists(directory, ec)) << directory;
}

#if defined(__APPLE__)
// What a module leaves in its temporary directory that cannot be removed (an immutable file) stays,
// and the runtime says so: a warning (module_host.directory_left) names the directory, so that
// nothing is left in the system's temporary directory unnoticed. The module host's directory is
// made below a scratch directory that TMPDIR names for this test, which the test removes with the
// immutable file however it ends.
TEST(Isolation, ATemporaryDirectoryThatCannotBeRemovedIsReported) {
    if (const char* why = praenora::test::restricted_profile_skip()) {
        GTEST_SKIP() << why;
    }
    const ScratchDir outside;
    const fs::path read = outside.path() / "read.txt";
    praenora::test::write_text_file(read, "read me");
    const ScratchDir system;
    const praenora::test::RemoveAllEvenImmutable cleanup(system.path());
    const praenora::test::ScopedEnvironment tmpdir("TMPDIR", utf8(system.path()));
    const praenora::test::LoopbackListener listener;
    Probed probed;
    probe(SandboxProfile::restricted, outside.path(), read, listener.port(), probed, true);
    ASSERT_TRUE(probed.data.is_object());
    EXPECT_EQ(probed.data.at("kept"), 0) << probed.data;
    const fs::path directory =
        praenora::test::utf8_path(probed.data.at("temporary_directory").get<std::string>());
    EXPECT_EQ(directory.parent_path(), fs::canonical(system.path())) << directory;
    const std::vector<json> left = events_named(probed.logs, "module_host.directory_left");
    ASSERT_EQ(left.size(), 1u);
    EXPECT_EQ(left[0].at("severity_text"), "WARN");
    EXPECT_NE(left[0].at("body").get<std::string>().find(utf8(directory)), std::string::npos)
        << left[0].at("body");
    EXPECT_TRUE(fs::exists(directory / "kept" / "file"));
}
#endif

// A runtime that runs in a sandbox already cannot give a module host the restricted profile
// (macOS applies no second one): the load fails with MODULE_HOST_UNAVAILABLE, details.reason
// "sandbox" and the system's text, located in the module's manifest. Where this test's process is
// not in a sandbox, a helper started in one of its own (sandbox-exec) is that runtime.
TEST(Isolation, ARestrictedModuleHostOfASandboxedRuntimeCannotStart) {
    if (!praenora::test::restricted_profile_available()) {
        GTEST_SKIP() << praenora::test::kNoRestrictedProfileSkip;
    }
    json answer;
    if (praenora::test::in_a_sandbox()) {
        CapturedLogs logs;
        ChildSettings settings = praenora::test::child_settings(logs);
        settings.sandbox = SandboxProfile::restricted;
        try {
            static_cast<void>(LoadedModule::load_in_child(
                praenora::detail::load_manifest(
                    praenora::test::fixture_manifest("fx-sandbox-probe")),
                settings));
            answer = {{"loaded", true}};
        } catch (const Error& error) {
            answer = {{"code", static_cast<int>(error.code())},
                      {"message", error.what()},
                      {"file", error.location().file},
                      {"details", json::parse(error.details_json())}};
        }
    } else {
        std::error_code ec;
        if (!fs::exists(praenora::test::kSandboxExec, ec)) {
            GTEST_SKIP() << praenora::test::kSandboxExec << " is not on this system";
        }
        auto helper = praenora::detail::ChildProcess::start(
            praenora::test::kSandboxExec,
            {"-p", "(version 1)(allow default)", PRAENORA_TEST_RESTRICTED_PROBE});
        const std::string line =
            praenora::test::read_within(helper.channel, std::chrono::seconds(30), true);
        ::close(helper.channel);
        const auto exit = helper.process->wait(std::chrono::seconds(30));
        ASSERT_TRUE(exit.has_value());
        EXPECT_EQ(exit->exit_code, 0);
        ASSERT_FALSE(line.empty()) << "the helper wrote no answer";
        answer = json::parse(line);
    }
    ASSERT_TRUE(answer.contains("code")) << answer;
    EXPECT_EQ(answer.at("code"), static_cast<int>(ErrorCode::MODULE_HOST_UNAVAILABLE)) << answer;
    EXPECT_EQ(answer.at("details").at("reason"), "sandbox") << answer;
    EXPECT_EQ(answer.at("details").at("system"), "Operation not permitted") << answer;
    EXPECT_EQ(answer.at("file"), utf8(praenora::detail::load_manifest(
                                          praenora::test::fixture_manifest("fx-sandbox-probe"))
                                          .manifest_path));
    EXPECT_NE(answer.at("message").get<std::string>().find(
                  "module fx-sandbox-probe@0.1.0: the module host cannot apply the restricted "
                  "sandbox profile: Operation not permitted"),
              std::string::npos)
        << answer;
}

#endif // _WIN32
