// The gate of one loaded module library: it orders the calls into the library across every run and
// contract-check session of the process, as the manifest's features.concurrency asks, and keeps
// out the calls that would have to wait for a call that was abandoned (unresponsive) and may still
// be running in the library.
//
// One gate per image. LibraryGate::of() returns the gate of a loaded image
// (SharedLibrary::image()), shared by everything that holds that image in the process: every
// LoadedModule of it, whichever run or session loaded it, and every lease. A copy of a library at
// another path is another image, with globals of its own, and has a gate of its own.
//
// Leases. A call takes a lease before it starts and gives it back when the module returns:
// exclusive for a library whose instances may not be called concurrently (features.concurrency
// "module") and for self_test(), shared for one whose instances are independent ("instance").
// Requests are served in order of arrival: the first one waiting is granted once it fits beside the
// leases held (an exclusive one beside none, a shared one beside no exclusive one), and the next
// one is then looked at, so shared requests in a row are granted together. A shared request that
// arrives behind a waiting exclusive one waits for it.
//
// Abandonment. When its caller gives up on a call (the call is unresponsive), the lease is marked
// abandoned and stays held until the call returns, if ever. A request that conflicts with an
// abandoned lease (an exclusive one with any, a shared one with an exclusive one) is refused at
// once instead of waiting, and the refusal names the abandoned call. Once that call returns, its
// lease is released and the library can be called again.
//
// Threading. Thread-safe. A lease may be released on any thread, usually the worker that ran the
// call, and abandoned on another.
#ifndef PRAENORA_RUNTIME_LOADER_LIBRARY_GATE_HPP
#define PRAENORA_RUNTIME_LOADER_LIBRARY_GATE_HPP

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace praenora::detail {

// A call into a module library, as a refusal names it.
struct LibraryCall {
    std::string module; // name@version
    std::string call;   // create, initialize, invoke, finalize, destroy, diagnostics or self_test
    std::string by; // who made it: "step 'a' of workflow test.chain@0.1.0", "the contract checks"
};

// "invoke by step 'a' of workflow test.chain@0.1.0 (module fx-hang@0.1.0)".
std::string to_string(const LibraryCall& call);
// {"module": ..., "call": ..., "by": ...}.
nlohmann::json to_json(const LibraryCall& call);
// Why a call or a load is refused for the abandoned call `blocker`: "<to_string(blocker)> did not
// return within its deadline and the cancellation grace and may still be running in the library".
std::string refusal_reason(const LibraryCall& blocker);

class LibraryGate : public std::enable_shared_from_this<LibraryGate> {
public:
    enum class Access { shared, exclusive };

    class Lease;

    // What acquire() came to: a lease; or a refusal, naming the abandoned call the request would
    // have to wait for; or neither, when a stop ended the wait.
    struct Grant {
        std::shared_ptr<Lease> lease;
        std::optional<LibraryCall> refused_by;
    };

    // The gate of the loaded image `image`: the same gate for as long as anything holds it, a new
    // one after everything let go. The registry is never destroyed, so that this works at any
    // time, also while static objects are destroyed as the process exits.
    static std::shared_ptr<LibraryGate> of(const void* image);

    LibraryGate() = default;
    LibraryGate(const LibraryGate&) = delete;
    LibraryGate& operator=(const LibraryGate&) = delete;

    // Waits for the request's turn and grants it a lease for `call`. Refuses it at once, or while
    // it waits, as soon as it conflicts with an abandoned lease. With `stop`, the wait also ends
    // once `stop` is set (it is looked at every few milliseconds, since whoever sets it may not be
    // able to wake a waiting thread, a signal handler for one); a library that is free is granted
    // whatever `stop` says. The gate must be owned by a std::shared_ptr.
    Grant acquire(Access access, LibraryCall call, const std::atomic<bool>* stop = nullptr);

    // The abandoned call that a request for `access` would be refused for now, if any.
    std::optional<LibraryCall> refusal(Access access) const;

    // The number of requests waiting (for tests).
    std::size_t waiting() const;

private:
    struct Held {
        Access access;
        LibraryCall call;
        bool abandoned = false;
    };

    void release(std::uint64_t id) noexcept;
    void abandon(std::uint64_t id) noexcept;
    // Under the mutex: the abandoned lease that conflicts with `access`, if any; whether a request
    // for `access` fits beside the leases held.
    const Held* abandoned_conflict(Access access) const;
    bool fits(Access access) const;
    void leave_queue(std::uint64_t id) noexcept;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t next_id_ = 1;
    std::deque<std::pair<std::uint64_t, Access>> queue_; // waiting, in order of arrival
    std::map<std::uint64_t, Held> held_;
};

// A lease on a gate. Released by release(), or when it goes away; abandon() keeps it held until
// then. Both may be called more than once, in either order: an abandon() after the release does
// nothing.
class LibraryGate::Lease {
public:
    ~Lease();
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

    // The call returned: the lease is given back, abandoned or not.
    void release() noexcept;
    // The caller gave up on the call: the lease stays held, and refuses conflicting requests,
    // until release().
    void abandon() noexcept;

private:
    friend class LibraryGate;
    Lease(std::shared_ptr<LibraryGate> gate, std::uint64_t id) noexcept;

    std::shared_ptr<LibraryGate> gate_;
    std::uint64_t id_;
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_LOADER_LIBRARY_GATE_HPP
