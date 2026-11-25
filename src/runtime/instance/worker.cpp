#include "instance/worker.hpp"

#include "platform/platform.hpp"

#include <praenora/runtime/error.hpp>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <system_error>
#include <utility>

namespace praenora::detail {

// The state shared by the worker object and its thread.
struct InstanceWorker::Queue {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::function<void()>> tasks; // guarded by mutex
    bool stopping = false;                   // guarded by mutex; no more tasks will come
};

InstanceWorker::InstanceWorker(std::string name) : queue_(std::make_shared<Queue>()) {
    try {
        thread_ = ModuleThread([queue = queue_, name = std::move(name)]() mutable {
            run(std::move(queue), std::move(name));
        });
    } catch (const std::system_error& error) {
        throw Error(ErrorCode::RESOURCE_EXHAUSTED,
                    std::string("cannot start a module worker thread: ") + error.what());
    }
}

InstanceWorker::~InstanceWorker() { join(); }

void InstanceWorker::run(std::shared_ptr<Queue> queue, std::string name) {
    set_current_thread_name(name);
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue->mutex);
            queue->changed.wait(lock,
                                [&queue] { return queue->stopping || !queue->tasks.empty(); });
            if (queue->tasks.empty()) {
                return; // stopping, and nothing left to run
            }
            task = std::move(queue->tasks.front());
            queue->tasks.pop_front();
        }
        task();
    }
}

void InstanceWorker::post(std::function<void()> task) {
    {
        const std::lock_guard<std::mutex> lock(queue_->mutex);
        if (queue_->stopping) {
            throw Error(ErrorCode::INVALID_STATE, "the module worker thread has stopped");
        }
        queue_->tasks.push_back(std::move(task));
    }
    queue_->changed.notify_one();
}

void InstanceWorker::stop() noexcept {
    {
        const std::lock_guard<std::mutex> lock(queue_->mutex);
        queue_->stopping = true;
    }
    queue_->changed.notify_one();
}

void InstanceWorker::detach() noexcept {
    if (!thread_.joinable()) {
        return;
    }
    stop();
    thread_.detach();
    detached_ = true;
}

void InstanceWorker::join() noexcept {
    if (!thread_.joinable()) {
        return;
    }
    stop();
    try {
        thread_.join();
    } catch (const std::system_error&) {
        // Only when called on the worker thread itself, which never happens: let it go instead.
        thread_.detach();
        detached_ = true;
    }
}

bool InstanceWorker::detached() const noexcept { return detached_; }

} // namespace praenora::detail
