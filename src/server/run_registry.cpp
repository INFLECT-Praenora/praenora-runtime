#include "run_registry.hpp"

#include "clock.hpp"
#include "control_api.hpp"

#include <frontend.hpp>

#include <praenora/runtime/error.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <system_error>
#include <thread>
#include <utility>

namespace praenora::server {

namespace fs = std::filesystem;

namespace {

std::string utf8_of(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

// The number that a run id names: decimal, without leading zeros, at most 2^53-1.
std::optional<std::uint64_t> id_number(const std::string& id) {
    if (id.empty() || id.size() > 16 || id.front() == '0') {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : id) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (value > frontend::kMaxCount) {
        return std::nullopt;
    }
    return value;
}

bool is_final(WorkflowState state) {
    return state == WorkflowState::completed || state == WorkflowState::stopped ||
           state == WorkflowState::failed;
}

// Whether `path` is `directory` or lies below it, comparing their components. Nothing lies
// under an empty directory.
bool lies_under(const fs::path& path, const fs::path& directory) {
    if (directory.empty() || path.empty()) {
        return false;
    }
    auto part = path.begin();
    for (const fs::path& expected : directory) {
        if (expected.empty()) {
            continue; // the empty last component of a path ending in a separator
        }
        if (part == path.end() || *part != expected) {
            return false;
        }
        ++part;
    }
    return true;
}

// `path` with its symbolic links resolved as far as it exists (std::filesystem::weakly_canonical),
// or nullopt when that fails (a loop of links, a directory that cannot be searched).
std::optional<fs::path> resolved(const fs::path& path) {
    std::error_code ec;
    fs::path result = fs::weakly_canonical(path, ec);
    if (ec || result.empty()) {
        return std::nullopt;
    }
    return result;
}

// The decimal number of a run-<digits> or run-<digits>.log name, if it is one and at most the
// largest count (2^53-1).
std::optional<std::uint64_t> run_number_of(const std::string& name) {
    constexpr std::string_view prefix = "run-";
    constexpr std::string_view log = ".log";
    std::string_view digits = name;
    if (digits.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }
    digits.remove_prefix(prefix.size());
    if (digits.size() > log.size() && digits.substr(digits.size() - log.size()) == log) {
        digits.remove_suffix(log.size());
    }
    if (digits.empty() || digits.size() > 16) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (value > frontend::kMaxCount) {
        return std::nullopt;
    }
    return value;
}

// A run of a Workflow, shared by the source that holds it and the handlers that found it.
class WorkflowRun final : public RunHandle {
public:
    WorkflowRun(std::shared_ptr<Workflow> workflow, RunFacts facts)
        : workflow_(std::move(workflow)), facts_(std::move(facts)) {}

    const RunFacts& facts() const override { return facts_; }
    StatusSnapshot status() const override { return workflow_->status(); }
    std::string status_json() const override {
        return workflow_->status_json(PathForm::placeholders);
    }
    std::string metrics_json() const override { return workflow_->metrics_json(); }
    std::string run_record_json() const override { return workflow_->run_record_json(); }
    std::string latest_outputs_json() const override { return workflow_->latest_outputs_json(); }
    void request_stop() noexcept override { workflow_->request_stop(); }

    Workflow& workflow() const { return *workflow_; }

private:
    std::shared_ptr<Workflow> workflow_;
    RunFacts facts_;
};

Error not_in_run_mode(const char* what) {
    return Error(ErrorCode::NOT_FOUND, std::string(what) +
                                           ": run mode serves its one run; praenora serve opens "
                                           "and deletes runs");
}

} // namespace

// ---- SingleRun --------------------------------------------------------------------------------

SingleRun::SingleRun(std::shared_ptr<Workflow> workflow, const std::string& configuration_path,
                     std::int64_t created_at_unix_ns) {
    if (workflow == nullptr) {
        throw Error(ErrorCode::INVALID_ARGUMENT, "SingleRun: no workflow");
    }
    RunFacts facts;
    facts.id = "1";
    facts.run_number = workflow->run_number();
    facts.configuration = utf8_of(frontend::path_from_utf8(configuration_path).filename());
    facts.created_at = format_rfc3339_utc(created_at_unix_ns);
    run_ = std::make_shared<WorkflowRun>(std::move(workflow), std::move(facts));
}

std::vector<std::shared_ptr<RunHandle>> SingleRun::list() const { return {run_}; }

std::shared_ptr<RunHandle> SingleRun::find(const std::string& id) const {
    return id == "1" ? run_ : nullptr;
}

std::shared_ptr<RunHandle> SingleRun::create(const RunRequest&) {
    throw not_in_run_mode("POST /v0/runs");
}

std::shared_ptr<RunHandle> SingleRun::stop(const std::string& id) {
    const std::shared_ptr<RunHandle> run = find(id);
    if (run) {
        run->request_stop();
    }
    return run;
}

void SingleRun::remove(const std::string&) { throw not_in_run_mode("DELETE /v0/runs/{id}"); }

// ---- RunRegistry ------------------------------------------------------------------------------

// One run of the registry: its handle, which handlers share, and the thread that drives it, which
// only the registry touches (under its mutex, or after taking the entry out of the map).
struct RunRegistry::Entry {
    std::shared_ptr<WorkflowRun> run;
    std::thread driver;
};

RunRegistry::RunRegistry(Runtime runtime, RegistrySettings settings)
    : runtime_(std::move(runtime)), settings_(std::move(settings)) {
    if (settings_.max_runs == 0) {
        throw Error(ErrorCode::INVALID_ARGUMENT, "--max-runs must be at least 1");
    }
    std::error_code ec;
    const fs::path configuration_root = frontend::path_from_utf8(settings_.configuration_root);
    if (!fs::is_directory(configuration_root, ec)) {
        throw Error(ErrorCode::INVALID_ARGUMENT,
                    "--config-root '" + settings_.configuration_root + "' is not a directory");
    }
    configuration_root_ = fs::canonical(configuration_root, ec);
    if (ec) {
        throw Error(ErrorCode::INVALID_ARGUMENT, "--config-root '" + settings_.configuration_root +
                                                     "' cannot be used: " + ec.message());
    }
    output_root_ =
        fs::absolute(frontend::path_from_utf8(settings_.output_root), ec).lexically_normal();
    if (!ec) {
        fs::create_directories(output_root_, ec);
    }
    if (ec || !fs::is_directory(output_root_)) {
        throw Error(ErrorCode::IO_ERROR,
                    "--output-root '" + settings_.output_root + "' cannot be created" +
                        (ec ? ": " + ec.message() : std::string(": it is not a directory")));
    }
    next_ = first_run_number(output_root_);
}

RunRegistry::~RunRegistry() { shutdown(); }

std::vector<std::shared_ptr<RunHandle>> RunRegistry::list() const {
    std::vector<std::shared_ptr<RunHandle>> runs;
    const std::lock_guard<std::mutex> lock(mutex_);
    runs.reserve(runs_.size());
    for (const auto& item : runs_) {
        runs.push_back(item.second->run);
    }
    return runs;
}

std::shared_ptr<RunHandle> RunRegistry::find(const std::string& id) const {
    const std::optional<std::uint64_t> number = id_number(id);
    if (!number) {
        return nullptr;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = runs_.find(*number);
    return found == runs_.end() ? nullptr : found->second->run;
}

std::shared_ptr<RunHandle> RunRegistry::create(const RunRequest& request) {
    const fs::path configuration =
        resolve_configuration(configuration_root_, request.configuration);
    if (settings_.isolation && request.isolation && *request.isolation != *settings_.isolation) {
        const std::string forced(
            frontend::name_of(frontend::kIsolationModes, *settings_.isolation, "in-process"));
        throw Error(ErrorCode::INVALID_ARGUMENT,
                    "'isolation': this server runs every run with --isolation " + forced +
                        "; leave it out or ask for " + forced);
    }
    std::uint64_t k = 0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (closing_) {
            throw Error(ErrorCode::INVALID_STATE, "the server is shutting down: no new run");
        }
        if (runs_.size() + pending_ >= settings_.max_runs) {
            throw Error(ErrorCode::RESOURCE_EXHAUSTED,
                        "the server holds " + std::to_string(settings_.max_runs) +
                            " run(s), its --max-runs; delete a finished one first");
        }
        k = next_++;
        ++pending_;
    }
    // However this ends, the open in progress is over.
    struct Pending {
        RunRegistry& registry;
        ~Pending() {
            {
                const std::lock_guard<std::mutex> lock(registry.mutex_);
                --registry.pending_;
            }
            registry.idle_.notify_all();
        }
    } pending{*this};

    const std::string output_name = "run-" + std::to_string(k);
    const fs::path output = output_root_ / output_name;
    WorkflowOptions options;
    options.seed = request.seed;
    options.ticks = request.ticks;
    options.trace_messages = request.trace_messages;
    options.isolation = settings_.isolation ? settings_.isolation : request.isolation;
    options.output_dir = utf8_of(output);
    options.log_file = utf8_of(output_root_ / (output_name + ".log"));
    options.status_file = ""; // none
    options.log_level = settings_.log_level;
    options.log_format = settings_.log_format;
    std::shared_ptr<Workflow> workflow;
    try {
        workflow = runtime_.open(utf8_of(configuration), options);
    } catch (const Error& error) {
        if (std::optional<Error> refusal = output_directory_refusal(error, output, k)) {
            throw *refusal;
        }
        throw;
    }
    if (settings_.after_open) {
        settings_.after_open(k);
    }

    RunFacts facts;
    facts.id = std::to_string(k);
    facts.run_number = workflow->run_number();
    facts.configuration = request.configuration;
    facts.created_at = format_rfc3339_utc(now_unix_ns());
    facts.output = output_name;
    auto entry = std::make_shared<Entry>();
    entry->run = std::make_shared<WorkflowRun>(std::move(workflow), std::move(facts));
    const std::lock_guard<std::mutex> lock(mutex_);
    if (closing_) {
        // shutdown() began while the run was opened: it is closed unstarted (the workflow's
        // destructor), and the request refused.
        throw Error(ErrorCode::INVALID_STATE, "the server is shutting down: no new run");
    }
    // Registered first, started last: should either throw, no joinable thread is left behind.
    const std::shared_ptr<WorkflowRun> run = entry->run;
    runs_.emplace(k, entry);
    try {
        entry->driver = std::thread([run] {
            try {
                run->workflow().run();
            } catch (...) {
                // The failure is the run's: its state, status and run record say what it was.
            }
        });
    } catch (...) {
        runs_.erase(k); // the workflow is closed unstarted when the last handle goes
        throw;
    }
    return run;
}

std::shared_ptr<RunHandle> RunRegistry::stop(const std::string& id) {
    const std::shared_ptr<RunHandle> run = find(id);
    if (run) {
        run->request_stop();
    }
    return run;
}

void RunRegistry::remove(const std::string& id) {
    const std::optional<std::uint64_t> number = id_number(id);
    std::shared_ptr<Entry> entry;
    std::thread driver;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = number ? runs_.find(*number) : runs_.end();
        if (it == runs_.end()) {
            throw Error(ErrorCode::NOT_FOUND, "there is no run '" + id + "'");
        }
        const WorkflowState state = it->second->run->workflow().state();
        if (!is_final(state)) {
            throw Error(ErrorCode::INVALID_STATE,
                        "run " + id + " is " + to_string(state) +
                            ": only a completed, stopped or failed run is deleted; stop it first");
        }
        entry = it->second;
        // A thread is joined once, by whoever takes it out under the mutex (this, or
        // shutdown()).
        driver = std::move(entry->driver);
        runs_.erase(it);
    }
    // Its state is final: run() is returning, if it has not yet.
    if (driver.joinable()) {
        driver.join();
    }
    entry->run->workflow().close();
}

void RunRegistry::shutdown() {
    std::vector<std::shared_ptr<Entry>> entries;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        closing_ = true;
        idle_.wait(lock, [this] { return pending_ == 0; });
        for (const auto& item : runs_) {
            entries.push_back(item.second);
        }
    }
    for (const auto& entry : entries) {
        entry->run->request_stop();
    }
    for (const auto& entry : entries) {
        // A thread is joined once, by whoever takes it out under the mutex (this, or remove()).
        std::thread driver;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            driver = std::move(entry->driver);
        }
        if (driver.joinable()) {
            driver.join();
        }
        entry->run->workflow().close();
    }
}

std::uint64_t RunRegistry::next_run_number() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return next_;
}

bool RunRegistry::closing() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return closing_;
}

std::optional<Error> output_directory_refusal(const Error& error, const fs::path& output,
                                              std::uint64_t k) {
    // A validation failure is answered as it is (422), whatever its first diagnostic locates: an
    // output directory is never one of its problems.
    if (error.code() != ErrorCode::IO_ERROR || error.location().file.empty() ||
        carries_validation_report(error)) {
        return std::nullopt;
    }
    const std::string output_name = "run-" + std::to_string(k);
    const std::optional<fs::path> located =
        resolved(frontend::path_from_utf8(error.location().file));
    const std::optional<fs::path> directory = resolved(output);
    if (!located || !directory) {
        // Where the error lies cannot be told: say so, rather than blame another server.
        return Error(ErrorCode::IO_ERROR,
                     "run " + std::to_string(k) +
                         ": the run cannot be opened, and whether the error lies in its output "
                         "directory " +
                         output_name + " cannot be told: a path cannot be resolved",
                     {},
                     nlohmann::json{{"output", output_name},
                                    {"cause", nlohmann::json::parse(error.to_json())}}
                         .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    }
    if (!lies_under(*located, *directory)) {
        return std::nullopt;
    }
    // Another server's run in that directory, or something there that is no directory. The
    // message names the directory by its name under the output root, as the entry does.
    return Error(ErrorCode::INVALID_STATE,
                 "the output directory " + output_name +
                     " holds an earlier run, or is not a directory: run " + std::to_string(k) +
                     " cannot use it; is another server using this output root?",
                 {}, nlohmann::json{{"output", output_name}}.dump());
}

fs::path resolve_configuration(const fs::path& canonical_root, const std::string& name) {
    const auto refused = [&name](const std::string& why) {
        return Error(ErrorCode::INVALID_ARGUMENT, "configuration '" + name + "': " + why);
    };
    if (name.empty()) {
        throw refused("the name is empty");
    }
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7f) {
            throw refused("the name holds a control character");
        }
        if (c == '\\') {
            throw refused("the name holds a '\\'; separate its segments with '/'");
        }
#ifdef _WIN32
        if (c == ':') {
            throw refused("the name holds a ':'");
        }
#endif
    }
    fs::path relative;
    std::size_t begin = 0;
    while (true) {
        const std::size_t end = name.find('/', begin);
        const std::string segment =
            name.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (segment.empty() || segment == "." || segment == "..") {
            throw refused(
                "it must be a relative path with '/' between its segments, none of them empty, "
                "'.' or '..'");
        }
        relative /= frontend::path_from_utf8(segment);
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    // A symbolic link may lead anywhere. Containment is checked first, on the path with its links
    // resolved as far as it exists: a name beyond a link that leaves the root is refused whether
    // or not its target exists, and whatever stops the resolution (a loop of links, a directory
    // that cannot be searched), with one message, so nothing outside the root can be probed.
    const auto outside = [&] {
        return refused("it does not resolve to a path inside the configuration root");
    };
    const std::optional<fs::path> beyond = resolved(canonical_root / relative);
    if (!beyond || !lies_under(*beyond, canonical_root)) {
        throw outside();
    }
    std::error_code ec;
    if (!fs::exists(*beyond, ec)) {
        throw Error(ErrorCode::NOT_FOUND,
                    "configuration '" + name + "' does not exist under the configuration root");
    }
    const fs::path canonical = fs::canonical(*beyond, ec);
    if (ec || !lies_under(canonical, canonical_root)) {
        throw outside();
    }
    if (!fs::is_regular_file(canonical, ec)) {
        throw refused("it is not a file");
    }
    return canonical;
}

std::uint64_t first_run_number(const fs::path& output_root) {
    std::error_code ec;
    if (!fs::is_directory(output_root, ec)) {
        return 1;
    }
    std::uint64_t highest = 0;
    fs::directory_iterator it(output_root, ec);
    for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (const auto number = run_number_of(utf8_of(it->path().filename()))) {
            highest = std::max(highest, *number);
        }
    }
    if (ec) {
        throw Error(ErrorCode::IO_ERROR, "the output root cannot be read: " + ec.message());
    }
    return highest + 1;
}

} // namespace praenora::server
