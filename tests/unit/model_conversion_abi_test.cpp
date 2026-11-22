// The model conversion service header (praenora/sdk/model_conversion.h): its name, version and
// limits, the layout of the table, and agreement between the C and C++ views of it
// (model_conversion_c99_check.c is compiled as C99 into this executable).
#include <praenora/sdk/model_conversion.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <string>

extern "C" {
// Defined in model_conversion_c99_check.c. Returns 0 when every check passes, otherwise the number
// of the first failing check.
int praenora_model_conversion_c99_check(void);
// Fills out[0..kLayoutCount) with sizes and offsets as the C compiler sees them, in the order of
// cpp_layout() below.
void praenora_model_conversion_c99_layout(std::size_t* out);
}

namespace {

constexpr std::size_t kLayoutCount = 6;
constexpr std::size_t kFn = sizeof(void (*)(void));

void cpp_layout(std::size_t* out) {
    out[0] = sizeof(praenora_model_conversion_v0);
    out[1] = offsetof(praenora_model_conversion_v0, header);
    out[2] = offsetof(praenora_model_conversion_v0, context);
    out[3] = offsetof(praenora_model_conversion_v0, describe);
    out[4] = offsetof(praenora_model_conversion_v0, convert);
    out[5] = PRAENORA_MODEL_CONVERSION_V0_BASE_SIZE;
}

} // namespace

TEST(ModelConversionAbi, MacrosHaveTheirValues) {
    EXPECT_STREQ(PRAENORA_MODEL_CONVERSION_SERVICE, "praenora.model-conversion");
    EXPECT_EQ(std::strlen(PRAENORA_MODEL_CONVERSION_SERVICE), 25u);
    EXPECT_EQ(sizeof(PRAENORA_MODEL_CONVERSION_SERVICE) - 1, 25u);
    EXPECT_EQ(PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR, 0u);
    EXPECT_EQ(PRAENORA_MODEL_CONVERSION_SERVICE_MINOR, 0u);
    EXPECT_EQ(PRAENORA_MODEL_CONVERSION_MAX_SOURCE_SIZE, std::size_t{16777216});
    EXPECT_EQ(PRAENORA_MODEL_CONVERSION_MAX_SOURCE_SIZE, std::size_t{16} * 1024 * 1024);
    EXPECT_EQ(PRAENORA_MODEL_CONVERSION_MAX_OPTIONS_SIZE, std::size_t{1048576});
    EXPECT_EQ(PRAENORA_MODEL_CONVERSION_MAX_OPTIONS_SIZE, std::size_t{1} * 1024 * 1024);
}

TEST(ModelConversionAbi, TableLayoutIsFrozen) {
    // The common header, the context pointer, then two functions.
    EXPECT_EQ(offsetof(praenora_model_conversion_v0, header), 0u);
    EXPECT_EQ(offsetof(praenora_model_conversion_v0, context), sizeof(praenora_api_header));
    const std::size_t first = sizeof(praenora_api_header) + sizeof(void*);
    EXPECT_EQ(offsetof(praenora_model_conversion_v0, describe), first);
    EXPECT_EQ(offsetof(praenora_model_conversion_v0, convert), first + kFn);
    EXPECT_EQ(sizeof(praenora_model_conversion_v0), first + 2 * kFn);
    // The base size is the v0.0 table's: up to the end of convert.
    EXPECT_EQ(PRAENORA_MODEL_CONVERSION_V0_BASE_SIZE, first + 2 * kFn);
    EXPECT_EQ(PRAENORA_MODEL_CONVERSION_V0_BASE_SIZE, sizeof(praenora_model_conversion_v0));
}

TEST(ModelConversionAbi, HeaderCompilesAsC99) {
    EXPECT_EQ(praenora_model_conversion_c99_check(), 0);
    EXPECT_EQ(praenora_model_conversion_c99_check(), 0) << "the check can run again in one process";
}

TEST(ModelConversionAbi, CAndCppAgreeOnLayout) {
    std::size_t c[kLayoutCount] = {};
    std::size_t cpp[kLayoutCount] = {};
    praenora_model_conversion_c99_layout(c);
    cpp_layout(cpp);
    for (std::size_t i = 0; i < kLayoutCount; ++i) {
        EXPECT_EQ(c[i], cpp[i]) << "layout entry " << i;
    }
}
