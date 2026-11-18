// The process-wide lock with which a provider of praenora.model-conversion@0 serializes its
// translations, and the cancellable wait for it that docs/specs/model-conversion-service.md
// prescribes: a call polls the calling instance's cancel_requested() once before it takes the
// lock and, while it waits, again every kPollInterval. Used by the provider antimony
// (src/providers/antimony) and the fixture provider test-conversion
// (tests/fixtures/providers/test-conversion), so that the checks that test-conversion's held
// conversions make of the wait test the code antimony runs too. The code is shared, the lock is
// not: each provider library has its own mutex.
//
// Internal, header-only C++17 with the module SDK's C headers only; never installed.
#ifndef PRAENORA_PROVIDERS_COMMON_CANCELLABLE_LOCK_HPP
#define PRAENORA_PROVIDERS_COMMON_CANCELLABLE_LOCK_HPP

#include <praenora/sdk/abi.h>

#include <chrono>
#include <cstddef>
#include <mutex>

namespace praenora::providers {

// How long a call waits for the lock between two polls of cancel_requested(); the service allows
// at most 50 ms.
constexpr std::chrono::milliseconds kPollInterval{20};

// Whether `host` can be polled: it is not NULL, its struct_size covers cancel_requested, and that
// callback is set.
inline bool can_poll(const praenora_host_api* host) noexcept {
    return host != nullptr &&
           host->struct_size >=
               offsetof(praenora_host_api, cancel_requested) + sizeof host->cancel_requested &&
           host->cancel_requested != nullptr;
}

// Whether the host asks the call to stop; false for a host that cannot be polled.
inline bool cancel_requested(const praenora_host_api* host) noexcept {
    return can_poll(host) && host->cancel_requested(host->context) != 0;
}

// Holds `mutex`, the provider's process-wide lock, for a scope. The constructor polls `host` once,
// then waits for the lock, polling `host` again after each kPollInterval it waited; once the host
// asks the call to stop, it gives up, and held() is false: the caller then ends the call with
// CANCELLED. A host that cannot be polled waits without polling. A call that finds the lock free
// polls exactly once.
class CancellableLock {
public:
    CancellableLock(std::timed_mutex& mutex, const praenora_host_api* host) : mutex_(mutex) {
        if (!can_poll(host)) {
            mutex_.lock();
            held_ = true;
            return;
        }
        if (cancel_requested(host)) {
            return;
        }
        while (!mutex_.try_lock_for(kPollInterval)) {
            if (cancel_requested(host)) {
                return;
            }
        }
        held_ = true;
    }
    CancellableLock(const CancellableLock&) = delete;
    CancellableLock& operator=(const CancellableLock&) = delete;
    ~CancellableLock() {
        if (held_) {
            mutex_.unlock();
        }
    }

    // Whether the lock is held: false when the host asked the call to stop first.
    bool held() const noexcept { return held_; }

private:
    std::timed_mutex& mutex_;
    bool held_ = false;
};

} // namespace praenora::providers

#endif // PRAENORA_PROVIDERS_COMMON_CANCELLABLE_LOCK_HPP
