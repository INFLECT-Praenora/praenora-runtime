// Where the control API finds its runs: the interface RunSource, which RunRegistry implements for
// both modes (run_registry.hpp) and tests implement with fakes. Every member of RunSource and
// RunHandle may be called from any thread at any time.
#ifndef PRAENORA_SERVER_RUN_SOURCE_HPP
#define PRAENORA_SERVER_RUN_SOURCE_HPP

#include <praenora/runtime/options.hpp>
#include <praenora/runtime/status.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace praenora::server {

// run: `praenora run --listen`, the one run of the process; serve: `praenora serve`, which opens
// runs on request.
enum class ServerMode { run, serve };

// The body of POST /v0/runs, checked: the configuration's name under the configuration root (a
// relative path with '/') and the run's overrides.
struct RunRequest {
    std::string configuration;
    std::optional<std::uint64_t> seed, ticks;
    std::optional<bool> trace_messages;
    std::optional<IsolationMode> isolation;
};

// What the control API reports of a run beside its status, fixed when it is opened.
struct RunFacts {
    std::string id;               // the run id: decimal, without leading zeros
    std::uint64_t run_number = 0; // Workflow::run_number()
    // Run mode: the configuration file's name; serve mode: its name under the configuration root,
    // with '/'. Never an absolute path.
    std::string configuration;
    std::string created_at;            // RFC 3339: when the server opened the run
    std::optional<std::string> output; // serve mode: "run-<k>"; run mode: none
};

// One run that a RunSource holds. Holding the handle keeps the run's workflow alive, also after
// the source removed it.
class RunHandle {
public:
    virtual ~RunHandle() = default;

    virtual const RunFacts& facts() const = 0;
    virtual StatusSnapshot status() const = 0;
    // The status snapshot as a status-snapshot document, its error records naming the run's
    // directories by placeholders (PathForm::placeholders).
    virtual std::string status_json() const = 0;
    virtual std::string metrics_json() const = 0;
    // The run record as last written; "null" before it exists.
    virtual std::string run_record_json() const = 0;
    virtual std::string latest_outputs_json() const = 0;
    virtual void request_stop() noexcept = 0;
};

class RunSource {
public:
    virtual ~RunSource() = default;

    virtual ServerMode mode() const = 0;
    // Every run held, in id order (numerically).
    virtual std::vector<std::shared_ptr<RunHandle>> list() const = 0;
    // The run `id`, or nullptr.
    virtual std::shared_ptr<RunHandle> find(const std::string& id) const = 0;
    // Opens and starts a run. Throws praenora::Error: NOT_FOUND in run mode, and whatever refuses
    // the request (see RunRegistry::create()).
    virtual std::shared_ptr<RunHandle> create(const RunRequest& request) = 0;
    // Asks the run `id` to stop (idempotent) and returns it; nullptr when there is none.
    virtual std::shared_ptr<RunHandle> stop(const std::string& id) = 0;
    // Forgets the finished run `id`, its files kept. Throws praenora::Error: NOT_FOUND when there
    // is none (and always in run mode), INVALID_STATE while it runs.
    virtual void remove(const std::string& id) = 0;
};

} // namespace praenora::server

#endif // PRAENORA_SERVER_RUN_SOURCE_HPP
