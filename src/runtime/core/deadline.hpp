// Deadlines on std::chrono::steady_clock without overflow: a wait added to a reading of the clock
// saturates at the clock's last instant instead of overflowing (a signed overflow, which would be
// undefined, and in practice a time long past). Every deadline of a call, of a module host's start
// and end, and of a write to its channel is made this way, so that a wait without limit (a timeout
// of `none`, passed on as the longest duration) stays a wait without limit.
#ifndef PRAENORA_RUNTIME_CORE_DEADLINE_HPP
#define PRAENORA_RUNTIME_CORE_DEADLINE_HPP

#include <chrono>

namespace praenora::detail {

// The instant `wait` after `start`, or std::chrono::steady_clock::time_point::max() when that lies
// beyond the clock's range; `start` itself for a wait of 0 or less.
inline std::chrono::steady_clock::time_point
deadline_after(std::chrono::steady_clock::time_point start,
               std::chrono::nanoseconds wait) noexcept {
    using Clock = std::chrono::steady_clock;
    if (wait <= std::chrono::nanoseconds::zero()) {
        return start;
    }
    // In the clock's own unit (nanoseconds wherever the runtime is built), rounded down: never a
    // longer wait than asked for, and never a conversion that overflows.
    const Clock::duration step = std::chrono::duration_cast<Clock::duration>(wait);
    if (start < Clock::time_point()) {
        return start + step; // before the epoch: any wait fits
    }
    return step >= Clock::time_point::max() - start ? Clock::time_point::max() : start + step;
}

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_CORE_DEADLINE_HPP
