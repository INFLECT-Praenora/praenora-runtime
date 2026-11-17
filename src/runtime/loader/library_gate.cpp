#include "loader/library_gate.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <utility>

namespace praenora::detail {

namespace {

// How often a wait with a stop flag looks at it.
constexpr std::chrono::milliseconds kStopPoll{10};

} // namespace

std::string to_string(const LibraryCall& call) {
    return call.call + " by " + call.by + " (module " + call.module + ")";
}

nlohmann::json to_json(const LibraryCall& call) {
    return {{"module", call.module}, {"call", call.call}, {"by", call.by}};
}

std::string refusal_reason(const LibraryCall& blocker) {
    return to_string(blocker) +
           " did not return within its deadline and the cancellation grace and may still be "
           "running in the library";
}

std::shared_ptr<LibraryGate> LibraryGate::of(const void* image) {
    // Never destroyed, so that of() works at any time, also while static objects are destroyed as
    // the process exits.
    static auto* const mutex = new std::mutex;
    static auto* const gates = new std::map<const void*, std::weak_ptr<LibraryGate>>;
    const std::lock_guard<std::mutex> lock(*mutex);
    for (auto it = gates->begin(); it != gates->end();) {
        it = it->second.expired() ? gates->erase(it) : std::next(it);
    }
    std::weak_ptr<LibraryGate>& slot = (*gates)[image];
    if (auto gate = slot.lock()) {
        return gate;
    }
    auto gate = std::make_shared<LibraryGate>();
    slot = gate;
    return gate;
}

LibraryGate::Grant LibraryGate::acquire(Access access, LibraryCall call,
                                        const std::atomic<bool>* stop) {
    // The lease is allocated before the request joins the queue, so that nothing can fail between
    // granting a lease and handing it out: a held entry that no lease releases would keep the
    // library for good. It gets its id under the lock; until then, and as long as it is not
    // granted, releasing it finds nothing to release.
    std::shared_ptr<Lease> lease(new Lease(shared_from_this(), 0));
    std::unique_lock<std::mutex> lock(mutex_);
    const std::uint64_t id = next_id_++;
    lease->id_ = id;
    queue_.emplace_back(id, access);
    // The request leaves the queue on every way out but a grant, an exception included, so that
    // the requests behind it are not kept waiting for ever.
    struct Leave {
        LibraryGate& gate;
        std::uint64_t id;
        bool queued = true;
        ~Leave() {
            if (queued) {
                gate.leave_queue(id);
            }
        }
    } leave{*this, id};
    for (;;) {
        if (const Held* blocker = abandoned_conflict(access)) {
            Grant grant;
            grant.refused_by = blocker->call;
            return grant;
        }
        if (queue_.front().first == id && fits(access)) {
            held_.emplace(id, Held{access, std::move(call), false});
            queue_.pop_front();
            leave.queued = false;
            changed_.notify_all(); // the next one may fit too
            Grant grant;
            grant.lease = std::move(lease);
            return grant;
        }
        if (stop != nullptr && stop->load()) {
            return Grant{};
        }
        if (stop != nullptr) {
            changed_.wait_for(lock, kStopPoll);
        } else {
            changed_.wait(lock);
        }
    }
}

std::optional<LibraryCall> LibraryGate::refusal(Access access) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (const Held* blocker = abandoned_conflict(access)) {
        return blocker->call;
    }
    return std::nullopt;
}

std::size_t LibraryGate::waiting() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void LibraryGate::release(std::uint64_t id) noexcept {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (held_.erase(id) == 0) {
            return;
        }
    }
    changed_.notify_all();
}

void LibraryGate::abandon(std::uint64_t id) noexcept {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto held = held_.find(id);
        if (held == held_.end()) {
            return; // the call returned first
        }
        held->second.abandoned = true;
    }
    changed_.notify_all(); // the requests waiting for it are refused now
}

const LibraryGate::Held* LibraryGate::abandoned_conflict(Access access) const {
    for (const auto& entry : held_) {
        const Held& held = entry.second;
        if (held.abandoned && (access == Access::exclusive || held.access == Access::exclusive)) {
            return &held;
        }
    }
    return nullptr;
}

bool LibraryGate::fits(Access access) const {
    if (access == Access::exclusive) {
        return held_.empty();
    }
    return std::none_of(held_.begin(), held_.end(),
                        [](const auto& entry) { return entry.second.access == Access::exclusive; });
}

void LibraryGate::leave_queue(std::uint64_t id) noexcept {
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                [id](const auto& request) { return request.first == id; }),
                 queue_.end());
    changed_.notify_all(); // the request behind it may be first now
}

LibraryGate::Lease::Lease(std::shared_ptr<LibraryGate> gate, std::uint64_t id) noexcept
    : gate_(std::move(gate)), id_(id) {}

LibraryGate::Lease::~Lease() { release(); }

void LibraryGate::Lease::release() noexcept { gate_->release(id_); }

void LibraryGate::Lease::abandon() noexcept { gate_->abandon(id_); }

} // namespace praenora::detail
