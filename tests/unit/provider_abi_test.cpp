// The provider interface header (src/runtime/provider/provider_abi.h): struct layout, constants
// and agreement between the C and C++ views of it (provider_abi_c99_check.c is compiled as C99
// into this executable).
#include "provider/provider_abi.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" {
// Defined in provider_abi_c99_check.c. Returns 0 when every check passes, otherwise the number of
// the first failing check.
int praenora_provider_c99_check(void);
// Fills out[0..kLayoutCount) with sizes and offsets as the C compiler sees them, in the order of
// cpp_layout() below.
void praenora_provider_c99_layout(std::size_t* out);
}

namespace {

constexpr std::size_t kLayoutCount = 10;
constexpr std::size_t kFn = sizeof(void (*)(void));

void cpp_layout(std::size_t* out) {
    out[0] = sizeof(praenora_provider_host);
    out[1] = offsetof(praenora_provider_host, context);
    out[2] = offsetof(praenora_provider_host, log);
    out[3] = offsetof(praenora_provider_host, log_enabled);
    out[4] = sizeof(praenora_provider_api_v0);
    out[5] = offsetof(praenora_provider_api_v0, describe);
    out[6] = offsetof(praenora_provider_api_v0, start);
    out[7] = offsetof(praenora_provider_api_v0, get_service);
    out[8] = offsetof(praenora_provider_host, load_compiled);
    out[9] = offsetof(praenora_provider_host, save_compiled);
}

} // namespace

TEST(ProviderAbi, VersionAndEntryPoint) {
    EXPECT_EQ(PRAENORA_PROVIDER_ABI_MAJOR, 0u);
    EXPECT_EQ(PRAENORA_PROVIDER_ABI_MINOR, 0u);
    EXPECT_STREQ(PRAENORA_PROVIDER_ENTRY_POINT, "praenora_provider_get_api");
}

TEST(ProviderAbi, FieldsFollowTheDeclaredOrder) {
    // The host: two uint32_t fields, the context pointer, then two log callbacks and, appended
    // after them, the two callbacks of the shared compiled models.
    EXPECT_EQ(offsetof(praenora_provider_host, struct_size), 0u);
    EXPECT_EQ(offsetof(praenora_provider_host, reserved), 4u);
    EXPECT_EQ(offsetof(praenora_provider_host, context), 8u);
    EXPECT_EQ(offsetof(praenora_provider_host, log), 8u + sizeof(void*));
    EXPECT_EQ(offsetof(praenora_provider_host, log_enabled), 8u + sizeof(void*) + kFn);
    EXPECT_EQ(offsetof(praenora_provider_host, load_compiled), 8u + sizeof(void*) + 2 * kFn);
    EXPECT_EQ(offsetof(praenora_provider_host, save_compiled), 8u + sizeof(void*) + 3 * kFn);
    EXPECT_EQ(sizeof(praenora_provider_host), 8u + sizeof(void*) + 4 * kFn);

    // The table: the common header, then three function pointers without padding.
    EXPECT_EQ(offsetof(praenora_provider_api_v0, header), 0u);
    EXPECT_EQ(offsetof(praenora_provider_api_v0, describe), sizeof(praenora_api_header));
    EXPECT_EQ(offsetof(praenora_provider_api_v0, start), 16 + kFn);
    EXPECT_EQ(offsetof(praenora_provider_api_v0, get_service), 16 + 2 * kFn);
    EXPECT_EQ(sizeof(praenora_provider_api_v0), 16 + 3 * kFn);
}

TEST(ProviderAbi, HeaderCompilesAsC99) {
    EXPECT_EQ(praenora_provider_c99_check(), 0);
    EXPECT_EQ(praenora_provider_c99_check(), 0) << "the check can run again in one process";
}

TEST(ProviderAbi, CAndCppAgreeOnLayout) {
    std::size_t c[kLayoutCount] = {};
    std::size_t cpp[kLayoutCount] = {};
    praenora_provider_c99_layout(c);
    cpp_layout(cpp);
    for (std::size_t i = 0; i < kLayoutCount; ++i) {
        EXPECT_EQ(c[i], cpp[i]) << "layout entry " << i;
    }
}
