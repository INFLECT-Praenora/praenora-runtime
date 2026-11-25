// How the start of the provider python fails when the files it starts from are wrong
// (docs/specs/python-service.md, the provider python's start): each test copies the build tree's
// provider directory into a scratch directory, edits the copy, and has praenora_python_start_probe
// (python_start_probe.cpp) resolve praenora.python@0 and start the provider in a process of its
// own, with the copy first on the provider search path and the build tree's provider directory
// after it, as an installation's provider directory comes after a caller's paths. The copy
// shadows the build tree's provider, so the copy is what starts; its failure is
// LIBRARY_LOAD_FAILED with details.reason "start" and the provider's error document as
// details.cause. Built with PRAENORA_WITH_PYTHON_ENVIRONMENT only.
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <praenora/sdk/status.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>

extern char** environ;
#endif

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::test::ScratchDir;
using praenora::test::utf8_path;

fs::path real_manifest() { return utf8_path(PRAENORA_TEST_PYTHON_PROVIDER_MANIFEST); }
fs::path real_directory() { return real_manifest().parent_path(); }
// The build tree's provider directory, where its runtime library finds its providers.
fs::path real_providers() { return real_directory().parent_path(); }

std::string read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << text;
}

std::string utf8(const fs::path& path) { return path.lexically_normal().u8string(); }

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// Whether `text` names `path`, as given or with its symbolic links resolved (the system's
// temporary directory may be reached through one).
bool names(const std::string& text, const fs::path& path) {
    std::error_code ec;
    const fs::path resolved = fs::weakly_canonical(path, ec);
    return contains(text, path.lexically_normal().u8string()) ||
           (!ec && contains(text, resolved.lexically_normal().u8string()));
}

// Whether the path `text` is `path`, as given or with its symbolic links resolved (the file need
// not exist).
bool same_path(const std::string& text, const fs::path& path) {
    const fs::path given = utf8_path(text);
    if (given.lexically_normal() == path.lexically_normal()) {
        return true;
    }
    std::error_code ec, other_ec;
    const fs::path resolved = fs::weakly_canonical(given, ec);
    const fs::path expected = fs::weakly_canonical(path, other_ec);
    return !ec && !other_ec && resolved.lexically_normal() == expected.lexically_normal();
}

#ifdef _WIN32
// `argument` quoted for CreateProcessW, so that the C runtime of the program splits it back as it
// was (backslashes before a quote are doubled).
std::wstring quoted(const std::wstring& argument) {
    std::wstring text = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        text.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        text += c;
        backslashes = 0;
    }
    text.append(backslashes * 2, L'\\');
    text += L'"';
    return text;
}
#endif

// Runs `program` with `arguments` (UTF-8) in a new process, with this process's environment and
// standard streams, and waits for it: its exit code, or -1 when it did not start or exit.
int run_program(const fs::path& program, const std::vector<std::string>& arguments) {
#ifdef _WIN32
    std::wstring command = quoted(program.wstring());
    for (const std::string& argument : arguments) {
        command += L" " + quoted(utf8_path(argument).wstring());
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &startup, &process)) {
        return -1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    const BOOL got = GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return got ? static_cast<int>(code) : -1;
#else
    std::vector<std::string> storage = {program.string()};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (std::string& text : storage) {
        argv.push_back(text.data());
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawn(&pid, program.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
        return -1;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// A copy of the build tree's provider python, in <root>/providers/python: its library, its
// manifest and its environment file and, with `with_core`, the core and the core's python/
// directory (praenora.sdk) too.
struct ProviderCopy {
    fs::path search_path; // <root>/providers
    fs::path directory;   // <root>/providers/python
    fs::path manifest;
    fs::path environment_file;
    std::string core_name; // the core's file name

    json environment() const { return json::parse(read_file(environment_file)); }
    void set_environment(const json& document) const {
        write_file(environment_file, document.dump(2) + "\n");
    }
};

ProviderCopy copy_provider(const fs::path& root, bool with_core = false) {
    ProviderCopy copy;
    copy.search_path = root / "providers";
    copy.directory = copy.search_path / "python";
    copy.manifest = copy.directory / real_manifest().filename();
    copy.environment_file = copy.directory / "python-environment.json";
    fs::create_directories(copy.directory);
    const json manifest = json::parse(read_file(real_manifest()));
    const fs::path library = utf8_path(manifest.at("library").at("path").get<std::string>());
    fs::copy_file(real_directory() / library, copy.directory / library);
    fs::copy_file(real_manifest(), copy.manifest);
    const fs::path real_environment_file = real_directory() / "python-environment.json";
    fs::copy_file(real_environment_file, copy.environment_file);
    const fs::path core =
        utf8_path(json::parse(read_file(real_environment_file)).at("core").get<std::string>());
    copy.core_name = core.filename().u8string();
    if (with_core) {
        fs::copy_file(core, copy.directory / core.filename());
        fs::copy(real_directory() / "python", copy.directory / "python",
                 fs::copy_options::recursive);
    }
    return copy;
}

// What praenora_python_start_probe wrote, and its exit code.
struct Probe {
    int exit_code = -1;
    json result;

    // The cause of the start's failure: details.cause of LIBRARY_LOAD_FAILED with reason start.
    json cause() const {
        const json& error = result.at("error");
        EXPECT_EQ(error.value("name", ""), "LIBRARY_LOAD_FAILED") << result.dump(2);
        const json& details = error.at("details");
        EXPECT_EQ(details.value("reason", ""), "start") << result.dump(2);
        return details.value("cause", json::object());
    }
};

// Runs the probe with the copy first on the provider search path and the build tree's provider
// directory after it (`load` is a library the probe loads globally first, when not empty).
Probe probe(const ScratchDir& scratch, const ProviderCopy& copy, const fs::path& load = {}) {
    const fs::path result_file = scratch.path() / "probe.json";
    std::error_code ignored;
    fs::remove(result_file, ignored);
    std::vector<std::string> arguments;
    if (!load.empty()) {
        arguments = {"--load", load.u8string()};
    }
    arguments.push_back(result_file.u8string());
    arguments.push_back(copy.search_path.u8string());
    arguments.push_back(real_providers().u8string());
    Probe probe;
    probe.exit_code = run_program(utf8_path(PRAENORA_TEST_PYTHON_START_PROBE), arguments);
    const json result = json::parse(read_file(result_file), nullptr, false);
    probe.result = result.is_discarded() ? json::object() : result;
    // The copy, first on the path, serves the service: it shadows the build tree's provider.
    EXPECT_EQ(utf8_path(probe.result.value("manifest", "")).lexically_normal(),
              copy.manifest.lexically_normal())
        << probe.result.dump(2);
    const json expected_manifests = {
        {{"manifest", utf8(copy.manifest)}, {"shadowed_by", nullptr}},
        {{"manifest", utf8(real_manifest())}, {"shadowed_by", utf8(copy.manifest)}}};
    json manifests = probe.result.value("python_manifests", json::array());
    for (json& entry : manifests) {
        entry["manifest"] = utf8(utf8_path(entry.at("manifest").get<std::string>()));
        if (entry.at("shadowed_by").is_string()) {
            entry["shadowed_by"] = utf8(utf8_path(entry.at("shadowed_by").get<std::string>()));
        }
    }
    EXPECT_EQ(manifests, expected_manifests);
    return probe;
}

} // namespace

// The control: an unedited copy starts, and it is the copy that does.
TEST(PythonEnvironmentFile, UneditedCopyStarts) {
    const ScratchDir scratch;
    const ProviderCopy copy = copy_provider(scratch.path());
    const Probe result = probe(scratch, copy);
    ASSERT_EQ(result.exit_code, 0) << result.result.dump(2);
    EXPECT_EQ(result.result.at("describe").at("environment").at("package"),
              PRAENORA_TEST_PYTHON_ENVIRONMENT_PACKAGE);
}

// No environment file beside the provider's library.
TEST(PythonEnvironmentFile, Missing) {
    const ScratchDir scratch;
    const ProviderCopy copy = copy_provider(scratch.path());
    fs::remove(copy.environment_file);
    const Probe result = probe(scratch, copy);
    EXPECT_EQ(result.exit_code, 1);
    const json cause = result.cause();
    EXPECT_EQ(cause.value("code", 0), PRAENORA_STATUS_NOT_FOUND) << cause.dump(2);
    EXPECT_TRUE(names(cause.value("message", ""), copy.environment_file)) << cause.dump(2);
    EXPECT_TRUE(same_path(cause.at("details").value("file", ""), copy.environment_file))
        << cause.dump(2);
}

// The environment file names a directory that holds no Python environment.
TEST(PythonEnvironmentFile, NamesNoBundle) {
    const ScratchDir scratch;
    const ProviderCopy copy = copy_provider(scratch.path());
    const fs::path empty = scratch.path() / "no environment";
    fs::create_directories(empty);
    json environment = copy.environment();
    environment["environment"] = empty.u8string();
    copy.set_environment(environment);
    const Probe result = probe(scratch, copy);
    EXPECT_EQ(result.exit_code, 1);
    const json cause = result.cause();
    EXPECT_EQ(cause.value("code", 0), PRAENORA_STATUS_NOT_FOUND) << cause.dump(2);
    const std::string message = cause.value("message", "");
    EXPECT_TRUE(names(message, empty / "praenora-runtime-pyenv.json")) << cause.dump(2);
    EXPECT_TRUE(names(message, copy.environment_file)) << cause.dump(2);
}

// The environment's lock is not the one the provider was built for: the refusal names both.
TEST(PythonEnvironmentFile, WrongLock) {
    const ScratchDir scratch;
    const ProviderCopy copy = copy_provider(scratch.path());
    const fs::path other = scratch.path() / "other environment";
    json manifest = json::parse(
        read_file(utf8_path(PRAENORA_TEST_PYTHON_ENVIRONMENT_DIR) / "praenora-runtime-pyenv.json"));
    const std::string wrong(64, 'f');
    manifest["lock"]["sha256"] = wrong;
    write_file(other / "praenora-runtime-pyenv.json", manifest.dump(2));
    json environment = copy.environment();
    environment["environment"] = other.u8string();
    copy.set_environment(environment);
    const Probe result = probe(scratch, copy);
    EXPECT_EQ(result.exit_code, 1);
    const json cause = result.cause();
    EXPECT_EQ(cause.value("code", 0), PRAENORA_STATUS_UNSUPPORTED) << cause.dump(2);
    const std::string message = cause.value("message", "");
    EXPECT_TRUE(contains(message, wrong)) << message;
    EXPECT_TRUE(contains(message, PRAENORA_TEST_PYTHON_ENVIRONMENT_LOCK_SHA256)) << message;
    const json& details = cause.at("details");
    EXPECT_EQ(details.value("lock_sha256", ""), wrong);
    EXPECT_EQ(details.value("expected_lock_sha256", ""),
              PRAENORA_TEST_PYTHON_ENVIRONMENT_LOCK_SHA256);
    EXPECT_EQ(details.value("expected_package", ""), PRAENORA_TEST_PYTHON_ENVIRONMENT_PACKAGE);
}

// The environment file names a core that is not there (relative to itself, as an installation's
// file does).
TEST(PythonEnvironmentFile, MissingCore) {
    const ScratchDir scratch;
    const ProviderCopy copy = copy_provider(scratch.path());
    json environment = copy.environment();
    environment["core"] = copy.core_name;
    copy.set_environment(environment);
    const Probe result = probe(scratch, copy);
    EXPECT_EQ(result.exit_code, 1);
    const json cause = result.cause();
    EXPECT_EQ(cause.value("code", 0), PRAENORA_STATUS_NOT_FOUND) << cause.dump(2);
    const fs::path core = copy.directory / utf8_path(copy.core_name);
    EXPECT_TRUE(names(cause.value("message", ""), core)) << cause.dump(2);
    EXPECT_EQ(utf8_path(cause.at("details").value("core", "")).filename().u8string(),
              copy.core_name)
        << cause.dump(2);
}

// A core of another build: the provider refuses it before it starts it, naming both identities.
TEST(PythonEnvironmentFile, CoreBuildIdDiffers) {
    const ScratchDir scratch;
    const ProviderCopy copy = copy_provider(scratch.path());
    fs::copy_file(utf8_path(PRAENORA_TEST_FAKE_PYTHON_CORE),
                  copy.directory / utf8_path(copy.core_name));
    json environment = copy.environment();
    environment["core"] = copy.core_name;
    copy.set_environment(environment);
    const Probe result = probe(scratch, copy);
    EXPECT_EQ(result.exit_code, 1);
    const json cause = result.cause();
    EXPECT_EQ(cause.value("code", 0), PRAENORA_STATUS_UNSUPPORTED) << cause.dump(2);
    const std::string message = cause.value("message", "");
    const std::string fake = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    EXPECT_TRUE(contains(message, fake)) << message;
    EXPECT_TRUE(contains(message, PRAENORA_TEST_PYTHON_CORE_BUILD_ID)) << message;
    EXPECT_EQ(cause.at("details").value("build_id", ""), fake);
    EXPECT_EQ(cause.at("details").value("expected_build_id", ""),
              PRAENORA_TEST_PYTHON_CORE_BUILD_ID);
}

// praenora.sdk beside a copy of the core was edited: the core's first start refuses it, naming the
// directory and both digests.
TEST(PythonEnvironmentFile, SdkEditedFailsStart) {
    const ScratchDir scratch;
    const ProviderCopy copy = copy_provider(scratch.path(), true);
    const fs::path sdk = copy.directory / "python" / "praenora" / "sdk";
    {
        std::ofstream out(sdk / "__init__.py", std::ios::binary | std::ios::app);
        out << "\n# edited after the build\n";
    }
    json environment = copy.environment();
    environment["core"] = copy.core_name;
    copy.set_environment(environment);
    const Probe result = probe(scratch, copy);
    EXPECT_EQ(result.exit_code, 1);
    const json cause = result.cause();
    EXPECT_EQ(cause.value("code", 0), PRAENORA_STATUS_DATA_INVALID) << cause.dump(2);
    EXPECT_TRUE(names(cause.value("message", ""), sdk)) << cause.dump(2);
    const json& details = cause.at("details");
    EXPECT_TRUE(same_path(details.value("path", ""), sdk)) << cause.dump(2);
    EXPECT_EQ(details.value("expected_sha256", ""), PRAENORA_TEST_PYTHON_SDK_DIGEST);
    EXPECT_NE(details.value("actual_sha256", ""), PRAENORA_TEST_PYTHON_SDK_DIGEST);
}

// The directory of praenora.sdk beside a copy of the core comes first on the import path: with
// __pycache__ directories in it the core starts, and with a file beside praenora/sdk (a json.py
// that would hide the standard library's json) its first start refuses it, naming the file.
TEST(PythonEnvironmentFile, StrayFileBesideSdkFailsStart) {
    const ScratchDir scratch;
    const ProviderCopy copy = copy_provider(scratch.path(), true);
    const fs::path python = copy.directory / "python";
    write_file(python / "__pycache__" / "json.cpython-312.pyc", "bytecode");
    write_file(python / "praenora" / "__pycache__" / "x.cpython-312.pyc", "bytecode");
    json environment = copy.environment();
    environment["core"] = copy.core_name;
    copy.set_environment(environment);
    const Probe started = probe(scratch, copy);
    ASSERT_EQ(started.exit_code, 0) << started.result.dump(2);

    for (const fs::path& stray : {python / "json.py", python / "praenora" / "other.py"}) {
        SCOPED_TRACE(utf8(stray));
        write_file(stray, "raise SystemExit('imported')\n");
        const Probe result = probe(scratch, copy);
        EXPECT_EQ(result.exit_code, 1);
        const json cause = result.cause();
        EXPECT_EQ(cause.value("code", 0), PRAENORA_STATUS_DATA_INVALID) << cause.dump(2);
        EXPECT_TRUE(names(cause.value("message", ""), stray)) << cause.dump(2);
        EXPECT_TRUE(same_path(cause.at("details").value("path", ""), stray)) << cause.dump(2);
        fs::remove(stray);
    }
}

// A process that has another Python interpreter's library loaded (here a copy of the
// environment's own libpython, in another directory) cannot start the provider: it says to run
// the step in a module host.
TEST(PythonProviderStart, AnotherInterpreterRefused) {
    const ScratchDir scratch;
    const ProviderCopy copy = copy_provider(scratch.path());
    const fs::path library = utf8_path(PRAENORA_TEST_PYTHON_LIBRARY);
    const fs::path other = scratch.path() / "other python" / library.filename();
    fs::create_directories(other.parent_path());
    fs::copy_file(library, other);
#ifdef _WIN32
    // The DLLs beside python312.dll (the C runtime's among them), which it loads from its own
    // directory.
    for (const fs::directory_entry& entry : fs::directory_iterator(library.parent_path())) {
        if (entry.is_regular_file() && entry.path().extension() == ".dll" &&
            entry.path().filename() != library.filename()) {
            fs::copy_file(entry.path(), other.parent_path() / entry.path().filename());
        }
    }
#endif
    const Probe result = probe(scratch, copy, other);
    EXPECT_EQ(result.exit_code, 1);
    const json cause = result.cause();
    EXPECT_EQ(cause.value("code", 0), PRAENORA_STATUS_UNSUPPORTED) << cause.dump(2);
    const std::string message = cause.value("message", "");
    EXPECT_TRUE(contains(message, "module host")) << message;
    EXPECT_TRUE(contains(message, "isolation mode process")) << message;
    EXPECT_TRUE(same_path(cause.at("details").value("interpreter", ""), other)) << cause.dump(2);
}
