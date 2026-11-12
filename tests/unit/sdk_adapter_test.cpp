// The C++ adapter (module.hpp). Modules are driven in process through the test kit
// (testing.hpp), or through the function table directly where a test needs to see individual sink
// writes or pass arguments the test kit never passes.
//
// The identity of the module defined below; praenora_add_module() passes these as compile
// definitions to real modules.
#define PRAENORA_MODULE_NAME "test-adapter"
#define PRAENORA_MODULE_VERSION "1.2.3"
#define PRAENORA_MODULE_CAPABILITIES "[{\"id\":\"test.echo\",\"version\":\"0.1.0\"}]"

#include <praenora/sdk/module.hpp>
#include <praenora/sdk/testing.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sdk = praenora::sdk;
using sdk::testing::CallResult;
using sdk::testing::DirectModule;
using sdk::testing::FakeHost;

namespace {

// Chooses its behavior from the invocation text, so each test can ask for one.
class AdapterTestModule final : public sdk::Module {
public:
    explicit AdapterTestModule(const sdk::CreateContext& context)
        : host_(context.host), create_json_(context.create_json) {
        if (context.create_json == "fail") {
            throw sdk::Error(PRAENORA_STATUS_FAILED, "create refused");
        }
    }

    void initialize(const sdk::InitializeContext& context) override {
        initialize_json_ = std::string(context.initialize_json);
    }

    std::string invoke(std::string_view invocation) override {
        ++invocations_;
        if (invocation == "sdk-error") {
            throw sdk::Error(PRAENORA_STATUS_DATA_INVALID, "bad", R"({"field":"x"})");
        }
        if (invocation == "sdk-error-module-code") {
            throw sdk::Error(PRAENORA_STATUS_MODULE_BASE + 1, "module code");
        }
        if (invocation == "sdk-error-500") {
            throw sdk::Error(500, "runtime code");
        }
        if (invocation == "sdk-error-0") {
            throw sdk::Error(PRAENORA_STATUS_OK, "zero");
        }
        if (invocation == "sdk-error-negative") {
            throw sdk::Error(-3, "negative");
        }
        if (invocation == "sdk-error-bad-details") {
            throw sdk::Error(PRAENORA_STATUS_FAILED, "details are not an object", "not json");
        }
        if (invocation == "std-exception") {
            throw std::runtime_error("boom");
        }
        if (invocation == "bad-alloc") {
            throw std::bad_alloc();
        }
        if (invocation == "not-an-exception") {
            throw 42;
        }
        if (invocation == "cancel-requested?") {
            return host_.cancel_requested() ? "yes" : "no";
        }
        return R"({"outputs":{"out":{"data":)" + std::string(invocation) + "}}}";
    }

    std::string diagnostics() const override {
        return R"({"invocations":)" + std::to_string(invocations_) + R"(,"create":)" +
               create_json_ + R"(,"initialize":)" + initialize_json_ + "}";
    }

private:
    sdk::Host host_;
    std::string create_json_;
    std::string initialize_json_ = "null";
    int invocations_ = 0;
};

PRAENORA_SDK_DEFINE_MODULE_GETTER(AdapterTestModule, get_adapter_test_api)

const praenora_module_api_v0* table() {
    return reinterpret_cast<const praenora_module_api_v0*>(get_adapter_test_api(0));
}

praenora_bytes bytes(std::string_view text) { return praenora_bytes{text.data(), text.size()}; }

// A sink that records every write separately, or refuses them with a chosen status.
struct RecordingSink {
    RecordingSink() : sink{this, &RecordingSink::write} {}
    RecordingSink(const RecordingSink&) = delete;
    RecordingSink& operator=(const RecordingSink&) = delete;

    std::string text() const {
        std::string all;
        for (const auto& piece : writes) {
            all += piece;
        }
        return all;
    }

    static praenora_status PRAENORA_CALL write(void* context, const char* data, std::size_t size) {
        auto* self = static_cast<RecordingSink*>(context);
        if (self->refuse_with != PRAENORA_STATUS_OK) {
            return self->refuse_with;
        }
        self->writes.emplace_back(data, size);
        return PRAENORA_STATUS_OK;
    }

    praenora_sink sink;
    std::vector<std::string> writes;
    praenora_status refuse_with = PRAENORA_STATUS_OK;
};

nlohmann::json parse_error(const CallResult& result) { return nlohmann::json::parse(result.error); }

// The test module, created and initialized on a fake host.
struct CreatedModule {
    CreatedModule() {
        EXPECT_EQ(module.create(host, R"({"step":"s"})").status, PRAENORA_STATUS_OK);
        EXPECT_EQ(module.initialize(R"({"run":{"seed":7}})").status, PRAENORA_STATUS_OK);
    }

    FakeHost host;
    DirectModule module{&get_adapter_test_api};
};

} // namespace

TEST(Adapter, DescribeReportsIdentity) {
    DirectModule module(&get_adapter_test_api);
    const CallResult result = module.describe();
    ASSERT_EQ(result.status, PRAENORA_STATUS_OK) << result.error;
    const auto describe = nlohmann::json::parse(result.output);
    EXPECT_EQ(describe.size(), 6u) << describe.dump();
    EXPECT_EQ(describe.at("name"), "test-adapter");
    EXPECT_EQ(describe.at("version"), "1.2.3");
    EXPECT_EQ(describe.at("abi").at("major"), PRAENORA_ABI_MAJOR);
    EXPECT_EQ(describe.at("abi").at("minor"), PRAENORA_ABI_MINOR);
    EXPECT_EQ(describe.at("sdk_version"), PRAENORA_SDK_VERSION_STRING);
    EXPECT_EQ(describe.at("pmp_version"), PRAENORA_PMP_VERSION_STRING);
    ASSERT_EQ(describe.at("capabilities").size(), 1u);
    EXPECT_EQ(describe.at("capabilities")[0].at("id"), "test.echo");
    EXPECT_EQ(describe.at("capabilities")[0].at("version"), "0.1.0");
}

TEST(Adapter, GetterReturnsStaticV0TableOnlyForMajorZero) {
    const praenora_api_header* header = get_adapter_test_api(0);
    ASSERT_NE(header, nullptr);
    EXPECT_EQ(header->abi_major, 0u);
    EXPECT_EQ(header->abi_minor, PRAENORA_ABI_MINOR);
    EXPECT_EQ(header->struct_size, sizeof(praenora_module_api_v0));
    EXPECT_EQ(header->reserved, 0u);
    EXPECT_EQ(get_adapter_test_api(0), header);
    EXPECT_EQ(get_adapter_test_api(1), nullptr);
    EXPECT_EQ(get_adapter_test_api(7), nullptr);

    const praenora_module_api_v0* api = table();
    EXPECT_NE(api->describe, nullptr);
    EXPECT_NE(api->create, nullptr);
    EXPECT_NE(api->initialize, nullptr);
    EXPECT_NE(api->invoke, nullptr);
    EXPECT_NE(api->finalize, nullptr);
    EXPECT_NE(api->destroy, nullptr);
    EXPECT_NE(api->diagnostics, nullptr);
    EXPECT_EQ(api->self_test, nullptr);
}

TEST(Adapter, InvokeReturnsModuleResult) {
    CreatedModule created;
    const CallResult result = created.module.invoke(R"({"tick":0})");
    EXPECT_EQ(result.status, PRAENORA_STATUS_OK) << result.error;
    EXPECT_EQ(result.output, R"({"outputs":{"out":{"data":{"tick":0}}}})");
    EXPECT_TRUE(result.error.empty());
    EXPECT_EQ(created.module.finalize().status, PRAENORA_STATUS_OK);
}

TEST(Adapter, DocumentsReachTheModule) {
    CreatedModule created;
    ASSERT_EQ(created.module.invoke("{}").status, PRAENORA_STATUS_OK);
    ASSERT_EQ(created.module.invoke("{}").status, PRAENORA_STATUS_OK);
    const CallResult result = created.module.diagnostics();
    ASSERT_EQ(result.status, PRAENORA_STATUS_OK) << result.error;
    const auto diagnostics = nlohmann::json::parse(result.output);
    EXPECT_EQ(diagnostics.at("invocations"), 2);
    EXPECT_EQ(diagnostics.at("create").at("step"), "s");
    EXPECT_EQ(diagnostics.at("initialize").at("run").at("seed"), 7);
}

TEST(Adapter, SdkErrorMapsToItsCode) {
    CreatedModule created;
    const CallResult result = created.module.invoke("sdk-error");
    EXPECT_EQ(result.status, PRAENORA_STATUS_DATA_INVALID);
    EXPECT_EQ(result.status, 12);
    EXPECT_TRUE(result.output.empty());
    const auto error = parse_error(result);
    EXPECT_EQ(error.at("code"), 12);
    EXPECT_EQ(error.at("message"), "bad");
    EXPECT_EQ(error.at("details"), nlohmann::json::parse(R"({"field":"x"})"));

    const CallResult module_code = created.module.invoke("sdk-error-module-code");
    EXPECT_EQ(module_code.status, PRAENORA_STATUS_MODULE_BASE + 1);
    EXPECT_EQ(parse_error(module_code).at("code"), PRAENORA_STATUS_MODULE_BASE + 1);
}

TEST(Adapter, ErrorDetailsThatAreNotAnObjectAreKeptAsText) {
    CreatedModule created;
    const CallResult result = created.module.invoke("sdk-error-bad-details");
    EXPECT_EQ(result.status, PRAENORA_STATUS_FAILED);
    EXPECT_EQ(parse_error(result).at("details"), nlohmann::json::parse(R"({"text":"not json"})"));
}

TEST(Adapter, OutOfRangeErrorCodeMapsToInternal) {
    CreatedModule created;
    const struct {
        const char* invocation;
        const char* message;
        int code;
    } cases[] = {
        {"sdk-error-500", "runtime code", 500},
        {"sdk-error-0", "zero", 0},
        {"sdk-error-negative", "negative", -3},
    };
    for (const auto& c : cases) {
        const CallResult result = created.module.invoke(c.invocation);
        EXPECT_EQ(result.status, PRAENORA_STATUS_INTERNAL) << c.invocation;
        const auto error = parse_error(result);
        EXPECT_EQ(error.at("code"), PRAENORA_STATUS_INTERNAL) << c.invocation;
        const auto message = error.at("message").get<std::string>();
        EXPECT_NE(message.find(c.message), std::string::npos) << message;
        EXPECT_NE(message.find(std::to_string(c.code)), std::string::npos) << message;
    }
}

TEST(Adapter, StdExceptionMapsToInternalWithMessage) {
    CreatedModule created;
    const CallResult result = created.module.invoke("std-exception");
    EXPECT_EQ(result.status, PRAENORA_STATUS_INTERNAL);
    EXPECT_EQ(parse_error(result).at("code"), PRAENORA_STATUS_INTERNAL);
    EXPECT_EQ(parse_error(result).at("message"), "boom");

    const CallResult other = created.module.invoke("not-an-exception");
    EXPECT_EQ(other.status, PRAENORA_STATUS_INTERNAL);
    EXPECT_FALSE(parse_error(other).at("message").get<std::string>().empty());
}

TEST(Adapter, BadAllocMapsToResourceExhausted) {
    CreatedModule created;
    const CallResult result = created.module.invoke("bad-alloc");
    EXPECT_EQ(result.status, PRAENORA_STATUS_RESOURCE_EXHAUSTED);
    EXPECT_EQ(parse_error(result).at("code"), PRAENORA_STATUS_RESOURCE_EXHAUSTED);
}

TEST(Adapter, SinkRefusalPropagates) {
    CreatedModule created;
    created.host.refuse_sink_writes(true);
    const CallResult refused = created.module.invoke(R"({"tick":0})");
    EXPECT_EQ(refused.status, PRAENORA_STATUS_RESOURCE_EXHAUSTED);
    EXPECT_TRUE(refused.output.empty());

    created.host.refuse_sink_writes(false);
    EXPECT_EQ(created.module.invoke(R"({"tick":1})").status, PRAENORA_STATUS_OK);
}

TEST(Adapter, SinkRefusalReturnsTheSinkStatus) {
    const praenora_module_api_v0* api = table();
    RecordingSink describe_out;
    describe_out.refuse_with = PRAENORA_STATUS_MODULE_BASE + 5;
    EXPECT_EQ(api->describe(&describe_out.sink), PRAENORA_STATUS_MODULE_BASE + 5);

    FakeHost host;
    praenora_instance* instance = nullptr;
    RecordingSink create_error;
    ASSERT_EQ(api->create(host.api(), bytes("{}"), &instance, &create_error.sink),
              PRAENORA_STATUS_OK);
    RecordingSink result;
    RecordingSink error;
    result.refuse_with = PRAENORA_STATUS_IO_ERROR;
    EXPECT_EQ(api->invoke(instance, bytes("{}"), &result.sink, &error.sink),
              PRAENORA_STATUS_IO_ERROR);
    EXPECT_EQ(nlohmann::json::parse(error.text()).at("code"), PRAENORA_STATUS_IO_ERROR);
    api->destroy(instance);
}

TEST(Adapter, LargeOutputIsWrittenInChunks) {
    const praenora_module_api_v0* api = table();
    FakeHost host;
    praenora_instance* instance = nullptr;
    RecordingSink error;
    ASSERT_EQ(api->create(host.api(), bytes("{}"), &instance, &error.sink), PRAENORA_STATUS_OK);

    const std::string invocation = "\"" + std::string(10000, 'x') + "\"";
    const std::string expected = R"({"outputs":{"out":{"data":)" + invocation + "}}}";
    RecordingSink result;
    ASSERT_EQ(api->invoke(instance, bytes(invocation), &result.sink, &error.sink),
              PRAENORA_STATUS_OK);
    EXPECT_EQ(result.text(), expected);
    const std::size_t chunk = sdk::detail::kSinkChunkSize;
    EXPECT_EQ(chunk, 4096u);
    ASSERT_EQ(result.writes.size(), (expected.size() + chunk - 1) / chunk);
    for (std::size_t i = 0; i + 1 < result.writes.size(); ++i) {
        EXPECT_EQ(result.writes[i].size(), chunk) << "write " << i;
    }
    EXPECT_EQ(result.writes.back().size(), expected.size() - (result.writes.size() - 1) * chunk);
    api->destroy(instance);
}

TEST(Adapter, CreateFailureReportsErrorAndReturnsNoInstance) {
    const praenora_module_api_v0* api = table();
    FakeHost host;
    praenora_instance* instance = reinterpret_cast<praenora_instance*>(&host);
    RecordingSink error;
    EXPECT_EQ(api->create(host.api(), bytes("fail"), &instance, &error.sink),
              PRAENORA_STATUS_FAILED);
    EXPECT_EQ(instance, nullptr);
    const auto report = nlohmann::json::parse(error.text());
    EXPECT_EQ(report.at("code"), PRAENORA_STATUS_FAILED);
    EXPECT_EQ(report.at("message"), "create refused");
}

TEST(Adapter, CreateRejectsUnusableArguments) {
    const praenora_module_api_v0* api = table();
    FakeHost host;
    // Calls create() and checks the status and the code in the error document.
    const auto expect_create = [api](const praenora_host_api* host_api, praenora_bytes document,
                                     praenora_instance** out, praenora_status expected) {
        RecordingSink error;
        EXPECT_EQ(api->create(host_api, document, out, &error.sink), expected);
        EXPECT_EQ(nlohmann::json::parse(error.text()).at("code"), expected);
    };
    praenora_instance* instance = nullptr;
    expect_create(nullptr, bytes("{}"), &instance, PRAENORA_STATUS_INVALID_ARGUMENT);
    expect_create(host.api(), bytes("{}"), nullptr, PRAENORA_STATUS_INVALID_ARGUMENT);
    expect_create(host.api(), praenora_bytes{nullptr, 3}, &instance,
                  PRAENORA_STATUS_INVALID_ARGUMENT);

    // A host of another ABI major, or one too short to hold its fixed fields, is not usable.
    praenora_host_api other_major = *host.api();
    other_major.abi_major = 7;
    expect_create(&other_major, bytes("{}"), &instance, PRAENORA_STATUS_UNSUPPORTED);
    praenora_host_api too_short = *host.api();
    too_short.struct_size = 8;
    expect_create(&too_short, bytes("{}"), &instance, PRAENORA_STATUS_UNSUPPORTED);
    EXPECT_EQ(instance, nullptr);
}

TEST(Adapter, InstanceCallsRejectNullArguments) {
    const praenora_module_api_v0* api = table();
    RecordingSink out;
    EXPECT_EQ(api->describe(nullptr), PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(api->initialize(nullptr, bytes("{}"), &out.sink), PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(api->invoke(nullptr, bytes("{}"), &out.sink, &out.sink),
              PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(api->finalize(nullptr, &out.sink), PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(api->diagnostics(nullptr, &out.sink), PRAENORA_STATUS_INVALID_ARGUMENT);
    api->destroy(nullptr);

    FakeHost host;
    praenora_instance* instance = nullptr;
    ASSERT_EQ(api->create(host.api(), bytes("{}"), &instance, nullptr), PRAENORA_STATUS_OK);
    EXPECT_EQ(api->invoke(instance, bytes("{}"), nullptr, &out.sink),
              PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(api->diagnostics(instance, nullptr), PRAENORA_STATUS_INVALID_ARGUMENT);
    // A failure is still reported by its status when there is no error sink.
    RecordingSink result;
    EXPECT_EQ(api->invoke(instance, bytes("sdk-error"), &result.sink, nullptr),
              PRAENORA_STATUS_DATA_INVALID);
    EXPECT_TRUE(result.writes.empty());
    api->destroy(instance);
}

TEST(Adapter, HostWithShortStructDisablesCancel) {
    FakeHost short_host;
    short_host.set_struct_size(offsetof(praenora_host_api, cancel_requested));
    short_host.request_cancel();
    DirectModule module(&get_adapter_test_api);
    ASSERT_EQ(module.create(short_host, "{}").status, PRAENORA_STATUS_OK);
    EXPECT_EQ(module.invoke("cancel-requested?").output, "no");

    FakeHost full_host;
    full_host.request_cancel();
    DirectModule other(&get_adapter_test_api);
    ASSERT_EQ(other.create(full_host, "{}").status, PRAENORA_STATUS_OK);
    EXPECT_EQ(other.invoke("cancel-requested?").output, "yes");
}

TEST(Adapter, HostChecksStructSizeBeforeEachCallback) {
    const sdk::Host none(nullptr);
    none.log(sdk::LogLevel::info, "dropped");
    EXPECT_FALSE(none.log_enabled(sdk::LogLevel::info));
    EXPECT_FALSE(none.cancel_requested());
    EXPECT_EQ(none.service("test.gate", 0), nullptr);

    FakeHost fake;
    const int service_table = 0;
    fake.register_service("test.gate", 0, &service_table);
    fake.request_cancel();
    const std::size_t sizes[] = {
        offsetof(praenora_host_api, log),
        offsetof(praenora_host_api, log_enabled),
        offsetof(praenora_host_api, cancel_requested),
        offsetof(praenora_host_api, get_service),
        sizeof(praenora_host_api),
    };
    std::size_t expected_logs = 0;
    for (const std::size_t size : sizes) {
        fake.set_struct_size(static_cast<std::uint32_t>(size));
        const sdk::Host host(fake.api());
        host.log(sdk::LogLevel::error, "message");
        if (size > offsetof(praenora_host_api, log)) {
            ++expected_logs;
        }
        EXPECT_EQ(fake.logs().size(), expected_logs) << "struct_size " << size;
        EXPECT_EQ(host.log_enabled(sdk::LogLevel::error),
                  size > offsetof(praenora_host_api, log_enabled))
            << "struct_size " << size;
        EXPECT_EQ(host.cancel_requested(), size > offsetof(praenora_host_api, cancel_requested))
            << "struct_size " << size;
        EXPECT_EQ(host.service("test.gate", 0) == &service_table,
                  size > offsetof(praenora_host_api, get_service))
            << "struct_size " << size;
    }
    const auto logs = fake.logs();
    ASSERT_FALSE(logs.empty());
    EXPECT_EQ(logs.back().level, PRAENORA_LOG_ERROR);
    EXPECT_EQ(logs.back().message, "message");
}

TEST(Adapter, ErrorCarriesCodeMessageAndDetails) {
    const sdk::Error error(PRAENORA_STATUS_NOT_FOUND, "missing", R"({"path":"a"})");
    EXPECT_EQ(error.code(), PRAENORA_STATUS_NOT_FOUND);
    EXPECT_STREQ(error.what(), "missing");
    EXPECT_EQ(error.details_json(), R"({"path":"a"})");
    EXPECT_EQ(sdk::Error(PRAENORA_STATUS_FAILED, "x").details_json(), "{}");
}

TEST(Adapter, JsonEscapeHandlesControlCharacters) {
    EXPECT_EQ(sdk::detail::json_escape("a\"b\n\x01"), "a\\\"b\\n\\u0001");
    EXPECT_EQ(sdk::detail::json_escape("\\/\b\f\r\t\x1f\x7f"), "\\\\/\\b\\f\\r\\t\\u001f\x7f");
    EXPECT_EQ(sdk::detail::json_escape(std::string_view("a\0b", 3)), "a\\u0000b");
    EXPECT_EQ(sdk::detail::json_escape(""), "");
    // Valid UTF-8 passes through unchanged.
    EXPECT_EQ(sdk::detail::json_escape("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x99\x82"),
              "caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x99\x82");
}

TEST(Adapter, JsonEscapeReplacesInvalidUtf8) {
    EXPECT_EQ(sdk::detail::json_escape("\xff"), "\\ufffd");
    EXPECT_EQ(sdk::detail::json_escape("a\xc3"), "a\\ufffd");
    EXPECT_EQ(sdk::detail::json_escape("\xc0\xaf"), "\\ufffd\\ufffd");
    EXPECT_EQ(sdk::detail::json_escape("\xed\xa0\x80"), "\\ufffd\\ufffd\\ufffd");
    EXPECT_EQ(sdk::detail::json_escape("\xf4\x90\x80\x80"), "\\ufffd\\ufffd\\ufffd\\ufffd");
    // Whatever the input, the escaped text is a valid JSON string.
    const std::string escaped = sdk::detail::json_escape("x\x80y\xe2\x82z");
    EXPECT_EQ(nlohmann::json::parse("\"" + escaped + "\"").get<std::string>(),
              "x\xef\xbf\xbdy\xef\xbf\xbd\xef\xbf\xbdz");
}
