// The SDK's C++ headers compile in a module that includes <windows.h> without NOMINMAX before
// them: that header defines the function-like macros min and max. The standard headers come
// first, as they would be already, then the macros, then the SDK headers.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))

#include <praenora/sdk/module.hpp>
#include <praenora/sdk/testing.hpp>

namespace {

// A sink that collects what is written to it.
struct Collected {
    std::string text;
    praenora_sink sink{this, &Collected::write};

    static praenora_status PRAENORA_CALL write(void* context, const char* chunk, std::size_t size) {
        static_cast<Collected*>(context)->text.append(chunk, size);
        return PRAENORA_STATUS_OK;
    }
};

} // namespace

TEST(Adapter, CompilesWithWindowsMinAndMaxMacros) {
    // SinkWriter::append() takes as much as fits into its buffer: the code that uses std::min.
    Collected out;
    praenora::sdk::detail::SinkWriter writer(&out.sink);
    const std::string text(praenora::sdk::detail::kSinkChunkSize + 10, 'x');
    writer.append("ab", 2);
    writer.append(text);
    EXPECT_EQ(writer.finish(), PRAENORA_STATUS_OK);
    EXPECT_EQ(out.text, "ab" + text);
}
