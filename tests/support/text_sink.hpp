// A praenora_sink that keeps the bytes it is given as text, for the test programs that call a
// provider or the Python core and print or parse what it wrote (tests/negative/python_start_probe,
// tests/providers/python_print). Header-only; it uses only the standard library and the SDK's C
// headers.
#ifndef PRAENORA_TESTS_SUPPORT_TEXT_SINK_HPP
#define PRAENORA_TESTS_SUPPORT_TEXT_SINK_HPP

#include <praenora/sdk/abi.h>

#include <cstddef>
#include <string>

namespace praenora::test {

class TextSink {
public:
    praenora_sink* get() noexcept { return &sink_; }
    const std::string& text() const noexcept { return text_; }

private:
    static praenora_status PRAENORA_CALL write(void* context, const char* data,
                                               std::size_t size) noexcept {
        try {
            static_cast<TextSink*>(context)->text_.append(data == nullptr ? "" : data, size);
        } catch (...) {
            return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
        }
        return PRAENORA_STATUS_OK;
    }

    std::string text_;
    praenora_sink sink_{this, &TextSink::write};
};

} // namespace praenora::test

#endif // PRAENORA_TESTS_SUPPORT_TEXT_SINK_HPP
