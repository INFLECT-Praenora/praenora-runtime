// The provider python (src/providers/python/provider) through the runtime's provider index and
// loader: its manifest as the build tree's runtime lists it, the table of praenora.python@0 it
// offers, the service's describe, and objects created and called through that table with a module
// host API of the test's own (docs/specs/python-service.md). The provider starts the Python core
// once per process, with the build's environment; the fixture classes are those of
// tests/fixtures/python-core. And the fixture provider test-python-echo
// (tests/fixtures/providers/test-python-echo), whose table of test.echo@0 forwards to a Python
// class through the shared glue of the providers that run Python: its echo, and its engine object.
// Built with PRAENORA_WITH_PYTHON_ENVIRONMENT only.
#include "provider/provider_loader.hpp"
#include "provider/provider_manifest.hpp"

#include "test_echo.h"
#include "test_support.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/runtime.hpp>
#include <praenora/sdk/abi.h>
#include <praenora/sdk/python.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using praenora::test::utf8_path;

// The build tree's provider python: its manifest, in the directory where the build tree's runtime
// library looks for its installation's providers.
fs::path manifest_path() { return utf8_path(PRAENORA_TEST_PYTHON_PROVIDER_MANIFEST); }

// The provider, loaded and started once per process, and its praenora.python@0 table.
const praenora_python_v0* service() {
    static const praenora_python_v0* const table = [] {
        namespace detail = praenora::detail;
        const detail::ProviderManifest manifest = detail::load_provider_manifest(manifest_path());
        const detail::LoadedProvider& provider = detail::load_provider(
            manifest,
            {detail::ServiceVersion{PRAENORA_PYTHON_SERVICE, PRAENORA_PYTHON_SERVICE_MAJOR,
                                    PRAENORA_PYTHON_SERVICE_MINOR}},
            std::nullopt, nullptr);
        return static_cast<const praenora_python_v0*>(
            provider.service(PRAENORA_PYTHON_SERVICE, PRAENORA_PYTHON_SERVICE_MAJOR));
    }();
    return table;
}

#define ASSERT_SERVICE()                                                                           \
    do {                                                                                           \
        try {                                                                                      \
            ASSERT_NE(service(), nullptr) << "the provider offers no praenora.python@0 table";     \
        } catch (const praenora::Error& error) {                                                   \
            FAIL() << "the provider python does not start: " << error.what() << " "                \
                   << error.details_json();                                                        \
        }                                                                                          \
    } while (false)

praenora_bytes bytes_of(std::string_view text) { return {text.data(), text.size()}; }

// A sink that keeps what it is given.
struct Capture {
    std::string bytes;
    praenora_sink sink{this, &Capture::write_fn};

    Capture() = default;
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    static praenora_status PRAENORA_CALL write_fn(void* context, const char* data,
                                                  std::size_t size) {
        static_cast<Capture*>(context)->bytes.append(data, size);
        return PRAENORA_STATUS_OK;
    }
};

struct Record {
    std::int32_t level;
    std::string message;
};

// A module's host API of the test's own: the records it is given, and a cancel flag the test
// raises.
struct ModuleHost {
    mutable std::mutex mutex;
    std::vector<Record> records;
    std::atomic<bool> cancel{false};
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
    ModuleHost(const ModuleHost&) = delete;
    ModuleHost& operator=(const ModuleHost&) = delete;

    // The messages of the records at `level`.
    std::vector<std::string> messages(std::int32_t level) const {
        const std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> found;
        for (const Record& record : records) {
            if (record.level == level) {
                found.push_back(record.message);
            }
        }
        return found;
    }

    static void PRAENORA_CALL log_fn(void* context, std::int32_t level, const char* data,
                                     std::size_t size) {
        auto* self = static_cast<ModuleHost*>(context);
        const std::lock_guard<std::mutex> lock(self->mutex);
        self->records.push_back({level, std::string(data, size)});
    }
    static std::int32_t PRAENORA_CALL enabled_fn(void*, std::int32_t) { return 1; }
    static std::int32_t PRAENORA_CALL cancel_fn(void* context) {
        return static_cast<ModuleHost*>(context)->cancel.load() ? 1 : 0;
    }
    static const void* PRAENORA_CALL service_fn(void*, const char*, std::size_t, std::uint32_t) {
        return nullptr;
    }
};

std::string fixtures_dir() {
    return (fs::path(utf8_path(PRAENORA_TEST_FIXTURES_DIR)) / "python-core").u8string();
}

// An object created through the service's table, released when it goes out of scope.
class Object {
public:
    Object(std::string_view name, std::string_view args, const praenora_host_api* host) {
        Capture error;
        const std::string path = fixtures_dir();
        status_ = service()->create_object(service()->context, host, bytes_of(path), bytes_of(""),
                                           bytes_of(name), bytes_of(args), &object_, &error.sink);
        error_ = error.bytes;
    }
    ~Object() { service()->release_object(object_); }
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

    praenora_status status() const { return status_; }
    const std::string& error() const { return error_; }

    // Calls `method` with `args`: the status, and the result or the error document.
    std::pair<praenora_status, std::string> call(std::string_view method,
                                                 std::string_view args) const {
        Capture result, error;
        const praenora_status status =
            service()->call(object_, bytes_of(method), bytes_of(args), &result.sink, &error.sink);
        return {status, status == PRAENORA_STATUS_OK ? result.bytes : error.bytes};
    }

private:
    praenora_status status_ = PRAENORA_STATUS_INTERNAL;
    praenora_python_object* object_ = nullptr;
    std::string error_;
};

// The fixture provider test-python-echo: its manifest, in its directory of the fixture providers.
fs::path echo_manifest_path() { return utf8_path(PRAENORA_TEST_PYTHON_ECHO_MANIFEST); }

// The provider, loaded and started once per process, and its test.echo@0 table.
const praenora_test_echo_v0* echo_service() {
    static const praenora_test_echo_v0* const table = [] {
        namespace detail = praenora::detail;
        const detail::ProviderManifest manifest =
            detail::load_provider_manifest(echo_manifest_path());
        const detail::LoadedProvider& provider = detail::load_provider(
            manifest, {detail::ServiceVersion{PRAENORA_TEST_ECHO_SERVICE, 0, 0}}, std::nullopt,
            nullptr);
        return static_cast<const praenora_test_echo_v0*>(
            provider.service(PRAENORA_TEST_ECHO_SERVICE, 0));
    }();
    return table;
}

#define ASSERT_ECHO_SERVICE()                                                                      \
    do {                                                                                           \
        try {                                                                                      \
            ASSERT_NE(echo_service(), nullptr) << "test-python-echo offers no test.echo@0 table";  \
        } catch (const praenora::Error& error) {                                                   \
            FAIL() << "test-python-echo does not start: " << error.what() << " "                   \
                   << error.details_json();                                                        \
        }                                                                                          \
    } while (false)

// A sink that refuses every write with `status`.
struct Refusing {
    praenora_status status;
    praenora_sink sink{this, &Refusing::write_fn};

    explicit Refusing(praenora_status refusal) : status(refusal) {}
    Refusing(const Refusing&) = delete;
    Refusing& operator=(const Refusing&) = delete;

    static praenora_status PRAENORA_CALL write_fn(void* context, const char*, std::size_t) {
        return static_cast<Refusing*>(context)->status;
    }
};

std::string read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

// The build tree's runtime, given the directory where it looks for its installation's providers,
// lists the provider python there, usable, once, with the engine object of the build's environment.
TEST(PythonProvider, ListedUsable) {
    praenora::RuntimeOptions options;
    options.use_environment = false;
    options.logging.level = praenora::LogLevel::off;
    options.provider_paths = {manifest_path().parent_path().parent_path().u8string()};
    const praenora::Runtime runtime(options);
    std::vector<praenora::ProviderInfo> found;
    for (const praenora::ProviderInfo& provider : runtime.list_providers()) {
        if (provider.name == "python") {
            found.push_back(provider);
        }
    }
    ASSERT_EQ(found.size(), 1u);
    const praenora::ProviderInfo& provider = found.front();
    EXPECT_TRUE(provider.usable) << provider.problem;
    EXPECT_EQ(utf8_path(provider.manifest_path).lexically_normal(),
              manifest_path().lexically_normal());
    EXPECT_EQ(provider.version, PRAENORA_TEST_RUNTIME_VERSION);
    EXPECT_EQ(provider.services, std::vector<std::string>{"praenora.python@0.0"});
    EXPECT_EQ(json::parse(provider.engine_json), json::parse(PRAENORA_TEST_PYTHON_ENGINE_JSON));
    // The library, its manifest, the core and the environment file are side by side.
    const fs::path directory = manifest_path().parent_path();
    EXPECT_TRUE(fs::is_regular_file(directory / "python-environment.json"));
    EXPECT_TRUE(fs::is_regular_file(utf8_path(provider.library_path)));
    EXPECT_EQ(utf8_path(provider.library_path).parent_path().lexically_normal(),
              directory.lexically_normal());
}

// The table's header names the service's major and minor, its size covers the v0 table, and none
// of its functions is NULL.
TEST(PythonProvider, ServiceTableValid) {
    ASSERT_SERVICE();
    const praenora_python_v0& table = *service();
    EXPECT_EQ(table.header.abi_major, PRAENORA_PYTHON_SERVICE_MAJOR);
    EXPECT_EQ(table.header.abi_minor, PRAENORA_PYTHON_SERVICE_MINOR);
    EXPECT_EQ(table.header.reserved, 0u);
    EXPECT_GE(table.header.struct_size, PRAENORA_PYTHON_V0_BASE_SIZE);
    EXPECT_EQ(table.header.struct_size, sizeof(praenora_python_v0));
    EXPECT_NE(table.describe, nullptr);
    EXPECT_NE(table.create_object, nullptr);
    EXPECT_NE(table.call, nullptr);
    EXPECT_NE(table.release_object, nullptr);
    // The provider in the process gives the same table.
    const praenora::detail::LoadedProvider* provider = praenora::detail::loaded_provider("python");
    ASSERT_NE(provider, nullptr);
    EXPECT_EQ(provider->service(PRAENORA_PYTHON_SERVICE, PRAENORA_PYTHON_SERVICE_MAJOR), service());
}

// describe names the interpreter and the environment the build used, the same bytes every time.
TEST(PythonProvider, ServiceDescribeNamesEnvironment) {
    ASSERT_SERVICE();
    Capture first, second;
    ASSERT_EQ(service()->describe(service()->context, &first.sink), PRAENORA_STATUS_OK);
    ASSERT_EQ(service()->describe(service()->context, &second.sink), PRAENORA_STATUS_OK);
    EXPECT_EQ(first.bytes, second.bytes);
    const json document = json::parse(first.bytes);
    EXPECT_EQ(document.at("python").at("implementation"), "cpython");
    EXPECT_EQ(document.at("python").at("version"), PRAENORA_TEST_PYTHON_VERSION);
    EXPECT_EQ(document.at("python").at("abi"), "cp312");
    EXPECT_EQ(document.at("environment"),
              json::parse(PRAENORA_TEST_PYTHON_ENGINE_JSON).at("environment"));
    // A sink without a write function is refused.
    praenora_sink broken{nullptr, nullptr};
    EXPECT_EQ(service()->describe(service()->context, &broken), PRAENORA_STATUS_INVALID_ARGUMENT);
}

// Objects created through the table with a module's host API: what they print goes to that host,
// at create and at every call (the table's call has no host: the object's own serves), its cancel
// flag reaches praenora.sdk, and an exception is the service's error document.
TEST(PythonProvider, CreateObjectAndCall) {
    ASSERT_SERVICE();
    ModuleHost host;
    {
        const Object echo("core_fixtures.echo:Echo", R"({"made": "here"})", &host.api);
        ASSERT_EQ(echo.status(), PRAENORA_STATUS_OK) << echo.error();
        EXPECT_EQ(echo.call("echo", R"({"x": [1, 2.5, "é"]})"),
                  std::make_pair(PRAENORA_STATUS_OK, std::string(R"({"x":[1,2.5,"é"]})")));
        EXPECT_EQ(echo.call("created_with", ""),
                  std::make_pair(PRAENORA_STATUS_OK, std::string(R"({"made":"here"})")));
        EXPECT_EQ(echo.call("nothing", "null"),
                  std::make_pair(PRAENORA_STATUS_OK, std::string("null")));
        const auto missing = echo.call("no_such_method", "null");
        EXPECT_EQ(missing.first, PRAENORA_STATUS_UNSUPPORTED) << missing.second;
    }
    {
        const Object output("core_fixtures.output:Output", "null", &host.api);
        ASSERT_EQ(output.status(), PRAENORA_STATUS_OK) << output.error();
        EXPECT_EQ(output.call("prints", R"({"text": "hi"})").first, PRAENORA_STATUS_OK);
    }
    const std::vector<std::string> info = host.messages(PRAENORA_LOG_INFO);
    EXPECT_EQ(info, (std::vector<std::string>{"constructed", "printed: hi", "unfinished"}));
    EXPECT_EQ(host.messages(PRAENORA_LOG_WARN), std::vector<std::string>{"to stderr: hi"});
    {
        const Object poller("core_fixtures.cancel:Poller", "null", &host.api);
        ASSERT_EQ(poller.status(), PRAENORA_STATUS_OK) << poller.error();
        EXPECT_EQ(poller.call("poll", R"({"limit": 3})"),
                  std::make_pair(PRAENORA_STATUS_OK, std::string(R"({"polls":3})")));
        host.cancel = true;
        const auto cancelled = poller.call("poll", R"({"limit": 1000000})");
        EXPECT_EQ(cancelled.first, PRAENORA_STATUS_CANCELLED) << cancelled.second;
    }
    {
        const Object raising("core_fixtures.raising:Raising", "null", &host.api);
        ASSERT_EQ(raising.status(), PRAENORA_STATUS_OK) << raising.error();
        const auto internal = raising.call("value_error", "1");
        EXPECT_EQ(internal.first, PRAENORA_STATUS_INTERNAL);
        EXPECT_EQ(json::parse(internal.second).at("message"), "ValueError: the value is 1");
        const auto data_invalid = raising.call("sdk_error", "null");
        EXPECT_EQ(data_invalid.first, PRAENORA_STATUS_DATA_INVALID);
        EXPECT_EQ(json::parse(data_invalid.second).at("details"), json({{"k", 1}}));
    }
    // A class that does not exist fails the create, with the service's error document.
    const Object missing("core_fixtures.echo:NoSuchClass", "null", &host.api);
    EXPECT_EQ(missing.status(), PRAENORA_STATUS_INTERNAL);
    const json error = json::parse(missing.error());
    EXPECT_EQ(error.at("code"), PRAENORA_STATUS_INTERNAL);
    EXPECT_TRUE(error.at("details").at("traceback").is_array());
}

// test-python-echo's echo returns its input through its Python class: every byte, a NUL and bytes
// that are not UTF-8 among them, and none; the class's record "echoed <n> bytes" (level debug)
// reaches the calling module's host; it is called from several threads at once, as a service is;
// and the table's refusals are those of test.echo@0 (tests/support/test_echo.h).
TEST(PythonEchoProvider, ServesEcho) {
    ASSERT_ECHO_SERVICE();
    const praenora_test_echo_v0& table = *echo_service();
    EXPECT_EQ(table.header.abi_major, 0u);
    EXPECT_EQ(table.header.abi_minor, 0u);
    EXPECT_EQ(table.header.reserved, 0u);
    EXPECT_EQ(table.header.struct_size, sizeof(praenora_test_echo_v0));
    ASSERT_NE(table.echo, nullptr);

    ModuleHost host;
    const std::string input = std::string("echo ") + '\0' + " \xff\xfe d\xC3\xA9j\xC3\xA0";
    Capture output;
    ASSERT_EQ(table.echo(table.context, &host.api, bytes_of(input), &output.sink),
              PRAENORA_STATUS_OK);
    EXPECT_EQ(output.bytes, input);
    EXPECT_EQ(host.messages(PRAENORA_LOG_DEBUG),
              std::vector<std::string>{"echoed " + std::to_string(input.size()) + " bytes"});

    // No bytes, without a host; and every byte value, many times over.
    Capture empty;
    ASSERT_EQ(table.echo(table.context, nullptr, praenora_bytes{nullptr, 0}, &empty.sink),
              PRAENORA_STATUS_OK);
    EXPECT_EQ(empty.bytes, "");
    std::string every_byte;
    for (int round = 0; round < 1024; ++round) {
        for (int value = 0; value < 256; ++value) {
            every_byte.push_back(static_cast<char>(value));
        }
    }
    Capture large;
    ASSERT_EQ(table.echo(table.context, nullptr, bytes_of(every_byte), &large.sink),
              PRAENORA_STATUS_OK);
    EXPECT_TRUE(large.bytes == every_byte) << "the echo of 256 KiB differs from its input";

    // Four threads at once, each with its own input and host.
    std::vector<std::thread> threads;
    std::vector<int> wrong(4, 0);
    std::vector<ModuleHost> hosts(4);
    for (int thread = 0; thread < 4; ++thread) {
        threads.emplace_back([&, thread] {
            for (int call = 0; call < 25; ++call) {
                const std::string text =
                    "thread " + std::to_string(thread) + " call " + std::to_string(call);
                Capture echoed;
                if (table.echo(table.context, &hosts[thread].api, bytes_of(text), &echoed.sink) !=
                        PRAENORA_STATUS_OK ||
                    echoed.bytes != text) {
                    ++wrong[thread];
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (int thread = 0; thread < 4; ++thread) {
        EXPECT_EQ(wrong[thread], 0) << "thread " << thread;
        EXPECT_EQ(hosts[thread].messages(PRAENORA_LOG_DEBUG).size(), 25u) << "thread " << thread;
    }

    // The refusals: no output sink, one without a write function, data that is NULL with a size;
    // a sink that refuses the echo gives the echo its status.
    EXPECT_EQ(table.echo(table.context, nullptr, bytes_of("x"), nullptr),
              PRAENORA_STATUS_INVALID_ARGUMENT);
    praenora_sink broken{nullptr, nullptr};
    EXPECT_EQ(table.echo(table.context, nullptr, bytes_of("x"), &broken),
              PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(table.echo(table.context, nullptr, praenora_bytes{nullptr, 3}, &output.sink),
              PRAENORA_STATUS_INVALID_ARGUMENT);
    Refusing refusing(PRAENORA_STATUS_IO_ERROR);
    EXPECT_EQ(table.echo(table.context, nullptr, bytes_of("x"), &refusing.sink),
              PRAENORA_STATUS_IO_ERROR);
}

// test-python-echo's engine object is its own, {"name": "test-python-echo-engine", "version":
// "1.0.0"}, with the environment's added: in its manifest exactly as its build wrote ENGINE_JSON,
// the environment's object after its last entry, and in its describe() once it started; the
// environment's object is the one of the provider python's engine. Its directory holds the
// library, the manifest, the environment file and the copy of its Python sources.
TEST(PythonEchoProvider, EngineHasEnvironment) {
    const praenora::detail::ProviderManifest manifest =
        praenora::detail::load_provider_manifest(echo_manifest_path());
    const json environment = json::parse(PRAENORA_TEST_PYTHON_ENGINE_JSON).at("environment");
    const json expected = {
        {"name", "test-python-echo-engine"}, {"version", "1.0.0"}, {"environment", environment}};
    EXPECT_EQ(manifest.engine, expected);
    EXPECT_EQ(environment.at("name"), PRAENORA_TEST_PYTHON_ENVIRONMENT_NAME);
    EXPECT_EQ(environment.at("version"), PRAENORA_TEST_PYTHON_ENVIRONMENT_VERSION);
    EXPECT_EQ(environment.at("package"), PRAENORA_TEST_PYTHON_ENVIRONMENT_PACKAGE);
    EXPECT_EQ(environment.at("lock_sha256"), PRAENORA_TEST_PYTHON_ENVIRONMENT_LOCK_SHA256);
    const std::string written =
        std::string("\"engine\": {\"name\": \"test-python-echo-engine\", \"version\": "
                    "\"1.0.0\", \"environment\": {\"lock_sha256\":\"") +
        PRAENORA_TEST_PYTHON_ENVIRONMENT_LOCK_SHA256 + "\",\"name\":\"" +
        PRAENORA_TEST_PYTHON_ENVIRONMENT_NAME + "\",\"package\":\"" +
        PRAENORA_TEST_PYTHON_ENVIRONMENT_PACKAGE + "\",\"version\":\"" +
        PRAENORA_TEST_PYTHON_ENVIRONMENT_VERSION + "\"}}\n";
    const std::string text = read_text(echo_manifest_path());
    EXPECT_NE(text.find(written), std::string::npos) << "the manifest does not hold\n"
                                                     << written << "\nbut:\n"
                                                     << text;

    const fs::path directory = echo_manifest_path().parent_path();
    EXPECT_TRUE(fs::is_regular_file(manifest.library_path));
    EXPECT_EQ(manifest.library_path.parent_path().lexically_normal(), directory.lexically_normal());
    EXPECT_TRUE(fs::is_regular_file(directory / "python-environment.json"));
    EXPECT_TRUE(fs::is_regular_file(directory / "python" / "test_python_echo" / "__init__.py"));

    ASSERT_ECHO_SERVICE();
    const praenora::detail::LoadedProvider* provider =
        praenora::detail::loaded_provider("test-python-echo");
    ASSERT_NE(provider, nullptr);
    EXPECT_EQ(provider->describe().at("engine"), expected);
}
