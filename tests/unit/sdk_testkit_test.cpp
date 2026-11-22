// The in-process test kit (testing.hpp): a fake host and a driver for a module's function table,
// with no dynamic loading.
#define PRAENORA_MODULE_NAME "test-kit"
#define PRAENORA_MODULE_VERSION "0.1.0"
#define PRAENORA_MODULE_CAPABILITIES "[{\"id\":\"test.echo\",\"version\":\"0.1.0\"}]"

#include <praenora/sdk/module.hpp>
#include <praenora/sdk/testing.hpp>

#include "test_gate.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace sdk = praenora::sdk;
using sdk::testing::CallResult;
using sdk::testing::DirectModule;
using sdk::testing::FakeHost;
using sdk::testing::LogRecord;

namespace {

std::atomic<int> g_destroyed{0};

constexpr int kLogThreads = 4;
constexpr int kLogsPerThread = 250;

class KitModule final : public sdk::Module {
public:
    explicit KitModule(const sdk::CreateContext& context) : host_(context.host) {
        host_.log(sdk::LogLevel::info, "created");
    }

    ~KitModule() override { g_destroyed.fetch_add(1); }

    void initialize(const sdk::InitializeContext&) override {
        host_.log(sdk::LogLevel::debug, "initialized");
    }

    std::string invoke(std::string_view invocation) override {
        if (invocation == "work") {
            if (host_.cancel_requested()) {
                throw sdk::Error(PRAENORA_STATUS_CANCELLED, "stopped on request");
            }
            return "worked";
        }
        if (invocation == "gate") {
            const auto* gate = static_cast<const praenora_test_gate_v0*>(
                host_.service(PRAENORA_TEST_GATE_SERVICE, 0));
            if (gate == nullptr) {
                throw sdk::Error(PRAENORA_STATUS_UNSUPPORTED, "no test.gate service");
            }
            return gate->is_open(gate->context) != 0 ? "open" : "closed";
        }
        if (invocation == "log-from-threads") {
            std::vector<std::thread> threads;
            for (int t = 0; t < kLogThreads; ++t) {
                threads.emplace_back([this, t] {
                    for (int i = 0; i < kLogsPerThread; ++i) {
                        host_.log(sdk::LogLevel::trace, "thread " + std::to_string(t));
                    }
                });
            }
            for (auto& thread : threads) {
                thread.join();
            }
            return "logged";
        }
        host_.log(sdk::LogLevel::warn, invocation);
        return "{}";
    }

private:
    sdk::Host host_;
};

PRAENORA_SDK_DEFINE_MODULE_GETTER(KitModule, get_kit_api)

// A gate whose state the test sets.
struct Gate {
    static std::int32_t PRAENORA_CALL is_open(void* context) {
        return static_cast<Gate*>(context)->open.load() ? 1 : 0;
    }

    std::atomic<bool> open{false};
    praenora_test_gate_v0 table{
        {0u, 0u, static_cast<std::uint32_t>(sizeof(praenora_test_gate_v0)), 0u},
        this,
        &Gate::is_open};
};

// Hand-written tables that break one acceptance rule each; describe() is never called.
praenora_status PRAENORA_CALL no_describe(praenora_sink*) { return PRAENORA_STATUS_INTERNAL; }

praenora_module_api_v0 broken_table() {
    const auto* good = reinterpret_cast<const praenora_module_api_v0*>(get_kit_api(0));
    praenora_module_api_v0 copy = *good;
    copy.describe = &no_describe;
    return copy;
}

const praenora_api_header* PRAENORA_CALL null_table(std::uint32_t) { return nullptr; }

const praenora_api_header* PRAENORA_CALL major_seven(std::uint32_t) {
    static const praenora_module_api_v0 table = [] {
        praenora_module_api_v0 t = broken_table();
        t.header.abi_major = 7;
        return t;
    }();
    return &table.header;
}

const praenora_api_header* PRAENORA_CALL short_table(std::uint32_t) {
    static const praenora_module_api_v0 table = [] {
        praenora_module_api_v0 t = broken_table();
        t.header.struct_size = 8;
        return t;
    }();
    return &table.header;
}

const praenora_api_header* PRAENORA_CALL missing_create(std::uint32_t) {
    static const praenora_module_api_v0 table = [] {
        praenora_module_api_v0 t = broken_table();
        t.create = nullptr;
        return t;
    }();
    return &table.header;
}

const praenora_api_header* PRAENORA_CALL no_diagnostics(std::uint32_t abi_major) {
    static const praenora_module_api_v0 table = [] {
        praenora_module_api_v0 t = *reinterpret_cast<const praenora_module_api_v0*>(get_kit_api(0));
        t.diagnostics = nullptr;
        return t;
    }();
    return abi_major == 0 ? &table.header : nullptr;
}

} // namespace

TEST(TestKit, CapturesLogs) {
    FakeHost host;
    DirectModule module(&get_kit_api);
    ASSERT_EQ(module.create(host, "{}").status, PRAENORA_STATUS_OK);
    ASSERT_EQ(module.initialize("{}").status, PRAENORA_STATUS_OK);
    ASSERT_EQ(module.invoke("hello").status, PRAENORA_STATUS_OK);

    const std::vector<LogRecord> logs = host.logs();
    ASSERT_EQ(logs.size(), 3u);
    EXPECT_EQ(logs[0].level, PRAENORA_LOG_INFO);
    EXPECT_EQ(logs[0].message, "created");
    EXPECT_EQ(logs[1].level, PRAENORA_LOG_DEBUG);
    EXPECT_EQ(logs[1].message, "initialized");
    EXPECT_EQ(logs[2].level, PRAENORA_LOG_WARN);
    EXPECT_EQ(logs[2].message, "hello");
    EXPECT_NE(host.api()->log_enabled(host.api()->context, PRAENORA_LOG_TRACE), 0);
}

TEST(TestKit, LogCaptureIsThreadSafe) {
    FakeHost host;
    DirectModule module(&get_kit_api);
    ASSERT_EQ(module.create(host, "{}").status, PRAENORA_STATUS_OK);
    ASSERT_EQ(module.invoke("log-from-threads").output, "logged");
    std::size_t from_threads = 0;
    for (const LogRecord& record : host.logs()) {
        from_threads += record.level == PRAENORA_LOG_TRACE ? 1 : 0;
    }
    EXPECT_EQ(from_threads, static_cast<std::size_t>(kLogThreads * kLogsPerThread));
}

TEST(TestKit, CancelFlagVisibleToModule) {
    FakeHost host;
    DirectModule module(&get_kit_api);
    ASSERT_EQ(module.create(host, "{}").status, PRAENORA_STATUS_OK);
    EXPECT_EQ(module.invoke("work").output, "worked");

    host.request_cancel();
    const CallResult cancelled = module.invoke("work");
    EXPECT_EQ(cancelled.status, PRAENORA_STATUS_CANCELLED);
    EXPECT_EQ(nlohmann::json::parse(cancelled.error).at("code"), PRAENORA_STATUS_CANCELLED);
    EXPECT_NE(host.api()->cancel_requested(host.api()->context), 0);
}

TEST(TestKit, ResetCancelClearsTheFlag) {
    FakeHost host;
    DirectModule module(&get_kit_api);
    ASSERT_EQ(module.create(host, "{}").status, PRAENORA_STATUS_OK);

    host.request_cancel();
    EXPECT_NE(host.api()->cancel_requested(host.api()->context), 0);
    EXPECT_EQ(module.invoke("work").status, PRAENORA_STATUS_CANCELLED);

    // As a real host does when the next call starts.
    host.reset_cancel();
    EXPECT_EQ(host.api()->cancel_requested(host.api()->context), 0);
    EXPECT_EQ(module.invoke("work").output, "worked");
}

TEST(TestKit, ServiceLookup) {
    FakeHost host;
    DirectModule module(&get_kit_api);
    ASSERT_EQ(module.create(host, "{}").status, PRAENORA_STATUS_OK);
    EXPECT_EQ(module.invoke("gate").status, PRAENORA_STATUS_UNSUPPORTED);

    Gate gate;
    host.register_service(PRAENORA_TEST_GATE_SERVICE, 0, &gate.table);
    EXPECT_EQ(module.invoke("gate").output, "closed");
    gate.open.store(true);
    EXPECT_EQ(module.invoke("gate").output, "open");

    // Lookups match the whole name and the major version.
    const praenora_host_api* api = host.api();
    EXPECT_EQ(api->get_service(api->context, "test.gate", 9, 0), &gate.table);
    EXPECT_EQ(api->get_service(api->context, "test.gate", 9, 1), nullptr);
    EXPECT_EQ(api->get_service(api->context, "test.gat", 8, 0), nullptr);
    EXPECT_EQ(api->get_service(api->context, "test.gate.x", 11, 0), nullptr);
    EXPECT_EQ(api->get_service(api->context, nullptr, 0, 0), nullptr);
}

TEST(TestKit, DestroyCalledOnce) {
    g_destroyed.store(0);
    {
        FakeHost host;
        DirectModule module(&get_kit_api);
        ASSERT_EQ(module.create(host, "{}").status, PRAENORA_STATUS_OK);
        module.destroy();
        EXPECT_EQ(g_destroyed.load(), 1);
        module.destroy();
        EXPECT_EQ(g_destroyed.load(), 1);
        EXPECT_EQ(module.invoke("work").status, PRAENORA_STATUS_INVALID_STATE);
    }
    EXPECT_EQ(g_destroyed.load(), 1);

    // The destructor destroys an instance that is still alive.
    {
        FakeHost host;
        DirectModule module(&get_kit_api);
        ASSERT_EQ(module.create(host, "{}").status, PRAENORA_STATUS_OK);
    }
    EXPECT_EQ(g_destroyed.load(), 2);
}

TEST(TestKit, CallsNeedOneCreatedInstance) {
    FakeHost host;
    DirectModule module(&get_kit_api);
    EXPECT_EQ(module.initialize("{}").status, PRAENORA_STATUS_INVALID_STATE);
    EXPECT_EQ(module.invoke("{}").status, PRAENORA_STATUS_INVALID_STATE);
    EXPECT_EQ(module.finalize().status, PRAENORA_STATUS_INVALID_STATE);
    EXPECT_EQ(module.diagnostics().status, PRAENORA_STATUS_INVALID_STATE);
    ASSERT_EQ(module.create(host, "{}").status, PRAENORA_STATUS_OK);
    EXPECT_EQ(module.create(host, "{}").status, PRAENORA_STATUS_INVALID_STATE);
    EXPECT_EQ(module.diagnostics().output, "{}");
}

TEST(TestKit, DiagnosticsUnsupportedWhenAbsent) {
    FakeHost host;
    DirectModule module(&no_diagnostics);
    ASSERT_EQ(module.create(host, "{}").status, PRAENORA_STATUS_OK);
    EXPECT_EQ(module.diagnostics().status, PRAENORA_STATUS_UNSUPPORTED);
}

TEST(TestKit, RejectsTablesTheHostWouldReject) {
    EXPECT_THROW(DirectModule{nullptr}, std::invalid_argument);
    EXPECT_THROW(DirectModule{&null_table}, std::invalid_argument);
    EXPECT_THROW(DirectModule{&major_seven}, std::invalid_argument);
    EXPECT_THROW(DirectModule{&short_table}, std::invalid_argument);
    try {
        DirectModule module(&missing_create);
        FAIL() << "a table without create() was accepted";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("create"), std::string::npos) << e.what();
    }
}
