// A host-owned praenora_sink that appends what a module writes to a string, up to a size limit, or
// passes each write on (a module host's sinks pass them to the runtime over the channel).
#ifndef PRAENORA_RUNTIME_LOADER_OUTPUT_SINK_HPP
#define PRAENORA_RUNTIME_LOADER_OUTPUT_SINK_HPP

#include <praenora/sdk/abi.h>
#include <praenora/sdk/status.h>

#include <cstddef>
#include <functional>
#include <string>
#include <utility>

namespace praenora::detail {

// The bytes of every write are copied (or passed on) before write() returns, as the ABI promises
// the module. A write that would take the bytes accepted beyond `limit` is refused with
// PRAENORA_STATUS_RESOURCE_EXHAUSTED, and so is every later write, so a refused document never
// continues with a gap. Not thread-safe: one call writes to it at a time. It must stay where it is
// while a module holds sink() (it is neither copyable nor movable).
class OutputSink {
public:
    // Passes the bytes of an accepted write on; false when they cannot be (the write is then
    // refused as above). Must not throw.
    using Forward = std::function<bool(const char* data, std::size_t size)>;

    // Keeps what it accepts (text(), take()).
    explicit OutputSink(std::size_t limit) noexcept : limit_(limit) {
        sink_.context = this;
        sink_.write = &OutputSink::write;
    }
    // Passes what it accepts to `forward` and keeps nothing; the limit counts the bytes passed.
    OutputSink(std::size_t limit, Forward forward) noexcept
        : limit_(limit), forward_(std::move(forward)) {
        sink_.context = this;
        sink_.write = &OutputSink::write;
    }
    OutputSink(const OutputSink&) = delete;
    OutputSink& operator=(const OutputSink&) = delete;

    praenora_sink* sink() noexcept { return &sink_; }
    const std::string& text() const noexcept { return text_; }
    // The text kept, and the count of bytes accepted starts again from 0.
    std::string take() noexcept {
        size_ = 0;
        return std::exchange(text_, std::string());
    }
    std::size_t limit() const noexcept { return limit_; }
    // The bytes accepted so far.
    std::size_t size() const noexcept { return size_; }
    // True once a write was refused.
    bool refused() const noexcept { return refused_; }

private:
    static praenora_status PRAENORA_CALL write(void* context, const char* data,
                                               std::size_t size) noexcept {
        auto* self = static_cast<OutputSink*>(context);
        if (self == nullptr || (data == nullptr && size != 0)) {
            return PRAENORA_STATUS_INVALID_ARGUMENT;
        }
        if (self->refused_ || size > self->limit_ - self->size_) {
            self->refused_ = true;
            return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
        }
        if (size == 0) {
            return PRAENORA_STATUS_OK;
        }
        if (self->forward_) {
            if (!self->forward_(data, size)) {
                self->refused_ = true;
                return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
            }
        } else {
            try {
                self->text_.append(data, size);
            } catch (...) {
                self->refused_ = true;
                return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
            }
        }
        self->size_ += size;
        return PRAENORA_STATUS_OK;
    }

    praenora_sink sink_{};
    std::size_t limit_;
    Forward forward_;
    std::string text_;     // what was accepted, unless forwarded; never longer than limit_
    std::size_t size_ = 0; // the bytes accepted, kept or passed on; never more than limit_
    bool refused_ = false;
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_LOADER_OUTPUT_SINK_HPP
