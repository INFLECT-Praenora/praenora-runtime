// The C ABI v0 headers: struct layout, constants, version macros, and agreement between the C and
// C++ views of the same headers (sdk_abi_c99_check.c is compiled as C99 into this executable).
#include <praenora/sdk/abi.h>
#include <praenora/sdk/status.h>
#include <praenora/sdk/version.h>

#include <praenora/runtime/version.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" {
// Defined in sdk_abi_c99_check.c. Returns 0 when every check passes, otherwise the number of the
// first failing check.
int praenora_sdk_c99_check(void);
// Fills out[0..kLayoutCount) with sizes and offsets as the C compiler sees them, in the order of
// cpp_layout() below.
void praenora_sdk_c99_layout(std::size_t* out);
}

namespace {

constexpr std::size_t kLayoutCount = 12;
constexpr std::size_t kFn = sizeof(void (*)(void));

void cpp_layout(std::size_t* out) {
    out[0] = sizeof(praenora_api_header);
    out[1] = sizeof(praenora_bytes);
    out[2] = sizeof(praenora_sink);
    out[3] = sizeof(praenora_host_api);
    out[4] = offsetof(praenora_host_api, context);
    out[5] = offsetof(praenora_host_api, get_service);
    out[6] = sizeof(praenora_module_api_v0);
    out[7] = offsetof(praenora_module_api_v0, describe);
    out[8] = offsetof(praenora_module_api_v0, destroy);
    out[9] = offsetof(praenora_module_api_v0, self_test);
    out[10] = PRAENORA_MODULE_API_V0_BASE_SIZE;
    out[11] = PRAENORA_HOST_API_V0_BASE_SIZE;
}

} // namespace

TEST(SdkAbi, LayoutIsStable) {
    static_assert(offsetof(praenora_module_api_v0, header) == 0);
    static_assert(sizeof(praenora_api_header) == 16);
    static_assert(sizeof(praenora_bytes) == 2 * sizeof(void*));
    EXPECT_EQ(PRAENORA_MODULE_API_V0_BASE_SIZE, offsetof(praenora_module_api_v0, diagnostics));
}

TEST(SdkAbi, FieldsFollowTheDeclaredOrder) {
    // The function table: the header, then eight function pointers without padding.
    EXPECT_EQ(offsetof(praenora_module_api_v0, describe), sizeof(praenora_api_header));
    EXPECT_EQ(offsetof(praenora_module_api_v0, create), 16 + 1 * kFn);
    EXPECT_EQ(offsetof(praenora_module_api_v0, initialize), 16 + 2 * kFn);
    EXPECT_EQ(offsetof(praenora_module_api_v0, invoke), 16 + 3 * kFn);
    EXPECT_EQ(offsetof(praenora_module_api_v0, finalize), 16 + 4 * kFn);
    EXPECT_EQ(offsetof(praenora_module_api_v0, destroy), 16 + 5 * kFn);
    EXPECT_EQ(offsetof(praenora_module_api_v0, diagnostics), 16 + 6 * kFn);
    EXPECT_EQ(offsetof(praenora_module_api_v0, self_test), 16 + 7 * kFn);
    EXPECT_EQ(sizeof(praenora_module_api_v0), 16 + 8 * kFn);

    // The header fields, each a uint32_t.
    EXPECT_EQ(offsetof(praenora_api_header, abi_major), 0u);
    EXPECT_EQ(offsetof(praenora_api_header, abi_minor), 4u);
    EXPECT_EQ(offsetof(praenora_api_header, struct_size), 8u);
    EXPECT_EQ(offsetof(praenora_api_header, reserved), 12u);

    // The host API: three uint32_t fields, the context pointer at the next pointer boundary, then
    // four callbacks.
    const std::size_t context = (12 + alignof(void*) - 1) / alignof(void*) * alignof(void*);
    EXPECT_EQ(offsetof(praenora_host_api, struct_size), 0u);
    EXPECT_EQ(offsetof(praenora_host_api, abi_major), 4u);
    EXPECT_EQ(offsetof(praenora_host_api, abi_minor), 8u);
    EXPECT_EQ(offsetof(praenora_host_api, context), context);
    EXPECT_EQ(offsetof(praenora_host_api, log), context + sizeof(void*));
    EXPECT_EQ(offsetof(praenora_host_api, log_enabled), context + sizeof(void*) + kFn);
    EXPECT_EQ(offsetof(praenora_host_api, cancel_requested), context + sizeof(void*) + 2 * kFn);
    EXPECT_EQ(offsetof(praenora_host_api, get_service), context + sizeof(void*) + 3 * kFn);
    EXPECT_EQ(sizeof(praenora_host_api), context + sizeof(void*) + 4 * kFn);
    // The base size is frozen at the v0.0 layout, which ends with get_service; today that is the
    // whole struct.
    EXPECT_EQ(PRAENORA_HOST_API_V0_BASE_SIZE, offsetof(praenora_host_api, get_service) + kFn);
    EXPECT_EQ(PRAENORA_HOST_API_V0_BASE_SIZE, sizeof(praenora_host_api));

    // Borrowed bytes and sinks: two pointer-sized fields each.
    EXPECT_EQ(offsetof(praenora_bytes, size), sizeof(void*));
    EXPECT_EQ(offsetof(praenora_sink, write), sizeof(void*));
    EXPECT_EQ(sizeof(praenora_sink), 2 * sizeof(void*));
    EXPECT_EQ(sizeof(praenora_status), 4u);
}

TEST(SdkAbi, StatusCodesMatchSpec) {
    EXPECT_EQ(PRAENORA_STATUS_OK, 0);
    EXPECT_EQ(PRAENORA_STATUS_INVALID_ARGUMENT, 2);
    EXPECT_EQ(PRAENORA_STATUS_CANCELLED, 7);
    EXPECT_EQ(PRAENORA_STATUS_TIMEOUT, 8);
    EXPECT_EQ(PRAENORA_STATUS_INTERNAL, 10);
    EXPECT_EQ(PRAENORA_STATUS_FAILED, 11);
    EXPECT_EQ(PRAENORA_STATUS_DATA_INVALID, 12);
    EXPECT_EQ(PRAENORA_STATUS_MODULE_BASE, 10000);
    EXPECT_STREQ(praenora_status_name(PRAENORA_STATUS_CANCELLED), "CANCELLED");
}

TEST(SdkAbi, StatusNamesCoverGeneralCodes) {
    const struct {
        std::int32_t code;
        const char* name;
    } expected[] = {
        {PRAENORA_STATUS_OK, "OK"},
        {PRAENORA_STATUS_UNKNOWN, "UNKNOWN"},
        {PRAENORA_STATUS_INVALID_ARGUMENT, "INVALID_ARGUMENT"},
        {PRAENORA_STATUS_INVALID_STATE, "INVALID_STATE"},
        {PRAENORA_STATUS_NOT_FOUND, "NOT_FOUND"},
        {PRAENORA_STATUS_UNSUPPORTED, "UNSUPPORTED"},
        {PRAENORA_STATUS_RESOURCE_EXHAUSTED, "RESOURCE_EXHAUSTED"},
        {PRAENORA_STATUS_CANCELLED, "CANCELLED"},
        {PRAENORA_STATUS_TIMEOUT, "TIMEOUT"},
        {PRAENORA_STATUS_IO_ERROR, "IO_ERROR"},
        {PRAENORA_STATUS_INTERNAL, "INTERNAL"},
        {PRAENORA_STATUS_FAILED, "FAILED"},
        {PRAENORA_STATUS_DATA_INVALID, "DATA_INVALID"},
    };
    std::int32_t next = 0;
    for (const auto& entry : expected) {
        EXPECT_EQ(entry.code, next) << entry.name;
        EXPECT_STREQ(praenora_status_name(entry.code), entry.name) << "code " << entry.code;
        ++next;
    }
    for (const std::int32_t other : {13, 99, 100, -1, 9999, 10000, 10001}) {
        EXPECT_STREQ(praenora_status_name(other), "UNKNOWN") << "code " << other;
    }
}

TEST(SdkAbi, LogLevelsMatchSpec) {
    EXPECT_EQ(PRAENORA_LOG_TRACE, 0);
    EXPECT_EQ(PRAENORA_LOG_DEBUG, 1);
    EXPECT_EQ(PRAENORA_LOG_INFO, 2);
    EXPECT_EQ(PRAENORA_LOG_WARN, 3);
    EXPECT_EQ(PRAENORA_LOG_ERROR, 4);
    EXPECT_EQ(PRAENORA_LOG_CRITICAL, 5);
}

TEST(SdkAbi, VersionMacrosMatchTheBuild) {
    const praenora::VersionInfo info = praenora::version_info();
    EXPECT_EQ(PRAENORA_ABI_MAJOR, info.abi_major);
    EXPECT_EQ(PRAENORA_ABI_MINOR, info.abi_minor);
    EXPECT_EQ(std::string(PRAENORA_SDK_VERSION_STRING), info.sdk);
    EXPECT_EQ(std::string(PRAENORA_PMP_VERSION_STRING), info.pmp);
    EXPECT_EQ(std::to_string(PRAENORA_SDK_VERSION_MAJOR) + "." +
                  std::to_string(PRAENORA_SDK_VERSION_MINOR) + "." +
                  std::to_string(PRAENORA_SDK_VERSION_PATCH),
              PRAENORA_SDK_VERSION_STRING);
    EXPECT_STREQ(PRAENORA_MODULE_ENTRY_POINT, "praenora_module_get_api");
}

TEST(SdkAbi, HeadersCompileAsC99) { EXPECT_EQ(praenora_sdk_c99_check(), 0); }

TEST(SdkAbi, CAndCppAgreeOnLayout) {
    std::size_t c[kLayoutCount] = {};
    std::size_t cpp[kLayoutCount] = {};
    praenora_sdk_c99_layout(c);
    cpp_layout(cpp);
    for (std::size_t i = 0; i < kLayoutCount; ++i) {
        EXPECT_EQ(c[i], cpp[i]) << "layout entry " << i;
    }
}
