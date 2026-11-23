// The runs behind the control API, over the public C++ API: SingleRun, the one run of
// `praenora run --listen` (run mode), and RunRegistry, the runs that `praenora serve` opens on
// request (serve mode), each driven on a thread of its own.
#ifndef PRAENORA_SERVER_RUN_REGISTRY_HPP
#define PRAENORA_SERVER_RUN_REGISTRY_HPP

#include "run_source.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/runtime.hpp>
#include <praenora/runtime/workflow.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace praenora::server {

// Run mode: the one run of the process, id "1", its run_number the workflow's, its configuration
// the configuration file's name and no output. The caller opened the workflow and drives it (on
// a thread of its own, as `praenora run` does); this source only queries it and requests its
// stop. It shares the workflow's ownership, so a handler that holds the run keeps it alive.
// create() and remove() throw NOT_FOUND: run mode opens and deletes no run.
class SingleRun final : public RunSource {
public:
    // `configuration_path`: the configuration file the workflow was opened from (only its file
    // name is reported); `created_at_unix_ns`: when it was opened.
    SingleRun(std::shared_ptr<Workflow> workflow, const std::string& configuration_path,
              std::int64_t created_at_unix_ns);

    ServerMode mode() const override { return ServerMode::run; }
    std::vector<std::shared_ptr<RunHandle>> list() const override;
    std::shared_ptr<RunHandle> find(const std::string& id) const override;
    std::shared_ptr<RunHandle> create(const RunRequest& request) override;
    std::shared_ptr<RunHandle> stop(const std::string& id) override;
    void remove(const std::string& id) override;

private:
    std::shared_ptr<RunHandle> run_;
};

// The settings of serve mode.
struct RegistrySettings {
    std::string configuration_root; // --config-root: an existing directory
    std::string output_root;        // --output-root: created with its parents when missing
    std::size_t max_runs = 16;      // --max-runs: runs held at once, running and finished
    // --isolation: every run's isolation; a request that asks for another is refused.
    std::optional<IsolationMode> isolation;
    // The runs' log level and format (the global options); each run logs to its own file.
    std::optional<LogLevel> log_level;
    std::optional<LogFormat> log_format;
    // Called on the requesting thread with k once Runtime::open() returned run k, before the run
    // is registered and started. Tests order a shutdown() against an open in progress with it;
    // the tool sets none.
    std::function<void(std::uint64_t)> after_open;
};

// Serve mode: opens runs on request with one Runtime and drives each on a thread of its own that
// calls Workflow::run(); queries and stop requests come from any thread. Run k (its id the decimal
// k) writes to <output-root>/run-<k>/, logs to <output-root>/run-<k>.log and keeps no status file;
// k starts after the highest run-<digits> or run-<digits>.log entry of the output root and grows
// with every request that reaches Runtime::open(), whether it succeeds or not. Entries are kept in
// shared pointers: a handler that found a run keeps it alive while remove() or shutdown() runs.
// One server per output root.
class RunRegistry final : public RunSource {
public:
    // Throws praenora::Error: INVALID_ARGUMENT for a configuration root that is not a directory or
    // a max_runs of 0, IO_ERROR when the output root cannot be created or read.
    RunRegistry(Runtime runtime, RegistrySettings settings);
    // shutdown(). Every handler that calls this source must have returned.
    ~RunRegistry() override;
    RunRegistry(const RunRegistry&) = delete;
    RunRegistry& operator=(const RunRegistry&) = delete;

    ServerMode mode() const override { return ServerMode::serve; }
    std::vector<std::shared_ptr<RunHandle>> list() const override;
    std::shared_ptr<RunHandle> find(const std::string& id) const override;
    // Checks the request, then opens run k and starts its thread. Throws praenora::Error:
    // INVALID_ARGUMENT for a configuration name that resolve_configuration() refuses and for an
    // isolation other than the forced one; NOT_FOUND for a configuration that does not exist;
    // RESOURCE_EXHAUSTED when max_runs runs are held; INVALID_STATE once shutdown() began and for
    // an output directory run-<k> that Runtime::open() refuses (another server's run); and the
    // errors of Runtime::open() (a validation failure carries its report in the details).
    std::shared_ptr<RunHandle> create(const RunRequest& request) override;
    std::shared_ptr<RunHandle> stop(const std::string& id) override;
    // Joins the run's thread, closes its workflow and forgets it; its files stay. INVALID_STATE
    // unless the run is completed, stopped or failed.
    void remove(const std::string& id) override;

    // Refuses new runs, asks every run to stop, waits for each one's thread to end (a run in a
    // long call ends at its own deadline, or when its module host is killed) and closes it.
    // Idempotent; the queries keep working afterwards.
    void shutdown();

    // The k that the next run takes.
    std::uint64_t next_run_number() const;

    // Whether shutdown() began: from then on, create() refuses every request.
    bool closing() const;

private:
    struct Entry;

    Runtime runtime_;
    RegistrySettings settings_;
    std::filesystem::path configuration_root_; // canonical
    std::filesystem::path output_root_;        // absolute
    mutable std::mutex mutex_;
    std::condition_variable idle_;                         // pending_ became 0
    std::map<std::uint64_t, std::shared_ptr<Entry>> runs_; // guarded by mutex_
    std::uint64_t next_ = 1;                               // guarded by mutex_
    std::size_t pending_ = 0;                              // guarded by mutex_: opens in progress
    bool closing_ = false;                                 // guarded by mutex_
};

// The configuration file that `name` names under the configuration root, whose canonical path is
// `canonical_root`, as a canonical path. `name` is a relative path with '/' between its segments:
// none of them empty, "." or "..", no '\', NUL or control character (and no ':' on Windows).
// The checks, in order, each throwing praenora::Error: INVALID_ARGUMENT for a name that breaks
// these rules; INVALID_ARGUMENT, with one message ("it does not resolve to a path inside the
// configuration root"), when the path with its symbolic links resolved as far as it exists
// (std::filesystem::weakly_canonical()) does not lie under `canonical_root` or cannot be resolved
// (a loop of links, a directory that cannot be searched), so a name beyond a link that leaves
// the root is refused whether or not anything exists there; NOT_FOUND for a path inside the root
// that does not exist; INVALID_ARGUMENT for one that is not a regular file. Messages name `name`,
// never an absolute path.
std::filesystem::path resolve_configuration(const std::filesystem::path& canonical_root,
                                            const std::string& name);

// What RunRegistry::create() answers when Runtime::open() threw `error` while it opened run `k`
// into `output`, the absolute path of <output-root>/run-<k>. Only an IO_ERROR with a location and
// without a validation report (carries_validation_report()) is looked at: for one located in
// `output` (both paths with their symbolic links resolved), INVALID_STATE naming run-<k> only
// (another server's run there, or something that is no directory), details
// {"output": "run-<k>"}; for one whose location or `output` cannot be resolved, an IO_ERROR of its
// own, details {"output", "cause": the error document of `error`}. Otherwise nullopt, and `error`
// is answered as it is (a validation failure as 422).
std::optional<Error> output_directory_refusal(const Error& error,
                                              const std::filesystem::path& output, std::uint64_t k);

// The k after the highest run-<digits> or run-<digits>.log entry of `output_root` (1 when there is
// none, or no such directory). Other entries are ignored. Throws praenora::Error(IO_ERROR) when
// the directory cannot be read.
std::uint64_t first_run_number(const std::filesystem::path& output_root);

} // namespace praenora::server

#endif // PRAENORA_SERVER_RUN_REGISTRY_HPP
