#include "observability/status_file.hpp"

#include "core/paths.hpp"
#include "platform/platform.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace praenora::detail {

namespace {

// Longer intervals are waited in steps of this, so the deadline cannot overflow the clock.
constexpr std::chrono::nanoseconds kLongestWait = std::chrono::hours(24);

} // namespace

StatusFileWriter::StatusFileWriter(std::filesystem::path file, std::chrono::nanoseconds interval,
                                   std::function<std::string()> producer, ErrorHandler on_error)
    : file_(std::move(file)), interval_(interval), producer_(std::move(producer)),
      on_error_(std::move(on_error)) {
    write();
    thread_ = std::thread([this] { run(); });
}

StatusFileWriter::~StatusFileWriter() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    write();
}

void StatusFileWriter::notify() noexcept {
    try {
        const std::lock_guard<std::mutex> lock(mutex_);
        notified_ = true;
    } catch (...) {
        return; // the next periodic write (or the final one) still comes
    }
    wake_.notify_one();
}

void StatusFileWriter::run() {
    set_current_thread_name("praenora:status");
    try {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto due = [this] { return notified_ || stopping_; };
        while (!stopping_) {
            if (interval_.count() > 0) {
                // Wait out the interval, or until notified or stopped.
                auto left = interval_;
                while (!due() && left.count() > 0) {
                    const auto step = std::min(left, kLongestWait);
                    const auto start = std::chrono::steady_clock::now();
                    if (wake_.wait_for(lock, step, due)) {
                        break;
                    }
                    left -= std::max(std::chrono::steady_clock::now() - start,
                                     std::chrono::steady_clock::duration(step));
                }
            } else {
                wake_.wait(lock, due);
            }
            if (stopping_) {
                break;
            }
            notified_ = false;
            lock.unlock();
            write();
            lock.lock();
        }
    } catch (...) {
        // Waiting failed (a system error): the destructor still makes the final write.
    }
}

void StatusFileWriter::write() noexcept {
    try {
        std::string text;
        try {
            text = producer_();
        } catch (const Error&) {
            throw;
        } catch (const std::exception& e) {
            throw Error(ErrorCode::INTERNAL,
                        std::string("cannot produce the status snapshot: ") + e.what());
        } catch (...) {
            throw Error(ErrorCode::INTERNAL, "cannot produce the status snapshot");
        }
        write_file_atomically(file_, text);
    } catch (const Error& error) {
        if (on_error_) {
            try {
                on_error_(error);
            } catch (...) {
                // The handler's own failure has nowhere to go.
            }
        }
    } catch (...) {
        // Allocation failure while reporting: nothing more can be done.
    }
}

} // namespace praenora::detail
