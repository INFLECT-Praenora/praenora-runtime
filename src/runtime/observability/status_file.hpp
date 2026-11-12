// The status file: a status snapshot kept on disk for other processes (praenora status <file>),
// replaced atomically so a reader never sees a partial file.
#ifndef PRAENORA_RUNTIME_OBSERVABILITY_STATUS_FILE_HPP
#define PRAENORA_RUNTIME_OBSERVABILITY_STATUS_FILE_HPP

#include <praenora/runtime/error.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace praenora::detail {

// Writes `producer()` to `file` with write_file_atomically() (which creates missing parent
// directories):
//   - once in the constructor, before it returns;
//   - after notify() (several notifications before a write make one write);
//   - every `interval` after the previous write (never when the interval is 0 or less);
//   - once more in the destructor, after the background thread has stopped.
// The writes after construction run on a background thread named praenora:status, which calls
// `producer` (and `on_error`), so both must be thread-safe. A failed write, or a producer that
// throws, is never thrown: it goes to `on_error` (when given) as a praenora::Error (IO_ERROR for
// writes, INTERNAL for a producer's exception), and the writer keeps going.
class StatusFileWriter {
public:
    using ErrorHandler = std::function<void(const Error&)>;

    StatusFileWriter(std::filesystem::path file, std::chrono::nanoseconds interval,
                     std::function<std::string()> producer, ErrorHandler on_error = {});
    ~StatusFileWriter();
    StatusFileWriter(const StatusFileWriter&) = delete;
    StatusFileWriter& operator=(const StatusFileWriter&) = delete;

    // Asks for a write soon. Thread-safe; never throws.
    void notify() noexcept;

private:
    void run();
    void write() noexcept;

    const std::filesystem::path file_;
    const std::chrono::nanoseconds interval_;
    const std::function<std::string()> producer_;
    const ErrorHandler on_error_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool notified_ = false, stopping_ = false;
    std::thread thread_;
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_OBSERVABILITY_STATUS_FILE_HPP
