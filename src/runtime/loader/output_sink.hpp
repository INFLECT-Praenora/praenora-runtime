// A host-owned praenora_sink that appends what a module writes to a string, up to a size limit.
#ifndef PRAENORA_RUNTIME_LOADER_OUTPUT_SINK_HPP
#define PRAENORA_RUNTIME_LOADER_OUTPUT_SINK_HPP

#include <praenora/sdk/abi.h>
#include <praenora/sdk/status.h>

#include <cstddef>
#include <string>
#include <utility>

namespace praenora::detail {

// The bytes of every write are copied before write() returns, as the ABI promises the module.
// A write that would take the text beyond `limit` bytes is refused with
// PRAENORA_STATUS_RESOURCE_EXHAUSTED, and so is every later write, so a refused document never
// continues with a gap. Not thread-safe: one call writes to it at a time. It must stay where it
// is while a module holds sink() (it is neither copyable nor movable).
class OutputSink {
public:
    explicit OutputSink(std::size_t limit) noexcept : limit_(limit) {
        sink_.context = this;
        sink_.write = &OutputSink::write;
    }
    OutputSink(const OutputSink&) = delete;
    OutputSink& operator=(const OutputSink&) = delete;

    praenora_sink* sink() noexcept { return &sink_; }
    const std::string& text() const noexcept { return text_; }
    std::string take() noexcept { return std::exchange(text_, std::string()); }
    // True once a write was refused.
    bool refused() const noexcept { return refused_; }

private:
    static praenora_status PRAENORA_CALL write(void* context, const char* data,
                                               std::size_t size) noexcept {
        auto* self = static_cast<OutputSink*>(context);
        if (self == nullptr || (data == nullptr && size != 0)) {
            return PRAENORA_STATUS_INVALID_ARGUMENT;
        }
        if (self->refused_ || size > self->limit_ - self->text_.size()) {
            self->refused_ = true;
            return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
        }
        if (size == 0) {
            return PRAENORA_STATUS_OK;
        }
        try {
            self->text_.append(data, size);
        } catch (...) {
            self->refused_ = true;
            return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
        }
        return PRAENORA_STATUS_OK;
    }

    praenora_sink sink_{};
    std::size_t limit_;
    std::string text_; // never longer than limit_
    bool refused_ = false;
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_LOADER_OUTPUT_SINK_HPP
