// The native side of Python modules (python_module.hpp), driven in process through the test kit
// (testing.hpp) and the function table, with a fake praenora.python@0 table: what each function of
// the module's table asks of the service, the arguments that name the class and the sources, how
// the service's failures pass through, the class-only object of self_test, and the refusals of the
// adapter itself. No Python is involved, so these tests run in every tree.
//
// The identity of the modules defined below; praenora_add_python_module() passes these as compile
// definitions to real modules.
#define PRAENORA_MODULE_NAME "test-python-adapter"
#define PRAENORA_MODULE_VERSION "1.2.3"
#define PRAENORA_MODULE_CAPABILITIES "[{\"id\":\"test.echo\",\"version\":\"0.1.0\"}]"

#include <praenora/sdk/python_module.hpp>
#include <praenora/sdk/testing.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sdk = praenora::sdk;
namespace fs = std::filesystem;
using nlohmann::json;
using sdk::testing::DirectModule;
using sdk::testing::FakeHost;

#define TEST_DIGEST "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

namespace {

PRAENORA_SDK_DEFINE_PYTHON_MODULE_GETTER("pkg.mod:Thing", TEST_DIGEST, 1, with_self_test)
PRAENORA_SDK_DEFINE_PYTHON_MODULE_GETTER("pkg.mod:Thing", TEST_DIGEST, 0, without_self_test)

std::string text_of(praenora_bytes bytes) {
    return bytes.size == 0 ? std::string() : std::string(bytes.data, bytes.size);
}

praenora_status write(praenora_sink* sink, std::string_view text) {
    return sink->write(sink->context, text.data(), text.size());
}

// A praenora.python@0 table that records what it is asked. Its one object is the fake itself.
struct FakePython {
    struct Created {
        const praenora_host_api* host;
        std::string path, sha256, name, args;
    };
    struct Called {
        std::string method, args;
    };

    praenora_python_v0 table{};
    std::vector<Created> created;
    std::vector<Called> calls;
    int released = 0;
    praenora_status create_status = PRAENORA_STATUS_OK;
    praenora_status call_status = PRAENORA_STATUS_OK;

    FakePython() {
        table.header = {PRAENORA_PYTHON_SERVICE_MAJOR, PRAENORA_PYTHON_SERVICE_MINOR,
                        static_cast<std::uint32_t>(sizeof(praenora_python_v0)), 0u};
        table.context = this;
        table.describe = &describe;
        table.create_object = &create_object;
        table.call = &call;
        table.release_object = &release_object;
    }
    FakePython(const FakePython&) = delete;
    FakePython& operator=(const FakePython&) = delete;

    static praenora_status PRAENORA_CALL describe(void*, praenora_sink* out_json) {
        return write(out_json, "{}");
    }

    static praenora_status PRAENORA_CALL create_object(void* context, const praenora_host_api* host,
                                                       praenora_bytes path, praenora_bytes sha256,
                                                       praenora_bytes name, praenora_bytes args,
                                                       praenora_python_object** out_object,
                                                       praenora_sink* error_json) {
        auto* self = static_cast<FakePython*>(context);
        self->created.push_back(
            {host, text_of(path), text_of(sha256), text_of(name), text_of(args)});
        if (self->create_status != PRAENORA_STATUS_OK) {
            *out_object = nullptr;
            write(error_json, R"({"code":12,"message":"the sources differ","details":{}})");
            return self->create_status;
        }
        *out_object = reinterpret_cast<praenora_python_object*>(self);
        return PRAENORA_STATUS_OK;
    }

    static praenora_status PRAENORA_CALL call(praenora_python_object* object, praenora_bytes method,
                                              praenora_bytes args, praenora_sink* result_json,
                                              praenora_sink* error_json) {
        auto* self = reinterpret_cast<FakePython*>(object);
        self->calls.push_back({text_of(method), text_of(args)});
        if (self->call_status != PRAENORA_STATUS_OK) {
            write(error_json, R"({"code":10,"message":"ValueError: no","details":{}})");
            return self->call_status;
        }
        const std::string name = text_of(method);
        if (name == "invoke") {
            return write(result_json, R"({"outputs":{"out":{"data":{"from":"python"}}}})");
        }
        if (name == "diagnostics") {
            return write(result_json, R"({"invocations":1})");
        }
        if (name == "self_test") {
            return write(result_json, R"({"passed":true,"checks":[]})");
        }
        return write(result_json, "null");
    }

    static void PRAENORA_CALL release_object(praenora_python_object* object) {
        if (object != nullptr) {
            ++reinterpret_cast<FakePython*>(object)->released;
        }
    }
};

const praenora_module_api_v0& table_of(praenora_module_get_api_fn getter) {
    return *reinterpret_cast<const praenora_module_api_v0*>(getter(0));
}

// A sink that collects what is written to it.
struct Collected {
    std::string text;
    praenora_sink sink{this, &Collected::append};

    static praenora_status PRAENORA_CALL append(void* context, const char* data, std::size_t size) {
        static_cast<Collected*>(context)->text.append(data == nullptr ? "" : data, size);
        return PRAENORA_STATUS_OK;
    }
};

// An object of this program, whose file holds the getters' code too.
const char kInThisProgram = 0;

const char* const kCreate = R"({"pmp_version":"0.1.0","step":"s","parameters":{"gain":2}})";

} // namespace

// describe() reports the module's identity, with no call into the service.
TEST(PythonModuleAdapter, DescribeIsTheIdentity) {
    DirectModule module(&without_self_test);
    const auto described = module.describe();
    ASSERT_EQ(described.status, PRAENORA_STATUS_OK);
    const json identity = json::parse(described.output);
    EXPECT_EQ(identity.at("name"), "test-python-adapter");
    EXPECT_EQ(identity.at("version"), "1.2.3");
    EXPECT_EQ(identity.at("capabilities"), json::parse(PRAENORA_MODULE_CAPABILITIES));
    EXPECT_EQ(identity.at("abi"), json({{"major", 0}, {"minor", 0}}));
}

// create() names the class's class method, the sources beside the library and their digest, and
// passes the create document as {"create": <document>}, with the module's own host.
TEST(PythonModuleAdapter, CreateNamesTheClassAndTheSources) {
    FakePython python;
    FakeHost host;
    host.register_service(PRAENORA_PYTHON_SERVICE, PRAENORA_PYTHON_SERVICE_MAJOR, &python.table);
    DirectModule module(&without_self_test);
    const auto created = module.create(host, kCreate);
    ASSERT_EQ(created.status, PRAENORA_STATUS_OK) << created.error;
    ASSERT_EQ(python.created.size(), 1u);
    const FakePython::Created& call = python.created[0];
    EXPECT_EQ(call.host, host.api());
    EXPECT_EQ(call.name, "pkg.mod:Thing._praenora_adapter");
    EXPECT_EQ(call.sha256, TEST_DIGEST);
    EXPECT_EQ(call.args, std::string("{\"create\":") + kCreate + "}");
    const fs::path sources = fs::u8path(call.path);
    EXPECT_TRUE(sources.is_absolute()) << call.path;
    EXPECT_EQ(sources.filename(), "test-python-adapter.python") << call.path;
    // The directory of this program, which holds the getter's code here.
    EXPECT_TRUE(fs::is_directory(sources.parent_path())) << call.path;
    EXPECT_EQ(sources.parent_path(), fs::u8path(sdk::library_directory(&kInThisProgram)))
        << call.path;
}

// initialize, invoke, finalize and diagnostics call the object's methods of those names with the
// call's document (null for finalize and diagnostics); the results reach the host's sinks, and
// destroy releases the object.
TEST(PythonModuleAdapter, CallsForwardToTheObject) {
    FakePython python;
    FakeHost host;
    host.register_service(PRAENORA_PYTHON_SERVICE, PRAENORA_PYTHON_SERVICE_MAJOR, &python.table);
    DirectModule module(&without_self_test);
    ASSERT_EQ(module.create(host, kCreate).status, PRAENORA_STATUS_OK);
    EXPECT_EQ(module.initialize(R"({"run":{"seed":7}})").status, PRAENORA_STATUS_OK);
    const auto invoked = module.invoke(R"({"tick":0})");
    EXPECT_EQ(invoked.status, PRAENORA_STATUS_OK);
    EXPECT_EQ(invoked.output, R"({"outputs":{"out":{"data":{"from":"python"}}}})");
    const auto diagnostics = module.diagnostics();
    EXPECT_EQ(diagnostics.status, PRAENORA_STATUS_OK);
    EXPECT_EQ(diagnostics.output, R"({"invocations":1})");
    EXPECT_EQ(module.finalize().status, PRAENORA_STATUS_OK);
    ASSERT_EQ(python.calls.size(), 4u);
    EXPECT_EQ(python.calls[0].method, "initialize");
    EXPECT_EQ(python.calls[0].args, R"({"run":{"seed":7}})");
    EXPECT_EQ(python.calls[1].method, "invoke");
    EXPECT_EQ(python.calls[1].args, R"({"tick":0})");
    EXPECT_EQ(python.calls[2].method, "diagnostics");
    EXPECT_EQ(python.calls[2].args, "");
    EXPECT_EQ(python.calls[3].method, "finalize");
    EXPECT_EQ(python.calls[3].args, "");
    EXPECT_EQ(python.released, 0);
    module.destroy();
    EXPECT_EQ(python.released, 1);
}

// The service's statuses and error documents are the module's.
TEST(PythonModuleAdapter, FailuresPassThrough) {
    FakePython python;
    FakeHost host;
    host.register_service(PRAENORA_PYTHON_SERVICE, PRAENORA_PYTHON_SERVICE_MAJOR, &python.table);
    {
        python.create_status = PRAENORA_STATUS_DATA_INVALID;
        DirectModule module(&without_self_test);
        const auto created = module.create(host, kCreate);
        EXPECT_EQ(created.status, PRAENORA_STATUS_DATA_INVALID);
        EXPECT_EQ(created.error, R"({"code":12,"message":"the sources differ","details":{}})");
    }
    python.create_status = PRAENORA_STATUS_OK;
    python.call_status = PRAENORA_STATUS_INTERNAL;
    DirectModule module(&without_self_test);
    ASSERT_EQ(module.create(host, kCreate).status, PRAENORA_STATUS_OK);
    const auto invoked = module.invoke("{}");
    EXPECT_EQ(invoked.status, PRAENORA_STATUS_INTERNAL);
    EXPECT_EQ(invoked.error, R"({"code":10,"message":"ValueError: no","details":{}})");
}

// Without the service, or with a table this adapter cannot use, create() fails with UNSUPPORTED;
// an empty create document is INVALID_ARGUMENT. The service is never called.
TEST(PythonModuleAdapter, CreateRefusals) {
    {
        FakeHost host;
        DirectModule module(&without_self_test);
        const auto created = module.create(host, kCreate);
        EXPECT_EQ(created.status, PRAENORA_STATUS_UNSUPPORTED);
        EXPECT_NE(created.error.find("the service praenora.python@0 is not available"),
                  std::string::npos)
            << created.error;
    }
    for (int broken = 0; broken < 3; ++broken) {
        SCOPED_TRACE(broken);
        FakePython python;
        if (broken == 0) {
            python.table.header.abi_major = 1;
        } else if (broken == 1) {
            python.table.header.struct_size = 8;
        } else {
            python.table.call = nullptr;
        }
        FakeHost host;
        host.register_service(PRAENORA_PYTHON_SERVICE, PRAENORA_PYTHON_SERVICE_MAJOR,
                              &python.table);
        DirectModule module(&without_self_test);
        EXPECT_EQ(module.create(host, kCreate).status, PRAENORA_STATUS_UNSUPPORTED);
        EXPECT_TRUE(python.created.empty());
    }
    FakePython python;
    FakeHost host;
    host.register_service(PRAENORA_PYTHON_SERVICE, PRAENORA_PYTHON_SERVICE_MAJOR, &python.table);
    DirectModule module(&without_self_test);
    EXPECT_EQ(module.create(host, " \n").status, PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_TRUE(python.created.empty());
}

// With a self test, the table's self_test creates a class-only object ({"create":null}), calls its
// self_test, writes the report and releases the object; a failure becomes a log record of level
// error. Without one, the table's self_test is NULL.
TEST(PythonModuleAdapter, SelfTestUsesAClassOnlyObject) {
    EXPECT_EQ(table_of(&without_self_test).self_test, nullptr);
    const praenora_module_api_v0& table = table_of(&with_self_test);
    ASSERT_NE(table.self_test, nullptr);

    FakePython python;
    FakeHost host;
    host.register_service(PRAENORA_PYTHON_SERVICE, PRAENORA_PYTHON_SERVICE_MAJOR, &python.table);
    Collected report;
    EXPECT_EQ(table.self_test(host.api(), &report.sink), PRAENORA_STATUS_OK);
    EXPECT_EQ(report.text, R"({"passed":true,"checks":[]})");
    ASSERT_EQ(python.created.size(), 1u);
    EXPECT_EQ(python.created[0].args, R"({"create":null})");
    EXPECT_EQ(python.created[0].name, "pkg.mod:Thing._praenora_adapter");
    ASSERT_EQ(python.calls.size(), 1u);
    EXPECT_EQ(python.calls[0].method, "self_test");
    EXPECT_EQ(python.released, 1);

    python.call_status = PRAENORA_STATUS_UNSUPPORTED;
    Collected refused;
    EXPECT_EQ(table.self_test(host.api(), &refused.sink), PRAENORA_STATUS_UNSUPPORTED);
    EXPECT_EQ(python.released, 2);
    const auto logs = host.logs();
    ASSERT_FALSE(logs.empty());
    EXPECT_EQ(logs.back().level, PRAENORA_LOG_ERROR);
    EXPECT_EQ(logs.back().message,
              R"(self_test: {"code":10,"message":"ValueError: no","details":{}})");

    FakeHost without;
    Collected unavailable;
    EXPECT_EQ(table.self_test(without.api(), &unavailable.sink), PRAENORA_STATUS_UNSUPPORTED);
    ASSERT_FALSE(without.logs().empty());
    EXPECT_NE(without.logs().back().message.find("praenora.python@0 is not available"),
              std::string::npos);
}
