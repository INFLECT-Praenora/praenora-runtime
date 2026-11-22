// The host's sink (loader/output_sink.hpp): the limit and its refusal at the same byte whether the
// sink keeps what it accepts or passes it on (as a module host passes it to the runtime), and the
// refusal that sticks.
#include "loader/output_sink.hpp"

#include <praenora/sdk/abi.h>
#include <praenora/sdk/status.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

using praenora::detail::OutputSink;

namespace {

praenora_status write(OutputSink& sink, const std::string& text) {
    praenora_sink* abi = sink.sink();
    return abi->write(abi->context, text.data(), text.size());
}

} // namespace

TEST(OutputSink, KeepsWhatFitsAndRefusesFromTheLimitOn) {
    OutputSink sink(10);
    EXPECT_EQ(sink.limit(), 10u);
    EXPECT_EQ(write(sink, "abcd"), PRAENORA_STATUS_OK);
    EXPECT_EQ(write(sink, ""), PRAENORA_STATUS_OK);
    EXPECT_EQ(write(sink, "efghij"), PRAENORA_STATUS_OK);
    EXPECT_EQ(sink.size(), 10u);
    EXPECT_FALSE(sink.refused());
    EXPECT_EQ(write(sink, "k"), PRAENORA_STATUS_RESOURCE_EXHAUSTED);
    EXPECT_TRUE(sink.refused());
    // Once refused, every later write is too, even one that would fit.
    EXPECT_EQ(write(sink, ""), PRAENORA_STATUS_RESOURCE_EXHAUSTED);
    EXPECT_EQ(sink.text(), "abcdefghij");
    EXPECT_EQ(sink.take(), "abcdefghij");
}

TEST(OutputSink, PassesWritesOnAndRefusesAtTheSameByte) {
    std::vector<std::string> passed;
    OutputSink sink(10, [&passed](const char* data, std::size_t size) {
        passed.emplace_back(data, size);
        return true;
    });
    EXPECT_EQ(write(sink, "abcd"), PRAENORA_STATUS_OK);
    EXPECT_EQ(write(sink, ""), PRAENORA_STATUS_OK);
    EXPECT_EQ(write(sink, "efghij"), PRAENORA_STATUS_OK);
    EXPECT_EQ(write(sink, "k"), PRAENORA_STATUS_RESOURCE_EXHAUSTED);
    EXPECT_EQ(write(sink, ""), PRAENORA_STATUS_RESOURCE_EXHAUSTED);
    EXPECT_EQ(passed, (std::vector<std::string>{"abcd", "efghij"}));
    // Nothing is kept here; the bytes count all the same.
    EXPECT_EQ(sink.text(), "");
    EXPECT_EQ(sink.size(), 10u);
    EXPECT_TRUE(sink.refused());
}

TEST(OutputSink, AWriteThatCannotBePassedOnIsRefused) {
    OutputSink sink(100, [](const char*, std::size_t) { return false; });
    EXPECT_EQ(write(sink, "abc"), PRAENORA_STATUS_RESOURCE_EXHAUSTED);
    EXPECT_TRUE(sink.refused());
    EXPECT_EQ(sink.size(), 0u);
}

TEST(OutputSink, RefusesNullDataWithASize) {
    OutputSink sink(10);
    praenora_sink* abi = sink.sink();
    EXPECT_EQ(abi->write(abi->context, nullptr, 3), PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(abi->write(abi->context, nullptr, 0), PRAENORA_STATUS_OK);
    EXPECT_FALSE(sink.refused());
}
