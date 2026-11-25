// The dedicated thread of one module instance: every call into the instance runs there, one at a
// time, in the order posted. Its stack is a main thread's (platform/platform.hpp, ModuleThread).
#ifndef PRAENORA_RUNTIME_INSTANCE_WORKER_HPP
#define PRAENORA_RUNTIME_INSTANCE_WORKER_HPP

#include "platform/platform.hpp"

#include <functional>
#include <memory>
#include <string>

namespace praenora::detail {

class InstanceWorker {
public:
    // Starts the thread and names it `name` where the platform allows (for example
    // "praenora:estimate"). Throws praenora::Error(RESOURCE_EXHAUSTED) when no thread can be
    // started.
    explicit InstanceWorker(std::string name);
    // Joins the thread (see join()); a detached worker is left alone.
    ~InstanceWorker();
    InstanceWorker(const InstanceWorker&) = delete;
    InstanceWorker& operator=(const InstanceWorker&) = delete;

    // Queues `task` for the thread. A task must not throw, and must own (or share) everything it
    // uses: after detach() it may run after this object is gone. Throws
    // praenora::Error(INVALID_STATE) once the worker is joined or detached.
    void post(std::function<void()> task);

    // Lets the thread go. It finishes its current task, which may never end, then the tasks still
    // queued, and exits on its own; nothing waits for it. The queue it uses is shared with it, so
    // it stays valid after this object is destroyed.
    void detach() noexcept;

    // Waits until the queued tasks have run and the thread has exited. Does nothing for a
    // detached worker or one already joined.
    void join() noexcept;

    bool detached() const noexcept;

private:
    struct Queue;
    static void run(std::shared_ptr<Queue> queue, std::string name);
    void stop() noexcept;

    std::shared_ptr<Queue> queue_;
    ModuleThread thread_;
    bool detached_ = false;
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_INSTANCE_WORKER_HPP
