// The Python service header (praenora/sdk/python.h): its name, version and limit, the layout of
// the table, and agreement between the C and C++ views of it (python_c99_check.c is compiled as
// C99 into this executable). The header needs no Python: these tests run in every build.
#include <praenora/sdk/python.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>

extern "C" {
// Defined in python_c99_check.c. Returns 0 when every check passes, otherwise the number of the
// first failing check.
int praenora_python_c99_check(void);
// Fills out[0..kLayoutCount) with sizes and offsets as the C compiler sees them, in the order of
// cpp_layout() below.
void praenora_python_c99_layout(std::size_t* out);
}

namespace {

constexpr std::size_t kLayoutCount = 8;
constexpr std::size_t kFn = sizeof(void (*)(void));

void cpp_layout(std::size_t* out) {
    out[0] = sizeof(praenora_python_v0);
    out[1] = offsetof(praenora_python_v0, header);
    out[2] = offsetof(praenora_python_v0, context);
    out[3] = offsetof(praenora_python_v0, describe);
    out[4] = offsetof(praenora_python_v0, create_object);
    out[5] = offsetof(praenora_python_v0, call);
    out[6] = offsetof(praenora_python_v0, release_object);
    out[7] = PRAENORA_PYTHON_V0_BASE_SIZE;
}

} // namespace

TEST(PythonAbi, MacrosHaveTheirValues) {
    EXPECT_STREQ(PRAENORA_PYTHON_SERVICE, "praenora.python");
    EXPECT_EQ(std::strlen(PRAENORA_PYTHON_SERVICE), 15u);
    EXPECT_EQ(sizeof(PRAENORA_PYTHON_SERVICE) - 1, 15u);
    EXPECT_EQ(PRAENORA_PYTHON_SERVICE_MAJOR, 0u);
    EXPECT_EQ(PRAENORA_PYTHON_SERVICE_MINOR, 0u);
    EXPECT_EQ(PRAENORA_PYTHON_MAX_DOCUMENT_SIZE, std::size_t{16777216});
    EXPECT_EQ(PRAENORA_PYTHON_MAX_DOCUMENT_SIZE, std::size_t{16} * 1024 * 1024);
}

TEST(PythonAbi, TableLayoutIsFrozen) {
    // The common header, the context pointer, then four functions.
    EXPECT_EQ(offsetof(praenora_python_v0, header), 0u);
    EXPECT_EQ(offsetof(praenora_python_v0, context), sizeof(praenora_api_header));
    const std::size_t first = sizeof(praenora_api_header) + sizeof(void*);
    EXPECT_EQ(offsetof(praenora_python_v0, describe), first);
    EXPECT_EQ(offsetof(praenora_python_v0, create_object), first + kFn);
    EXPECT_EQ(offsetof(praenora_python_v0, call), first + 2 * kFn);
    EXPECT_EQ(offsetof(praenora_python_v0, release_object), first + 3 * kFn);
    EXPECT_EQ(sizeof(praenora_python_v0), first + 4 * kFn);
    // The base size is the v0.0 table's: up to the end of release_object.
    EXPECT_EQ(PRAENORA_PYTHON_V0_BASE_SIZE, first + 4 * kFn);
    EXPECT_EQ(PRAENORA_PYTHON_V0_BASE_SIZE, sizeof(praenora_python_v0));
}

TEST(PythonAbi, HeaderCompilesAsC99) {
    EXPECT_EQ(praenora_python_c99_check(), 0);
    EXPECT_EQ(praenora_python_c99_check(), 0) << "the check can run again in one process";
}

TEST(PythonAbi, CAndCppAgreeOnLayout) {
    std::size_t c[kLayoutCount] = {};
    std::size_t cpp[kLayoutCount] = {};
    praenora_python_c99_layout(c);
    cpp_layout(cpp);
    for (std::size_t i = 0; i < kLayoutCount; ++i) {
        EXPECT_EQ(c[i], cpp[i]) << "layout entry " << i;
    }
}
