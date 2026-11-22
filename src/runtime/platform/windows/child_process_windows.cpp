// isolation/child_process.hpp on Windows: process isolation is not available in this version, so
// nothing is ever started.
#include "isolation/child_process.hpp"

#include "core/paths.hpp"

#include <praenora/runtime/error.hpp>

#include <nlohmann/json.hpp>

namespace praenora::detail {

std::string signal_name(int signal) { return "signal " + std::to_string(signal); }

std::string describe(const ChildExit& exit) {
    if (exit.signal) {
        return "was ended by signal " + std::to_string(*exit.signal);
    }
    if (exit.exit_code) {
        return "exited with code " + std::to_string(*exit.exit_code);
    }
    return "ended";
}

bool process_isolation_available() noexcept { return false; }

ChildProcess::Started ChildProcess::start(const std::filesystem::path& executable,
                                          const std::vector<std::string>&) {
    throw Error(ErrorCode::MODULE_HOST_UNAVAILABLE,
                "cannot start the module host '" + path_to_utf8(executable) +
                    "': process isolation is not available on Windows in this version",
                {}, nlohmann::json{{"reason", "spawn"}}.dump());
}

ChildProcess::ChildProcess(std::int64_t pid) noexcept : pid_(pid) {}

ChildProcess::~ChildProcess() = default;

std::int64_t ChildProcess::pid() const noexcept { return pid_; }

void ChildProcess::terminate() noexcept {}

void ChildProcess::poll_locked() {}

std::optional<ChildExit> ChildProcess::wait(std::optional<std::chrono::nanoseconds>) {
    return ChildExit{};
}

std::optional<ChildExit> ChildProcess::exit_status() const { return ChildExit{}; }

} // namespace praenora::detail
