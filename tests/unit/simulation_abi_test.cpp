// The simulation service header (praenora/sdk/simulation.h): its name, version and limits, the
// layout of the table and the kernel, and agreement between the C and C++ views of it
// (simulation_c99_check.c is compiled as C99 into this executable).
#include <praenora/sdk/simulation.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" {
// Defined in simulation_c99_check.c. Returns 0 when every check passes, otherwise the number of
// the first failing check.
int praenora_simulation_c99_check(void);
// Fills out[0..kLayoutCount) with sizes and offsets as the C compiler sees them, in the order of
// cpp_layout() below.
void praenora_simulation_c99_layout(std::size_t* out);
}

namespace {

constexpr std::size_t kLayoutCount = 12;
constexpr std::size_t kFn = sizeof(void (*)(void));

void cpp_layout(std::size_t* out) {
    out[0] = sizeof(praenora_simulation_kernel);
    out[1] = offsetof(praenora_simulation_kernel, model);
    out[2] = offsetof(praenora_simulation_kernel, state_count);
    out[3] = offsetof(praenora_simulation_kernel, rhs);
    out[4] = offsetof(praenora_simulation_kernel, quantities);
    out[5] = sizeof(praenora_simulation_v0);
    out[6] = offsetof(praenora_simulation_v0, context);
    out[7] = offsetof(praenora_simulation_v0, describe);
    out[8] = offsetof(praenora_simulation_v0, run_batch);
    out[9] = offsetof(praenora_simulation_v0, close_session);
    out[10] = PRAENORA_SIMULATION_V0_BASE_SIZE;
    out[11] = PRAENORA_SIMULATION_KERNEL_V0_SIZE;
}

} // namespace

TEST(SimulationAbi, NameVersionAndOutcomes) {
    EXPECT_STREQ(PRAENORA_SIMULATION_SERVICE, "praenora.simulation");
    EXPECT_EQ(PRAENORA_SIMULATION_SERVICE_MAJOR, 0u);
    EXPECT_EQ(PRAENORA_SIMULATION_SERVICE_MINOR, 0u);
    EXPECT_EQ(PRAENORA_SIMULATION_RUN_OK, 0);
    EXPECT_EQ(PRAENORA_SIMULATION_RUN_FAILED, 1);
}

TEST(SimulationAbi, LimitsMatchTheSpecification) {
    EXPECT_EQ(PRAENORA_SIMULATION_MAX_MODEL_SIZE, std::size_t{64} * 1024 * 1024);
    EXPECT_EQ(PRAENORA_SIMULATION_MAX_DOCUMENT_SIZE, std::size_t{16} * 1024 * 1024);
    EXPECT_EQ(PRAENORA_SIMULATION_MAX_RUN_VALUES, std::uint64_t{4194304});
}

TEST(SimulationAbi, FieldsFollowTheDeclaredOrder) {
    // The kernel: two uint32_t fields, the model pointer, three uint64_t counts, then four
    // functions.
    EXPECT_EQ(offsetof(praenora_simulation_kernel, struct_size), 0u);
    EXPECT_EQ(offsetof(praenora_simulation_kernel, reserved), 4u);
    EXPECT_EQ(offsetof(praenora_simulation_kernel, model), 8u);
    EXPECT_EQ(offsetof(praenora_simulation_kernel, state_count), 8u + sizeof(void*));
    EXPECT_EQ(offsetof(praenora_simulation_kernel, parameter_count), 16u + sizeof(void*));
    EXPECT_EQ(offsetof(praenora_simulation_kernel, quantity_count), 24u + sizeof(void*));
    const std::size_t functions = 32u + sizeof(void*);
    EXPECT_EQ(offsetof(praenora_simulation_kernel, rhs), functions);
    EXPECT_EQ(offsetof(praenora_simulation_kernel, set_parameters), functions + kFn);
    EXPECT_EQ(offsetof(praenora_simulation_kernel, initial_state), functions + 2 * kFn);
    EXPECT_EQ(offsetof(praenora_simulation_kernel, quantities), functions + 3 * kFn);
    EXPECT_EQ(sizeof(praenora_simulation_kernel), functions + 4 * kFn);
    EXPECT_EQ(PRAENORA_SIMULATION_KERNEL_V0_SIZE, sizeof(praenora_simulation_kernel));

    // The table: the common header, the context pointer, then eleven functions.
    EXPECT_EQ(offsetof(praenora_simulation_v0, header), 0u);
    EXPECT_EQ(offsetof(praenora_simulation_v0, context), sizeof(praenora_api_header));
    const std::size_t first = sizeof(praenora_api_header) + sizeof(void*);
    EXPECT_EQ(offsetof(praenora_simulation_v0, describe), first);
    EXPECT_EQ(offsetof(praenora_simulation_v0, open_session), first + kFn);
    EXPECT_EQ(offsetof(praenora_simulation_v0, describe_model), first + 2 * kFn);
    EXPECT_EQ(offsetof(praenora_simulation_v0, simulate), first + 3 * kFn);
    EXPECT_EQ(offsetof(praenora_simulation_v0, prepare), first + 4 * kFn);
    EXPECT_EQ(offsetof(praenora_simulation_v0, run), first + 5 * kFn);
    EXPECT_EQ(offsetof(praenora_simulation_v0, run_batch), first + 6 * kFn);
    EXPECT_EQ(offsetof(praenora_simulation_v0, open_kernel), first + 7 * kFn);
    EXPECT_EQ(offsetof(praenora_simulation_v0, close_kernel), first + 8 * kFn);
    EXPECT_EQ(offsetof(praenora_simulation_v0, close_plan), first + 9 * kFn);
    EXPECT_EQ(offsetof(praenora_simulation_v0, close_session), first + 10 * kFn);
    EXPECT_EQ(sizeof(praenora_simulation_v0), first + 11 * kFn);
    EXPECT_EQ(PRAENORA_SIMULATION_V0_BASE_SIZE, sizeof(praenora_simulation_v0));
}

TEST(SimulationAbi, HeaderCompilesAsC99) {
    EXPECT_EQ(praenora_simulation_c99_check(), 0);
    EXPECT_EQ(praenora_simulation_c99_check(), 0) << "the check can run again in one process";
}

TEST(SimulationAbi, CAndCppAgreeOnLayout) {
    std::size_t c[kLayoutCount] = {};
    std::size_t cpp[kLayoutCount] = {};
    praenora_simulation_c99_layout(c);
    cpp_layout(cpp);
    for (std::size_t i = 0; i < kLayoutCount; ++i) {
        EXPECT_EQ(c[i], cpp[i]) << "layout entry " << i;
    }
}
