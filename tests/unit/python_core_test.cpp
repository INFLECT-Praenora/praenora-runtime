// White-box tests of the runtime's Python core (src/providers/python/core): the interpreter it
// starts with the environment the build uses, the objects it creates and calls, the documents and
// error documents it writes, what Python prints and logs, and child processes
// (docs/specs/python-service.md). The tests link the core and call its C interface
// (python_core.h). The core is started once per process, by the first test that needs it, with
// the environment of the build and the build's copy of praenora.sdk beside the core; the fixture
// classes are in tests/fixtures/python-core.
#include "python_core.h"

#include "digest.hpp"

#include "scoped_environment.hpp"
#include "temp_dir.hpp"
#include "test_support.hpp"

#include <praenora/sdk/abi.h>
#include <praenora/sdk/python.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---------------------------------------------------------------------------------------------
// Hosts, sinks and objects.

struct Record {
    std::int32_t level;
    std::string message;
};

// The records a host was given, kept for the test to read. Thread-safe.
class RecordLog {
public:
    void add(std::int32_t level, const char* data, std::size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.push_back({level, std::string(data, size)});
    }
    std::vector<Record> records() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_;
    }
    // The records whose message holds `text`.
    std::vector<Record> containing(std::string_view text) const {
        std::vector<Record> found;
        for (const Record& record : records()) {
            if (record.message.find(text) != std::string::npos) {
                found.push_back(record);
            }
        }
        return found;
    }
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<Record> records_;
};

// A provider host whose records go to a RecordLog. A provider host stays valid for the rest of the
// process once start() was given it, so tests allocate these and never free them.
struct ProviderHost {
    RecordLog log;
    praenora_provider_host host{};

    ProviderHost() {
        host.struct_size = static_cast<std::uint32_t>(sizeof host);
        host.reserved = 0;
        host.context = this;
        host.log = &ProviderHost::log_fn;
        host.log_enabled = &ProviderHost::enabled_fn;
        host.load_compiled = nullptr;
        host.save_compiled = nullptr;
    }

    static void PRAENORA_CALL log_fn(void* context, std::int32_t level, const char* data,
                                     std::size_t size) {
        static_cast<ProviderHost*>(context)->log.add(level, data, size);
    }
    static std::int32_t PRAENORA_CALL enabled_fn(void*, std::int32_t) { return 1; }
};

// A module's host API: its records, and a cancel flag that rises after `cancel_after` polls.
struct ModuleHost {
    RecordLog log;
    std::atomic<int> polls{0};
    int cancel_after = -1; // never
    praenora_host_api api{};

    ModuleHost() {
        api.struct_size = static_cast<std::uint32_t>(sizeof api);
        api.abi_major = PRAENORA_ABI_MAJOR;
        api.abi_minor = PRAENORA_ABI_MINOR;
        api.context = this;
        api.log = &ModuleHost::log_fn;
        api.log_enabled = &ModuleHost::enabled_fn;
        api.cancel_requested = &ModuleHost::cancel_fn;
        api.get_service = &ModuleHost::service_fn;
    }

    static void PRAENORA_CALL log_fn(void* context, std::int32_t level, const char* data,
                                     std::size_t size) {
        static_cast<ModuleHost*>(context)->log.add(level, data, size);
    }
    static std::int32_t PRAENORA_CALL enabled_fn(void*, std::int32_t) { return 1; }
    static std::int32_t PRAENORA_CALL cancel_fn(void* context) {
        auto* self = static_cast<ModuleHost*>(context);
        const int polls = ++self->polls;
        return self->cancel_after >= 0 && polls > self->cancel_after ? 1 : 0;
    }
    static const void* PRAENORA_CALL service_fn(void*, const char*, std::size_t, std::uint32_t) {
        return nullptr;
    }
};

// A sink that keeps what it is given and counts the writes.
struct Capture {
    std::string bytes;
    int writes = 0;
    praenora_sink sink{this, &Capture::write_fn};

    Capture() = default;
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    static praenora_status PRAENORA_CALL write_fn(void* context, const char* data,
                                                  std::size_t size) {
        auto* self = static_cast<Capture*>(context);
        self->bytes.append(data, size);
        self->writes += 1;
        return PRAENORA_STATUS_OK;
    }
};

praenora_bytes bytes_of(std::string_view text) { return {text.data(), text.size()}; }

const praenora_python_core_v0& core() { return *praenora_python_core_get_api(); }

fs::path environment_dir() {
    return praenora::test::utf8_path(PRAENORA_TEST_PYTHON_ENVIRONMENT_DIR);
}
// The directory that holds praenora.sdk (praenora/sdk) and nothing else, as the core requires:
// the build's copy beside the core.
fs::path sdk_dir() { return praenora::test::utf8_path(PRAENORA_TEST_PYTHON_SDK_DIR); }
fs::path fixtures_dir() { return fs::path(PRAENORA_TEST_FIXTURES_DIR) / "python-core"; }

std::string utf8_of(const fs::path& path) { return path.u8string(); }

// The provider host of the process's first start: records outside calls go to it.
ProviderHost& process_host() {
    static ProviderHost* host = new ProviderHost();
    return *host;
}

struct StartResult {
    praenora_status status;
    std::string error;
};

StartResult start_core(ProviderHost& host, const fs::path& environment, const fs::path& sdk) {
    Capture error;
    const std::string environment_text = utf8_of(environment);
    const std::string sdk_text = utf8_of(sdk);
    const praenora_status status =
        core().start(&host.host, bytes_of(environment_text), bytes_of(sdk_text), &error.sink);
    return {status, error.bytes};
}

// The core started with the build's environment, once per process.
const StartResult& started() {
    static const StartResult result = start_core(process_host(), environment_dir(), sdk_dir());
    return result;
}

#define ASSERT_STARTED()                                                                           \
    ASSERT_EQ(started().status, PRAENORA_STATUS_OK) << "the core did not start: " << started().error

json parse_or_null(const std::string& text) {
    return text.empty() ? json() : json::parse(text, nullptr, false);
}

// An object created by the core, released when it goes out of scope.
class Object {
public:
    explicit Object(std::string_view name, std::string_view args = "null",
                    const praenora_host_api* host = nullptr, const fs::path& path = fixtures_dir(),
                    std::string_view path_sha256 = "") {
        Capture error;
        const std::string path_text = path.empty() ? std::string() : utf8_of(path);
        status_ = core().create_object(&process_host().host, host, bytes_of(path_text),
                                       bytes_of(path_sha256), bytes_of(name), bytes_of(args),
                                       &object_, &error.sink);
        error_text_ = error.bytes;
        error_ = parse_or_null(error.bytes);
    }
    ~Object() {
        if (object_ != nullptr) {
            core().release_object(object_);
        }
    }
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

    praenora_status status() const { return status_; }
    const json& error() const { return error_; }
    const std::string& error_text() const { return error_text_; }
    praenora_python_object* get() const { return object_; }

private:
    praenora_status status_ = PRAENORA_STATUS_INTERNAL;
    praenora_python_object* object_ = nullptr;
    std::string error_text_;
    json error_;
};

struct CallResult {
    praenora_status status;
    std::string result;
    int result_writes;
    std::string error_text;
    json error;
};

CallResult call(const Object& object, std::string_view method, std::string_view args = "null",
                const praenora_host_api* host = nullptr) {
    Capture result;
    Capture error;
    const praenora_status status = core().call(object.get(), host, bytes_of(method), bytes_of(args),
                                               &result.sink, &error.sink);
    return {status, result.bytes, result.writes, error.bytes, parse_or_null(error.bytes)};
}

#define ASSERT_CREATED(object)                                                                     \
    ASSERT_EQ((object).status(), PRAENORA_STATUS_OK) << (object).error_text()

// ---------------------------------------------------------------------------------------------
// Text and descriptors.

bool valid_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto byte = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        std::uint32_t code = 0;
        if (byte < 0x80) {
            ++i;
            continue;
        } else if ((byte & 0xE0) == 0xC0) {
            length = 2;
            code = byte & 0x1F;
        } else if ((byte & 0xF0) == 0xE0) {
            length = 3;
            code = byte & 0x0F;
        } else if ((byte & 0xF8) == 0xF0) {
            length = 4;
            code = byte & 0x07;
        } else {
            return false;
        }
        if (i + length > text.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            code = (code << 6) | (next & 0x3F);
        }
        const std::uint32_t smallest[] = {0, 0, 0x80, 0x800, 0x10000};
        if (code < smallest[length] || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        i += length;
    }
    return true;
}

constexpr std::string_view kReplacement = "\xEF\xBF\xBD"; // U+FFFD
constexpr std::string_view kEAcute = "\xC3\xA9";          // U+00E9

// Points descriptors 1 and 2 at pipes of their own until finish(), which gives back what was
// written to each. Nothing may print through them meanwhile: the test makes no assertion before
// finish().
class CapturedDescriptors {
public:
    CapturedDescriptors() {
        std::fflush(stdout);
        std::fflush(stderr);
        for (int i = 0; i < 2; ++i) {
            const int fd = i + 1;
            int ends[2] = {-1, -1};
#ifdef _WIN32
            ok_ = ok_ && _pipe(ends, 1 << 20, _O_BINARY) == 0;
            saved_[i] = _dup(fd);
            ok_ = ok_ && saved_[i] >= 0 && _dup2(ends[1], fd) == 0;
            _close(ends[1]);
#else
            ok_ = ok_ && ::pipe(ends) == 0;
            saved_[i] = ::dup(fd);
            ok_ = ok_ && saved_[i] >= 0 && ::dup2(ends[1], fd) >= 0;
            ::close(ends[1]);
            // Children may keep the write end open after the test (multiprocessing's resource
            // tracker does): the reads must not wait for an end of file.
            ::fcntl(ends[0], F_SETFL, ::fcntl(ends[0], F_GETFL) | O_NONBLOCK);
#endif
            read_[i] = ends[0];
        }
    }
    ~CapturedDescriptors() { (void)finish(); }
    CapturedDescriptors(const CapturedDescriptors&) = delete;
    CapturedDescriptors& operator=(const CapturedDescriptors&) = delete;

    bool ok() const { return ok_; }

    // What descriptors 1 and 2 received; the descriptors are given back first.
    std::pair<std::string, std::string> finish() {
        if (finished_) {
            return {};
        }
        finished_ = true;
        std::fflush(stdout);
        std::fflush(stderr);
        std::string text[2];
        for (int i = 0; i < 2; ++i) {
#ifdef _WIN32
            _dup2(saved_[i], i + 1);
            _close(saved_[i]);
            const HANDLE pipe = reinterpret_cast<HANDLE>(_get_osfhandle(read_[i]));
            DWORD available = 0;
            while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
                char buffer[4096];
                const int count = _read(read_[i], buffer, sizeof buffer);
                if (count <= 0) {
                    break;
                }
                text[i].append(buffer, static_cast<std::size_t>(count));
            }
            _close(read_[i]);
#else
            ::dup2(saved_[i], i + 1);
            ::close(saved_[i]);
            char buffer[4096];
            ssize_t count = 0;
            while ((count = ::read(read_[i], buffer, sizeof buffer)) > 0) {
                text[i].append(buffer, static_cast<std::size_t>(count));
            }
            ::close(read_[i]);
#endif
        }
        return {text[0], text[1]};
    }

private:
    bool ok_ = true;
    bool finished_ = false;
    int saved_[2] = {-1, -1};
    int read_[2] = {-1, -1};
};

// A package of its own below `root`, with a module `mod` that defines `Thing`, so that tests
// running in one process never import the same package from two directories.
std::string write_package(const fs::path& root, const std::string& marker = "first") {
    static std::atomic<int> counter{0};
    const std::string package = "core_test_package_" + std::to_string(++counter);
    fs::create_directories(root / package);
    std::ofstream(root / package / "__init__.py", std::ios::binary) << "";
    std::ofstream(root / package / "mod.py", std::ios::binary)
        << "class Thing:\n"
           "    def __init__(self, document):\n"
           "        self.document = document\n"
           "\n"
           "    def marker(self, document):\n"
           "        return \""
        << marker << "\"\n";
    return package;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Starting and describing.

TEST(PythonCore, StartTwiceIsAccepted) {
    ASSERT_STARTED();
    static ProviderHost* again = new ProviderHost();
    const StartResult second = start_core(*again, environment_dir(), sdk_dir());
    EXPECT_EQ(second.status, PRAENORA_STATUS_OK) << second.error;
    EXPECT_TRUE(second.error.empty()) << second.error;
}

TEST(PythonCore, StartWithAnotherEnvironmentFails) {
    ASSERT_STARTED();
    std::ifstream in(environment_dir() / "praenora-runtime-pyenv.json", std::ios::binary);
    json manifest = json::parse(in);
    const std::string lock = manifest["lock"]["sha256"].get<std::string>();
    const std::string other(64, lock[0] == '1' ? '2' : '1');
    manifest["lock"]["sha256"] = other;
    praenora::test::ScratchDir scratch;
    std::ofstream(scratch.path() / "praenora-runtime-pyenv.json", std::ios::binary)
        << manifest.dump();

    static ProviderHost* host = new ProviderHost();
    const StartResult result = start_core(*host, scratch.path(), sdk_dir());
    EXPECT_EQ(result.status, PRAENORA_STATUS_UNSUPPORTED) << result.error;
    const json error = parse_or_null(result.error);
    ASSERT_TRUE(error.is_object()) << result.error;
    EXPECT_EQ(error.value("code", 0), PRAENORA_STATUS_UNSUPPORTED);
    const std::string message = error.value("message", "");
    EXPECT_NE(message.find(lock), std::string::npos) << message;
    EXPECT_NE(message.find(other), std::string::npos) << message;
}

TEST(PythonCore, ChangedSdkFailsTheFirstStart) {
    Capture probe;
    if (core().describe(&probe.sink) != PRAENORA_STATUS_INVALID_STATE) {
        GTEST_SKIP() << "the core of this process is started already; CTest runs each test in a "
                        "process of its own";
    }
    // A copy of praenora.sdk with one file changed.
    praenora::test::ScratchDir scratch;
    const fs::path copy = scratch.path() / "python";
    fs::create_directories(copy / "praenora");
    fs::copy(sdk_dir() / "praenora" / "sdk", copy / "praenora" / "sdk",
             fs::copy_options::recursive);
    fs::remove_all(copy / "praenora" / "sdk" / "__pycache__");
    std::ofstream(copy / "praenora" / "sdk" / "_runtime.py", std::ios::binary | std::ios::app)
        << "# changed\n";

    static ProviderHost* host = new ProviderHost();
    const StartResult result = start_core(*host, environment_dir(), copy);
    EXPECT_EQ(result.status, PRAENORA_STATUS_DATA_INVALID) << result.error;
    const json error = parse_or_null(result.error);
    EXPECT_EQ(error["details"]["path"], utf8_of(copy / "praenora" / "sdk")) << result.error;
    EXPECT_NE(error.value("message", "").find("differ"), std::string::npos) << result.error;
    Capture still;
    EXPECT_EQ(core().describe(&still.sink), PRAENORA_STATUS_INVALID_STATE) << "not started";

    // The refusal comes before the interpreter starts: the process can start it afterwards.
    ASSERT_STARTED();
}

TEST(PythonCore, DescribeNamesEnvironment) {
    ASSERT_STARTED();
    Capture out;
    ASSERT_EQ(core().describe(&out.sink), PRAENORA_STATUS_OK);
    const json description = json::parse(out.bytes);
    EXPECT_EQ(description["python"]["implementation"], "cpython");
    EXPECT_EQ(description["python"]["version"], PRAENORA_TEST_PYTHON_VERSION);
    EXPECT_EQ(description["python"]["version"], "3.12.16");
    EXPECT_FALSE(description["python"]["abi"].get<std::string>().empty());
    EXPECT_EQ(description["environment"]["name"], "praenora-runtime-pyenv");
    EXPECT_EQ(description["environment"]["version"], PRAENORA_TEST_PYTHON_ENVIRONMENT_VERSION);
    EXPECT_EQ(description["environment"]["package"], PRAENORA_TEST_PYTHON_ENVIRONMENT_PACKAGE);
    EXPECT_EQ(description["environment"]["lock_sha256"],
              PRAENORA_TEST_PYTHON_ENVIRONMENT_LOCK_SHA256);
    // The same bytes every time: canonical JSON.
    Capture again;
    ASSERT_EQ(core().describe(&again.sink), PRAENORA_STATUS_OK);
    EXPECT_EQ(again.bytes, out.bytes);
    EXPECT_EQ(out.bytes, description.dump());
}

TEST(PythonCore, BuildIdIsTheConfiguredValue) {
    Capture out;
    ASSERT_EQ(core().build_id(&out.sink), PRAENORA_STATUS_OK);
    EXPECT_EQ(out.bytes, PRAENORA_TEST_PYTHON_CORE_BUILD_ID);
    EXPECT_EQ(out.bytes.size(), 64u);
    EXPECT_EQ(core().header.abi_major, PRAENORA_PYTHON_CORE_MAJOR);
    EXPECT_EQ(core().header.reserved, 0u);
    EXPECT_GE(core().header.struct_size, sizeof(praenora_python_core_v0));
}

TEST(PythonCore, WarnsAboutUnsetVariables) {
    ASSERT_STARTED();
    const auto hash_seed_warnings = [](const ProviderHost& host) {
        std::size_t count = 0;
        for (const Record& record : host.log.containing("PYTHONHASHSEED")) {
            count += record.level == PRAENORA_LOG_WARN ? 1 : 0;
        }
        return count;
    };
    {
        praenora::test::ScopedEnvironment unset("PYTHONHASHSEED", std::nullopt);
        static ProviderHost* host = new ProviderHost();
        const StartResult result = start_core(*host, environment_dir(), sdk_dir());
        ASSERT_EQ(result.status, PRAENORA_STATUS_OK) << result.error;
        EXPECT_EQ(hash_seed_warnings(*host), 1u);
    }
    {
        praenora::test::ScopedEnvironment zero("PYTHONHASHSEED", std::string("0"));
        static ProviderHost* host = new ProviderHost();
        const StartResult result = start_core(*host, environment_dir(), sdk_dir());
        ASSERT_EQ(result.status, PRAENORA_STATUS_OK) << result.error;
        EXPECT_EQ(hash_seed_warnings(*host), 0u);
    }
    {
        praenora::test::ScopedEnvironment other("PYTHONHASHSEED", std::string("7"));
        static ProviderHost* host = new ProviderHost();
        const StartResult result = start_core(*host, environment_dir(), sdk_dir());
        ASSERT_EQ(result.status, PRAENORA_STATUS_OK) << result.error;
        EXPECT_EQ(hash_seed_warnings(*host), 1u) << "a value other than 0";
    }
}

// ---------------------------------------------------------------------------------------------
// Objects and calls.

TEST(PythonCore, CreateAndCallEcho) {
    ASSERT_STARTED();
    Object echo("core_fixtures.echo:Echo", R"({"created":[1,2.5,"t\u00e9xt",null,true]})");
    ASSERT_CREATED(echo);
    ASSERT_NE(echo.get(), nullptr);

    const CallResult created = call(echo, "created_with");
    ASSERT_EQ(created.status, PRAENORA_STATUS_OK) << created.error_text;
    EXPECT_EQ(json::parse(created.result),
              json::parse(R"({"created":[1,2.5,"t\u00e9xt",null,true]})"));

    const std::string args = R"({"a":{"b":[1,2,3]},"text":"caf)" + std::string(kEAcute) + "\"}";
    const CallResult echoed = call(echo, "echo", args);
    ASSERT_EQ(echoed.status, PRAENORA_STATUS_OK) << echoed.error_text;
    EXPECT_EQ(json::parse(echoed.result), json::parse(args));
    EXPECT_NE(echoed.result.find(kEAcute), std::string::npos) << "not escaped: " << echoed.result;
    EXPECT_TRUE(echoed.error_text.empty());

    const CallResult nothing = call(echo, "nothing");
    ASSERT_EQ(nothing.status, PRAENORA_STATUS_OK) << nothing.error_text;
    EXPECT_EQ(nothing.result, "null");

    // Empty argument bytes are null.
    const CallResult empty = call(echo, "echo", "");
    ASSERT_EQ(empty.status, PRAENORA_STATUS_OK) << empty.error_text;
    EXPECT_EQ(empty.result, "null");
}

TEST(PythonCore, ArgumentsMustBeJson) {
    ASSERT_STARTED();
    Object echo("core_fixtures.echo:Echo");
    ASSERT_CREATED(echo);
    for (const char* args : {"{", "NaN", "[Infinity]", "\xff"}) {
        const CallResult result = call(echo, "echo", args);
        EXPECT_EQ(result.status, PRAENORA_STATUS_INVALID_ARGUMENT) << args;
        EXPECT_EQ(result.error.value("code", 0), PRAENORA_STATUS_INVALID_ARGUMENT) << args;
        EXPECT_TRUE(result.result.empty()) << args;
    }
    Object refused("core_fixtures.echo:Echo", "{");
    EXPECT_EQ(refused.status(), PRAENORA_STATUS_INVALID_ARGUMENT) << refused.error_text();
    EXPECT_EQ(refused.get(), nullptr);
}

TEST(PythonCore, MissingMethodIsUnsupported) {
    ASSERT_STARTED();
    Object echo("core_fixtures.echo:Echo");
    ASSERT_CREATED(echo);
    for (const char* method : {"no_such_method", "not_a_method"}) {
        const CallResult result = call(echo, method);
        EXPECT_EQ(result.status, PRAENORA_STATUS_UNSUPPORTED) << method;
        EXPECT_EQ(result.error.value("code", 0), PRAENORA_STATUS_UNSUPPORTED) << method;
        EXPECT_NE(result.error.value("message", "").find(method), std::string::npos)
            << result.error_text;
    }
}

TEST(PythonCore, ImportErrorIsInternal) {
    ASSERT_STARTED();
    for (const char* name : {"core_fixtures.no_such_module:Echo", "core_fixtures.echo:NoSuchClass",
                             "core_fixtures.raising:FailsToConstruct"}) {
        Object object(name);
        EXPECT_EQ(object.status(), PRAENORA_STATUS_INTERNAL) << name;
        EXPECT_EQ(object.get(), nullptr) << name;
        ASSERT_TRUE(object.error()["details"]["traceback"].is_array()) << object.error_text();
        EXPECT_FALSE(object.error()["details"]["traceback"].empty()) << object.error_text();
    }
    Object unnamed("core_fixtures.echo.Echo");
    EXPECT_EQ(unnamed.status(), PRAENORA_STATUS_INVALID_ARGUMENT) << unnamed.error_text();
}

TEST(PythonCore, ExceptionHasTraceback) {
    ASSERT_STARTED();
    Object raising("core_fixtures.raising:Raising");
    ASSERT_CREATED(raising);
    const CallResult result = call(raising, "value_error", "3");
    EXPECT_EQ(result.status, PRAENORA_STATUS_INTERNAL);
    EXPECT_EQ(result.error.value("code", 0), PRAENORA_STATUS_INTERNAL);
    EXPECT_EQ(result.error.value("message", ""), "ValueError: the value is 3");
    const json& traceback = result.error["details"]["traceback"];
    ASSERT_TRUE(traceback.is_array()) << result.error_text;
    ASSERT_FALSE(traceback.empty());
    for (const json& line : traceback) {
        ASSERT_TRUE(line.is_string());
        EXPECT_EQ(line.get<std::string>().find('\n'), std::string::npos) << "one line each";
    }
    EXPECT_EQ(traceback.back(), "ValueError: the value is 3");
    EXPECT_NE(result.error_text.find("raising.py"), std::string::npos) << result.error_text;
    EXPECT_TRUE(result.result.empty());
}

TEST(PythonCore, SdkErrorKeepsCode) {
    ASSERT_STARTED();
    Object raising("core_fixtures.raising:Raising");
    ASSERT_CREATED(raising);
    const CallResult result = call(raising, "sdk_error");
    EXPECT_EQ(result.status, PRAENORA_STATUS_DATA_INVALID);
    EXPECT_EQ(result.error, json::parse(R"({"code":12,"message":"x","details":{"k":1}})"))
        << result.error_text;
}

TEST(PythonCore, CancelledMapsToCancelled) {
    ASSERT_STARTED();
    Object raising("core_fixtures.raising:Raising");
    ASSERT_CREATED(raising);
    const CallResult result = call(raising, "cancelled");
    EXPECT_EQ(result.status, PRAENORA_STATUS_CANCELLED);
    EXPECT_EQ(result.error.value("code", 0), PRAENORA_STATUS_CANCELLED);
    EXPECT_EQ(result.error.value("message", ""), "cancelled");
}

TEST(PythonCore, MemoryErrorIsResourceExhausted) {
    ASSERT_STARTED();
    Object raising("core_fixtures.raising:Raising");
    ASSERT_CREATED(raising);
    const CallResult result = call(raising, "memory_error", "\"x\"");
    EXPECT_EQ(result.status, PRAENORA_STATUS_RESOURCE_EXHAUSTED);
    EXPECT_EQ(result.error.value("code", 0), PRAENORA_STATUS_RESOURCE_EXHAUSTED);
    EXPECT_EQ(result.error.value("message", ""), "MemoryError: no room for x");
}

TEST(PythonCore, NanResultIsInvalidArgument) {
    ASSERT_STARTED();
    Object results("core_fixtures.results:Results");
    ASSERT_CREATED(results);
    const CallResult result = call(results, "nan");
    EXPECT_EQ(result.status, PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(result.error.value("code", 0), PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_NE(result.error.value("message", "").find("nan"), std::string::npos)
        << "names the method: " << result.error_text;
    EXPECT_TRUE(result.result.empty());
}

TEST(PythonCore, UnserializableResultIsInvalidArgument) {
    ASSERT_STARTED();
    Object results("core_fixtures.results:Results");
    ASSERT_CREATED(results);
    const CallResult result = call(results, "unserializable");
    EXPECT_EQ(result.status, PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_NE(result.error.value("message", "").find("unserializable"), std::string::npos)
        << "names the method: " << result.error_text;
    EXPECT_TRUE(result.result.empty());
}

TEST(PythonCore, LargeResultIsDelivered) {
    ASSERT_STARTED();
    Object results("core_fixtures.results:Results");
    ASSERT_CREATED(results);
    constexpr int kCount = 1500000;
    std::string expected = "[";
    for (int i = 0; i < kCount; ++i) {
        if (i > 0) {
            expected += ',';
        }
        expected += std::to_string(i);
    }
    expected += ']';
    ASSERT_GT(expected.size(), std::size_t{10} * 1024 * 1024);

    const CallResult result =
        call(results, "numbers", "{\"count\":" + std::to_string(kCount) + "}");
    ASSERT_EQ(result.status, PRAENORA_STATUS_OK) << result.error_text;
    EXPECT_GT(result.result_writes, 1) << "the result is written in chunks";
    EXPECT_EQ(result.result.size(), expected.size());
    EXPECT_TRUE(result.result == expected) << "the bytes differ";
}

TEST(PythonCore, TooLargeResultIsResourceExhausted) {
    ASSERT_STARTED();
    Object results("core_fixtures.results:Results");
    ASSERT_CREATED(results);
    const CallResult result =
        call(results, "text", "{\"count\":" + std::to_string(17 * 1024 * 1024) + "}");
    EXPECT_EQ(result.status, PRAENORA_STATUS_RESOURCE_EXHAUSTED);
    EXPECT_EQ(result.error.value("code", 0), PRAENORA_STATUS_RESOURCE_EXHAUSTED);
    EXPECT_NE(result.error.value("message", "").find("16 MiB"), std::string::npos)
        << result.error_text;
    EXPECT_EQ(result.result_writes, 0) << "refused before anything is written";

    // Arguments over the limit are refused before they are parsed.
    std::string large = "\"" + std::string(PRAENORA_PYTHON_MAX_DOCUMENT_SIZE, 'x') + "\"";
    Object echo("core_fixtures.echo:Echo");
    ASSERT_CREATED(echo);
    const CallResult refused = call(echo, "echo", large);
    EXPECT_EQ(refused.status, PRAENORA_STATUS_RESOURCE_EXHAUSTED);
    EXPECT_NE(refused.error.value("message", "").find("16 MiB"), std::string::npos)
        << refused.error_text;
}

// ---------------------------------------------------------------------------------------------
// Output and logging.

TEST(PythonCore, NonUtf8TextIsReplaced) {
    ASSERT_STARTED();
    ModuleHost host;
    Object output("core_fixtures.output:Output", "null", &host.api);
    ASSERT_CREATED(output);
    const CallResult result = call(output, "text_that_is_not_utf8");
    EXPECT_EQ(result.status, PRAENORA_STATUS_INTERNAL);
    ASSERT_TRUE(valid_utf8(result.error_text)) << "the error document is valid UTF-8";
    const std::string message = result.error.value("message", "");
    EXPECT_EQ(message, "ValueError: caf" + std::string(kEAcute) + " " + std::string(kReplacement));

    const std::vector<Record> records = host.log.records();
    ASSERT_FALSE(records.empty());
    for (const Record& record : records) {
        EXPECT_TRUE(valid_utf8(record.message)) << "a record is not valid UTF-8";
    }
    const std::string text = "caf" + std::string(kEAcute) + " " + std::string(kReplacement);
    EXPECT_EQ(host.log.containing("printed " + text).size(), 1u);
    EXPECT_EQ(host.log.containing("logged " + text).size(), 1u);
    EXPECT_EQ(host.log.containing("bytes " + std::string(kReplacement)).size(), 1u);
}

TEST(PythonCore, PartialBytesEndWithTheirCall) {
    ASSERT_STARTED();
    Object output("core_fixtures.output:Output");
    ASSERT_CREATED(output);
    ModuleHost first;
    ModuleHost second;
    ASSERT_EQ(call(output, "partial_bytes", "null", &first.api).status, PRAENORA_STATUS_OK);
    ASSERT_EQ(call(output, "prints", R"({"text":"clean"})", &second.api).status,
              PRAENORA_STATUS_OK);
    // The unfinished sequence is the first call's, as U+FFFD; the next call's records are clean.
    EXPECT_EQ(first.log.containing("partial").size(), 1u);
    EXPECT_EQ(first.log.containing("partial " + std::string(kReplacement)).size(), 1u);
    EXPECT_EQ(second.log.containing("printed: clean").size(), 1u);
    for (const Record& record : second.log.records()) {
        EXPECT_TRUE(valid_utf8(record.message));
        EXPECT_EQ(record.message.find(kReplacement), std::string::npos) << record.message;
    }
}

TEST(PythonCore, PrintBecomesRecord) {
    ASSERT_STARTED();
    ModuleHost host;
    CapturedDescriptors descriptors;
    Object output("core_fixtures.output:Output", "null", &host.api);
    const CallResult result = call(output, "prints", R"({"text":"hello"})");
    const auto [out, err] = descriptors.finish();
    ASSERT_TRUE(descriptors.ok());
    ASSERT_CREATED(output);
    ASSERT_EQ(result.status, PRAENORA_STATUS_OK) << result.error_text;
    EXPECT_EQ(out, "") << "descriptor 1 received output";
    EXPECT_EQ(err, "") << "descriptor 2 received output";

    const auto at = [&](std::string_view text) { return host.log.containing(text); };
    ASSERT_EQ(at("constructed").size(), 1u);
    EXPECT_EQ(at("constructed")[0].level, PRAENORA_LOG_INFO);
    ASSERT_EQ(at("printed: hello").size(), 1u);
    EXPECT_EQ(at("printed: hello")[0].level, PRAENORA_LOG_INFO);
    EXPECT_EQ(at("printed: hello")[0].message, "printed: hello") << "one record per line";
    ASSERT_EQ(at("to stderr: hello").size(), 1u);
    EXPECT_EQ(at("to stderr: hello")[0].level, PRAENORA_LOG_WARN);
    // An unfinished line ends with the call that wrote it.
    ASSERT_EQ(at("unfinished").size(), 1u);
    EXPECT_EQ(at("unfinished")[0].message, "unfinished");
}

TEST(PythonCore, LoggingBecomesRecord) {
    ASSERT_STARTED();
    ModuleHost host;
    Object output("core_fixtures.output:Output", "null", &host.api);
    ASSERT_CREATED(output);
    const CallResult result = call(output, "logs");
    ASSERT_EQ(result.status, PRAENORA_STATUS_OK) << result.error_text;
    const std::pair<const char*, std::int32_t> expected[] = {
        {"a debug record", PRAENORA_LOG_DEBUG},
        {"an info record", PRAENORA_LOG_INFO},
        {"a warning record", PRAENORA_LOG_WARN},
        {"an error record", PRAENORA_LOG_ERROR},
        {"a critical record", PRAENORA_LOG_CRITICAL}};
    for (const auto& [text, level] : expected) {
        const std::vector<Record> found = host.log.containing(text);
        ASSERT_EQ(found.size(), 1u) << text;
        EXPECT_EQ(found[0].level, level) << text;
        EXPECT_EQ(found[0].message, std::string("core_fixtures.output: ") + text);
    }
}

TEST(PythonCore, RecordsGoToTheCallsHost) {
    ASSERT_STARTED();
    // Created without a module host: the object's records go to the provider host.
    const std::size_t before = process_host().log.containing("constructed").size();
    Object output("core_fixtures.output:Output");
    ASSERT_CREATED(output);
    EXPECT_EQ(process_host().log.containing("constructed").size(), before + 1);

    // A host passed to the call receives the call's records, and only that call's.
    ModuleHost first;
    ModuleHost second;
    ASSERT_EQ(call(output, "prints", R"({"text":"to the first"})", &first.api).status,
              PRAENORA_STATUS_OK);
    ASSERT_EQ(call(output, "prints", R"({"text":"to the second"})", &second.api).status,
              PRAENORA_STATUS_OK);
    EXPECT_EQ(first.log.containing("printed: to the first").size(), 1u);
    EXPECT_TRUE(first.log.containing("to the second").empty());
    EXPECT_EQ(second.log.containing("printed: to the second").size(), 1u);
    EXPECT_TRUE(second.log.containing("to the first").empty());
    // Without one, the object's own host: here the provider host.
    ASSERT_EQ(call(output, "prints", R"({"text":"to the provider"})").status, PRAENORA_STATUS_OK);
    EXPECT_EQ(process_host().log.containing("printed: to the provider").size(), 1u);

    // A thread of the object's own prints outside any call: to the provider host of the process.
    ModuleHost third;
    ASSERT_EQ(call(output, "background", "null", &third.api).status, PRAENORA_STATUS_OK);
    ASSERT_EQ(call(output, "finish", "null", &third.api).status, PRAENORA_STATUS_OK);
    EXPECT_TRUE(third.log.containing("a thread of its own").empty());
    EXPECT_EQ(process_host().log.containing("printed by a thread of its own").size(), 1u);
}

TEST(PythonCore, CancelRequestedReachesPython) {
    ASSERT_STARTED();
    ModuleHost host;
    host.cancel_after = 3;
    Object poller("core_fixtures.cancel:Poller", "null", &host.api);
    ASSERT_CREATED(poller);
    const CallResult result = call(poller, "poll", R"({"limit":100})");
    EXPECT_EQ(result.status, PRAENORA_STATUS_CANCELLED) << result.error_text;
    EXPECT_EQ(result.error["details"]["polls"], 4);
    EXPECT_EQ(host.polls.load(), 4);

    // The flag of a host passed to the call, not the object's.
    ModuleHost calm;
    const CallResult calm_result = call(poller, "poll", R"({"limit":10})", &calm.api);
    ASSERT_EQ(calm_result.status, PRAENORA_STATUS_OK) << calm_result.error_text;
    EXPECT_EQ(json::parse(calm_result.result), json::parse(R"({"polls":10})"));
    EXPECT_EQ(calm.polls.load(), 10);
    EXPECT_EQ(host.polls.load(), 4);
}

// ---------------------------------------------------------------------------------------------
// The code of a path.

TEST(PythonCore, PathDigestMismatchIsDataInvalid) {
    ASSERT_STARTED();
    praenora::test::ScratchDir scratch;
    const fs::path root = scratch.path() / "sources";
    const std::string package = write_package(root);
    const std::string actual = praenora::detail::python::directory_digest(root);
    const std::string wrong(64, actual[0] == 'a' ? 'b' : 'a');

    Object refused(package + ".mod:Thing", "null", nullptr, root, wrong);
    EXPECT_EQ(refused.status(), PRAENORA_STATUS_DATA_INVALID) << refused.error_text();
    EXPECT_EQ(refused.get(), nullptr);
    const json& details = refused.error()["details"];
    EXPECT_EQ(details["path"], utf8_of(root));
    EXPECT_EQ(details["expected_sha256"], wrong);
    EXPECT_EQ(details["actual_sha256"], actual);
    EXPECT_NE(refused.error().value("message", "").find("differ"), std::string::npos)
        << refused.error_text();

    // A missing directory is an I/O error; a digest that is not 64 lowercase hexadecimal digits
    // is refused before anything is read.
    Object missing(package + ".mod:Thing", "null", nullptr, scratch.path() / "missing", actual);
    EXPECT_EQ(missing.status(), PRAENORA_STATUS_IO_ERROR) << missing.error_text();
    Object malformed(package + ".mod:Thing", "null", nullptr, root, "ABC");
    EXPECT_EQ(malformed.status(), PRAENORA_STATUS_INVALID_ARGUMENT) << malformed.error_text();
    Object relative(package + ".mod:Thing", "null", nullptr, fs::path("relative"), "");
    EXPECT_EQ(relative.status(), PRAENORA_STATUS_INVALID_ARGUMENT) << relative.error_text();

    // The right digest is accepted.
    Object accepted(package + ".mod:Thing", "null", nullptr, root, actual);
    ASSERT_CREATED(accepted);
    const CallResult marker = call(accepted, "marker");
    ASSERT_EQ(marker.status, PRAENORA_STATUS_OK) << marker.error_text;
    EXPECT_EQ(marker.result, "\"first\"");
}

TEST(PythonCore, PathCodeIsFixedForTheProcess) {
    ASSERT_STARTED();
    praenora::test::ScratchDir scratch;
    const fs::path root = scratch.path() / "sources";
    const std::string package = write_package(root, "first");
    const std::string first_digest = praenora::detail::python::directory_digest(root);
    {
        Object first(package + ".mod:Thing", "null", nullptr, root, first_digest);
        ASSERT_CREATED(first);
    }
    // The sources change: a create with their new digest is refused, since this process runs
    // the code it imported first.
    std::ofstream(root / package / "mod.py", std::ios::binary | std::ios::app) << "# edited\n";
    const std::string second_digest = praenora::detail::python::directory_digest(root);
    ASSERT_NE(second_digest, first_digest);
    Object second(package + ".mod:Thing", "null", nullptr, root, second_digest);
    EXPECT_EQ(second.status(), PRAENORA_STATUS_DATA_INVALID) << second.error_text();
    EXPECT_EQ(second.get(), nullptr);
    EXPECT_NE(second.error().value("message", "").find("new process"), std::string::npos)
        << second.error_text();
    // The digest it imported is still accepted, with the code it imported.
    Object again(package + ".mod:Thing", "null", nullptr, root, first_digest);
    ASSERT_CREATED(again);
    EXPECT_EQ(call(again, "marker").result, "\"first\"");
}

TEST(PythonCore, PackageClashIsInvalidArgument) {
    ASSERT_STARTED();
    praenora::test::ScratchDir scratch;
    const fs::path first_root = scratch.path() / "first";
    const std::string package = write_package(first_root, "first");
    // A second directory with the same top-level package.
    const fs::path second_root = scratch.path() / "second";
    fs::create_directories(second_root);
    fs::copy(first_root / package, second_root / package, fs::copy_options::recursive);

    Object first(package + ".mod:Thing", "null", nullptr, first_root);
    ASSERT_CREATED(first);
    Object second(package + ".mod:Thing", "null", nullptr, second_root);
    EXPECT_EQ(second.status(), PRAENORA_STATUS_INVALID_ARGUMENT) << second.error_text();
    EXPECT_EQ(second.get(), nullptr);
    const std::string message = second.error().value("message", "");
    EXPECT_NE(message.find(package), std::string::npos) << message;
    EXPECT_NE(message.find("already loaded"), std::string::npos) << message;

    // A package of the environment cannot be shadowed either.
    const fs::path shadow_root = scratch.path() / "shadow";
    fs::create_directories(shadow_root / "json");
    std::ofstream(shadow_root / "json" / "__init__.py", std::ios::binary) << "";
    Object shadow("json:loads", "null", nullptr, shadow_root);
    EXPECT_EQ(shadow.status(), PRAENORA_STATUS_INVALID_ARGUMENT) << shadow.error_text();
}

// ---------------------------------------------------------------------------------------------
// The interpreter and its children.

TEST(PythonCore, HashSeedIsZero) {
    ASSERT_STARTED();
    Object facts("core_fixtures.facts:Facts");
    ASSERT_CREATED(facts);
    const CallResult result = call(facts, "interpreter");
    ASSERT_EQ(result.status, PRAENORA_STATUS_OK) << result.error_text;
    const json document = json::parse(result.result);
    EXPECT_EQ(document["hash_randomization"], 0);
    // hash("abc") of CPython 3.12 with the hash seed 0, on a 64-bit platform:
    // PYTHONHASHSEED=0 python3.12 -c 'print(hash("abc"))'
    EXPECT_EQ(document["hash"].get<std::int64_t>(), INT64_C(-4594863902769663758));
    EXPECT_EQ(document["isolated"], 1);
    EXPECT_EQ(document["dont_write_bytecode"], 1);
    EXPECT_EQ(document["utf8_mode"], 1);
    EXPECT_TRUE(fs::equivalent(praenora::test::utf8_path(document["executable"].get<std::string>()),
                               praenora::test::utf8_path(PRAENORA_TEST_PYTHON_EXECUTABLE)));
    // praenora.sdk comes first on the import path.
    ASSERT_FALSE(document["path"].empty());
    EXPECT_TRUE(fs::equivalent(praenora::test::utf8_path(document["path"][0].get<std::string>()),
                               sdk_dir()));
}

TEST(PythonCore, StartMethodIsSpawn) {
    ASSERT_STARTED();
    Object facts("core_fixtures.facts:Facts");
    ASSERT_CREATED(facts);
    const CallResult result = call(facts, "interpreter");
    ASSERT_EQ(result.status, PRAENORA_STATUS_OK) << result.error_text;
    const json document = json::parse(result.result);
    EXPECT_EQ(document["start_method"], "spawn");
    EXPECT_EQ(document["main_spec"], "praenora.sdk._child_main");
}

TEST(PythonCore, SpawnPoolRuns) {
    ASSERT_STARTED();
    Object children("core_fixtures.children:Children");
    ASSERT_CREATED(children);
    const CallResult result = call(children, "square_root_sums", R"({"counts":[10,1000,5]})");
    ASSERT_EQ(result.status, PRAENORA_STATUS_OK) << result.error_text;
    const json document = json::parse(result.result);
    ASSERT_EQ(document.size(), 3u) << result.result;
    const int counts[] = {10, 1000, 5};
    for (std::size_t i = 0; i < 3; ++i) {
        double expected = 0.0;
        for (int k = 0; k < counts[i]; ++k) {
            expected += std::sqrt(static_cast<double>(k));
        }
        EXPECT_NEAR(document[i]["value"].get<double>(), expected, 1e-9 * (1.0 + expected));
        EXPECT_TRUE(
            fs::equivalent(praenora::test::utf8_path(document[i]["executable"].get<std::string>()),
                           praenora::test::utf8_path(PRAENORA_TEST_PYTHON_EXECUTABLE)));
        EXPECT_EQ(document[i]["isolated"], 1);
    }
}

TEST(PythonCore, ChildOutputAvoidsStdout) {
    ASSERT_STARTED();
    Object children("core_fixtures.children:Children");
    ASSERT_CREATED(children);
    CapturedDescriptors descriptors;
    const CallResult result = call(children, "print_text", R"({"text":"a child prints"})");
    const auto [out, err] = descriptors.finish();
    ASSERT_TRUE(descriptors.ok());
    ASSERT_EQ(result.status, PRAENORA_STATUS_OK) << result.error_text;
    EXPECT_EQ(out.find("a child prints"), std::string::npos) << "descriptor 1: " << out;
    EXPECT_EQ(out, "") << "descriptor 1 received output";
#ifndef _WIN32
    // On Windows, multiprocessing starts its children without inherited handles.
    EXPECT_NE(err.find("a child prints"), std::string::npos) << "descriptor 2: " << err;
#endif
}

TEST(PythonCore, ChildrenImportSdk) {
    ASSERT_STARTED();
    Object children("core_fixtures.children:Children");
    ASSERT_CREATED(children);
    const CallResult result = call(children, "sdk_facts");
    ASSERT_EQ(result.status, PRAENORA_STATUS_OK) << result.error_text;
    const json document = json::parse(result.result);
    ASSERT_EQ(document.size(), 1u) << result.result;
    EXPECT_EQ(document[0]["status"], PRAENORA_STATUS_DATA_INVALID);
    EXPECT_EQ(document[0]["main"], "praenora.sdk._child_main");
    EXPECT_EQ(document[0]["stdout_is_stderr"], true);
    const fs::path sdk_file = praenora::test::utf8_path(document[0]["sdk_file"].get<std::string>());
    EXPECT_TRUE(fs::equivalent(sdk_file.parent_path(), sdk_dir() / "praenora" / "sdk")) << sdk_file;
}

TEST(PythonCore, ConcurrentCallsOnTwoThreads) {
    ASSERT_STARTED();
    Object first("core_fixtures.echo:Echo", R"({"object":1})");
    Object second("core_fixtures.echo:Echo", R"({"object":2})");
    ASSERT_CREATED(first);
    ASSERT_CREATED(second);
    std::atomic<int> failures{0};
    const auto work = [&failures](const Object& object, int which) {
        ModuleHost host;
        for (int i = 0; i < 200; ++i) {
            const std::string args =
                "{\"thread\":" + std::to_string(which) + ",\"i\":" + std::to_string(i) + "}";
            const CallResult result = call(object, "echo", args, &host.api);
            if (result.status != PRAENORA_STATUS_OK ||
                json::parse(result.result) != json::parse(args)) {
                ++failures;
            }
        }
        const CallResult created = call(object, "created_with", "null", &host.api);
        if (created.status != PRAENORA_STATUS_OK ||
            json::parse(created.result) != json{{"object", which}}) {
            ++failures;
        }
    };
    std::thread one(work, std::cref(first), 1);
    std::thread two(work, std::cref(second), 2);
    one.join();
    two.join();
    EXPECT_EQ(failures.load(), 0);
}

// What Python keeps per thread lasts from one call on a thread to the next, as in a Python
// program: a context variable, decimal's context and a threading.local value that one call sets
// are what the next call on the thread reads; another thread has its own.
TEST(PythonCore, ThreadStateLastsFromCallToCall) {
    ASSERT_STARTED();
    Object threads("core_fixtures.threads:Threads");
    ASSERT_CREATED(threads);
    json first;
    json second;
    std::thread one([&] {
        const CallResult set = call(threads, "set", R"({"value":7})");
        EXPECT_EQ(set.status, PRAENORA_STATUS_OK) << set.error_text;
        const CallResult got = call(threads, "get");
        EXPECT_EQ(got.status, PRAENORA_STATUS_OK) << got.error_text;
        first = parse_or_null(got.result);
    });
    one.join();
    std::thread two([&] {
        const CallResult got = call(threads, "get");
        EXPECT_EQ(got.status, PRAENORA_STATUS_OK) << got.error_text;
        second = parse_or_null(got.result);
    });
    two.join();
    EXPECT_EQ(first, json({{"context", 7}, {"local", 7}, {"precision", 7}}));
    EXPECT_EQ(second, json({{"context", nullptr}, {"local", nullptr}, {"precision", 28}}));
}

// A thread's state lasts as long as the thread: a threading.local value is held after the call
// that set it, and let go once the thread has ended (on Windows a thread's state is kept until the
// process exits).
TEST(PythonCore, ThreadStateEndsWithItsThread) {
    ASSERT_STARTED();
    Object threads("core_fixtures.threads:Threads");
    ASSERT_CREATED(threads);
    const std::string name = R"({"name":"ends-with-its-thread"})";
    json released_in_thread;
    std::thread worker([&] {
        const CallResult kept = call(threads, "keep", name);
        EXPECT_EQ(kept.status, PRAENORA_STATUS_OK) << kept.error_text;
        const CallResult released = call(threads, "released", name);
        EXPECT_EQ(released.status, PRAENORA_STATUS_OK) << released.error_text;
        released_in_thread = parse_or_null(released.result);
    });
    worker.join();
    EXPECT_EQ(released_in_thread, json(false)) << "the value was let go when its call ended";
    const CallResult released = call(threads, "released", name);
    ASSERT_EQ(released.status, PRAENORA_STATUS_OK) << released.error_text;
#ifdef _WIN32
    EXPECT_EQ(json::parse(released.result), json(false));
#else
    EXPECT_EQ(json::parse(released.result), json(true)) << "the ended thread's state was kept";
#endif
}

// Cached bytecode beside a source (in __pycache__) is never read, even when its header matches
// the source's modification time and size: the interpreter compiles each module from its source,
// the code that the digests cover. Its pycache_prefix is a path that can hold no file.
TEST(PythonCore, CachedBytecodeIsNotRead) {
    ASSERT_STARTED();
    Object facts("core_fixtures.facts:Facts");
    ASSERT_CREATED(facts);
    praenora::test::ScratchDir scratch;
    const std::string package = "core_test_cached_bytecode";
    const fs::path source = scratch.path() / package / "mod.py";
    fs::create_directories(source.parent_path());
    std::ofstream(scratch.path() / package / "__init__.py", std::ios::binary) << "";
    const std::string text = "class Thing:\n"
                             "    def __init__(self, document):\n"
                             "        pass\n"
                             "\n"
                             "    def origin(self, document):\n"
                             "        return \"source\"\n";
    std::string cached = text;
    cached.replace(cached.find("source"), 6, "cached"); // the same size
    std::ofstream(source, std::ios::binary) << text;
    const json request{{"source", utf8_of(source)}, {"cached", cached}};
    const CallResult written = call(facts, "write_cached_bytecode", request.dump());
    ASSERT_EQ(written.status, PRAENORA_STATUS_OK) << written.error_text;
    ASSERT_TRUE(fs::is_regular_file(
        praenora::test::utf8_path(json::parse(written.result).get<std::string>())))
        << written.result;

    Object thing(package + ".mod:Thing", "null", nullptr, scratch.path());
    ASSERT_CREATED(thing);
    const CallResult origin = call(thing, "origin");
    ASSERT_EQ(origin.status, PRAENORA_STATUS_OK) << origin.error_text;
    EXPECT_EQ(json::parse(origin.result), "source");

    const CallResult interpreter = call(facts, "interpreter");
    ASSERT_EQ(interpreter.status, PRAENORA_STATUS_OK) << interpreter.error_text;
    const json prefix = json::parse(interpreter.result).at("pycache_prefix");
    ASSERT_TRUE(prefix.is_string()) << interpreter.result;
    std::error_code ec;
    EXPECT_FALSE(fs::exists(praenora::test::utf8_path(prefix.get<std::string>()), ec)) << prefix;
}
