// The service checks of praenora.simulation@0 (simulation_service_checks.hpp). Every check uses
// only the table, the parameter set and the rules of docs/specs/simulation-service.md, so that any
// provider of the service can be checked with a parameter set of its own.
#include "simulation_service_checks.hpp"

#include "instance/services.hpp"
#include "provider/provider_loader.hpp"
#include "provider/provider_manifest.hpp"

#include <praenora/runtime/error.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace praenora::test {

void PrintTo(const SimulationCase& simulation_case, std::ostream* out) {
    *out << simulation_case.name << " (" << simulation_case.provider_manifest.filename().u8string()
         << ", " << simulation_case.model_format << ")";
}

std::string simulation_case_name(const ::testing::TestParamInfo<SimulationCase>& info) {
    return info.param.name;
}

void SimulationService::SetUp() {
    const SimulationCase& c = GetParam();
    try {
        const detail::ProviderManifest manifest =
            detail::load_provider_manifest(c.provider_manifest);
        const detail::LoadedProvider& provider = detail::load_provider(
            manifest,
            {detail::ServiceVersion{PRAENORA_SIMULATION_SERVICE, PRAENORA_SIMULATION_SERVICE_MAJOR,
                                    PRAENORA_SIMULATION_SERVICE_MINOR}},
            std::nullopt, nullptr);
        table_ = static_cast<const praenora_simulation_v0*>(
            provider.service(PRAENORA_SIMULATION_SERVICE, PRAENORA_SIMULATION_SERVICE_MAJOR));
    } catch (const Error& error) {
        FAIL() << "the provider of " << c.provider_manifest.u8string()
               << " does not load: " << error.what() << " " << error.details_json();
    }
    ASSERT_NE(table_, nullptr) << "the provider offers no praenora.simulation@0 table";
}

namespace {

using nlohmann::json;

constexpr const char* kService = PRAENORA_SIMULATION_SERVICE;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ---------------------------------------------------------------------------------------------
// Sinks and a host API for the checks.

// A sink that collects what is written to it, or refuses every write with `refusal`.
class TextSink {
public:
    explicit TextSink(praenora_status refusal = PRAENORA_STATUS_OK) : refusal_(refusal) {}
    TextSink(const TextSink&) = delete;
    TextSink& operator=(const TextSink&) = delete;

    praenora_sink* get() noexcept { return &sink_; }
    const std::string& text() const noexcept { return text_; }

private:
    static praenora_status PRAENORA_CALL write(void* context, const char* data,
                                               std::size_t size) noexcept {
        auto* self = static_cast<TextSink*>(context);
        if (self->refusal_ != PRAENORA_STATUS_OK) {
            return self->refusal_;
        }
        if (data == nullptr && size != 0) {
            return PRAENORA_STATUS_INVALID_ARGUMENT;
        }
        try {
            self->text_.append(data == nullptr ? "" : data, size);
        } catch (...) {
            return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
        }
        return PRAENORA_STATUS_OK;
    }

    praenora_status refusal_;
    std::string text_;
    praenora_sink sink_{this, &TextSink::write};
};

// A host API that records log messages and lets a check decide what cancel_requested() reports:
// nonzero from its n-th call on, and optionally waiting in one call until the check releases it.
// Thread-safe.
class CheckHost {
public:
    CheckHost() {
        api_.struct_size = static_cast<std::uint32_t>(sizeof(praenora_host_api));
        api_.abi_major = PRAENORA_ABI_MAJOR;
        api_.abi_minor = PRAENORA_ABI_MINOR;
        api_.context = this;
        api_.log = &CheckHost::log;
        api_.log_enabled = &CheckHost::log_enabled;
        api_.cancel_requested = &CheckHost::cancel_requested;
        api_.get_service = &CheckHost::get_service;
    }
    CheckHost(const CheckHost&) = delete;
    CheckHost& operator=(const CheckHost&) = delete;

    const praenora_host_api* api() const noexcept { return &api_; }

    // From the `poll`-th call of cancel_requested() on (1: every call), it reports nonzero; 0:
    // never.
    void cancel_from_poll(int poll) {
        const std::lock_guard<std::mutex> lock(mutex_);
        cancel_from_ = poll;
    }

    // The next call of cancel_requested() waits until release().
    void block_next_poll() {
        const std::lock_guard<std::mutex> lock(mutex_);
        block_next_ = true;
        released_ = false;
    }

    // Waits until a call waits in cancel_requested(); false after 30 s.
    bool wait_until_blocked() {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, std::chrono::seconds(30), [this] { return blocked_; });
    }

    // Lets the waiting call go on.
    void release() {
        const std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }

    int polls() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return polls_;
    }

    std::size_t logged() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return logs_.size();
    }

    // Reports a smaller struct_size, as a host of an older minor version or one that lacks the
    // later callbacks; the callbacks beyond it must not be called.
    void set_struct_size(std::size_t size) {
        api_.struct_size = static_cast<std::uint32_t>(size);
    }

private:
    static CheckHost* self(void* context) noexcept { return static_cast<CheckHost*>(context); }

    static void PRAENORA_CALL log(void* context, std::int32_t, const char* message,
                                  std::size_t size) noexcept {
        try {
            const std::lock_guard<std::mutex> lock(self(context)->mutex_);
            self(context)->logs_.emplace_back(message == nullptr ? "" : message,
                                              message == nullptr ? 0 : size);
        } catch (...) {
        }
    }

    static std::int32_t PRAENORA_CALL log_enabled(void*, std::int32_t) noexcept { return 1; }

    static std::int32_t PRAENORA_CALL cancel_requested(void* context) noexcept {
        CheckHost* host = self(context);
        std::unique_lock<std::mutex> lock(host->mutex_);
        ++host->polls_;
        if (host->block_next_) {
            host->block_next_ = false;
            host->blocked_ = true;
            host->changed_.notify_all();
            host->changed_.wait(lock, [host] { return host->released_; });
            host->blocked_ = false;
        }
        return host->cancel_from_ > 0 && host->polls_ >= host->cancel_from_ ? 1 : 0;
    }

    static const void* PRAENORA_CALL get_service(void*, const char*, std::size_t,
                                                 std::uint32_t) noexcept {
        return nullptr;
    }

    praenora_host_api api_{};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> logs_;
    int polls_ = 0;
    int cancel_from_ = 0;
    bool block_next_ = false;
    bool blocked_ = false;
    bool released_ = false;
};

// Runs `body` on a thread of its own; the destructor releases the host the thread may wait in and
// joins it.
class Worker {
public:
    template <class Body>
    Worker(CheckHost& host, Body&& body) : host_(host), thread_(std::forward<Body>(body)) {}
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    ~Worker() { finish(); }

    void finish() {
        host_.release();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    CheckHost& host_;
    std::thread thread_;
};

// ---------------------------------------------------------------------------------------------
// Objects of the service, closed when they go.

class Session {
public:
    Session() = default;
    Session(const praenora_simulation_v0* table, praenora_simulation_session* session)
        : table_(table), session_(session) {}
    Session(Session&& other) noexcept
        : table_(other.table_), session_(std::exchange(other.session_, nullptr)) {}
    Session& operator=(Session&& other) noexcept {
        if (this != &other) {
            close();
            table_ = other.table_;
            session_ = std::exchange(other.session_, nullptr);
        }
        return *this;
    }
    ~Session() { close(); }

    praenora_simulation_session* get() const noexcept { return session_; }
    explicit operator bool() const noexcept { return session_ != nullptr; }
    void close() noexcept {
        if (session_ != nullptr) {
            table_->close_session(std::exchange(session_, nullptr));
        }
    }

private:
    const praenora_simulation_v0* table_ = nullptr;
    praenora_simulation_session* session_ = nullptr;
};

class Plan {
public:
    Plan() = default;
    Plan(const praenora_simulation_v0* table, praenora_simulation_plan* plan)
        : table_(table), plan_(plan) {}
    Plan(Plan&& other) noexcept
        : table_(other.table_), plan_(std::exchange(other.plan_, nullptr)) {}
    Plan& operator=(Plan&& other) noexcept {
        if (this != &other) {
            close();
            table_ = other.table_;
            plan_ = std::exchange(other.plan_, nullptr);
        }
        return *this;
    }
    ~Plan() { close(); }

    praenora_simulation_plan* get() const noexcept { return plan_; }
    explicit operator bool() const noexcept { return plan_ != nullptr; }
    void close() noexcept {
        if (plan_ != nullptr) {
            table_->close_plan(std::exchange(plan_, nullptr));
        }
    }

private:
    const praenora_simulation_v0* table_ = nullptr;
    praenora_simulation_plan* plan_ = nullptr;
};

// A kernel struct of this build's size; closed when it goes, if it was opened.
class Kernel {
public:
    explicit Kernel(const praenora_simulation_v0* table) : table_(table) {
        kernel_.struct_size = static_cast<std::uint32_t>(sizeof(praenora_simulation_kernel));
    }
    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;
    ~Kernel() {
        if (kernel_.model != nullptr) {
            table_->close_kernel(&kernel_);
        }
    }

    praenora_simulation_kernel* get() noexcept { return &kernel_; }
    praenora_simulation_kernel& operator*() noexcept { return kernel_; }
    praenora_simulation_kernel* operator->() noexcept { return &kernel_; }

private:
    const praenora_simulation_v0* table_;
    praenora_simulation_kernel kernel_{};
};

// ---------------------------------------------------------------------------------------------
// Calls.

// What a call returned and wrote.
struct Call {
    praenora_status status = -1;
    std::string output, error;
};

praenora_bytes bytes(std::string_view text) { return praenora_bytes{text.data(), text.size()}; }

std::string options_of(const SimulationCase& c) { return json{{"format", c.model_format}}.dump(); }

Call open(const praenora_simulation_v0* t, std::string_view model, std::string_view options,
          Session& session, const praenora_host_api* host = nullptr) {
    TextSink error;
    praenora_simulation_session* opened = nullptr;
    Call call;
    call.status = t->open_session(t->context, host, bytes(model), bytes(options), &opened,
                                  error.get());
    call.error = error.text();
    if (call.status == PRAENORA_STATUS_OK) {
        session = Session(t, opened);
    } else {
        EXPECT_EQ(opened, nullptr) << "a failed open_session left a session";
    }
    return call;
}

// The case's model, opened; an empty session (and a test failure) when it does not open.
Session open_case(const praenora_simulation_v0* t, const SimulationCase& c) {
    Session session;
    const Call call = open(t, c.model, options_of(c), session);
    EXPECT_EQ(call.status, PRAENORA_STATUS_OK) << call.error;
    return session;
}

Call describe_model(const praenora_simulation_v0* t, const Session& session,
                    const praenora_host_api* host = nullptr) {
    TextSink out, error;
    Call call;
    call.status = t->describe_model(session.get(), host, out.get(), error.get());
    call.output = out.text();
    call.error = error.text();
    return call;
}

Call simulate(const praenora_simulation_v0* t, const Session& session, std::string_view request,
              const praenora_host_api* host = nullptr) {
    TextSink result, error;
    Call call;
    call.status = t->simulate(session.get(), host, bytes(request), result.get(), error.get());
    call.output = result.text();
    call.error = error.text();
    return call;
}

Call prepare(const praenora_simulation_v0* t, const Session& session, std::string_view plan_json,
             Plan& plan, const praenora_host_api* host = nullptr) {
    TextSink info, error;
    praenora_simulation_plan* prepared = nullptr;
    Call call;
    call.status =
        t->prepare(session.get(), host, bytes(plan_json), &prepared, info.get(), error.get());
    call.output = info.text();
    call.error = error.text();
    if (call.status == PRAENORA_STATUS_OK) {
        plan = Plan(t, prepared);
    } else {
        EXPECT_EQ(prepared, nullptr) << "a failed prepare left a plan";
    }
    return call;
}

struct RunResult {
    praenora_status status = -1;
    std::int32_t outcome = -1;
    std::vector<double> out;
    std::string failure, error;
};

RunResult run(const praenora_simulation_v0* t, const Plan& plan, const std::vector<double>& values,
              std::size_t out_count, const praenora_host_api* host = nullptr,
              bool failure_sink = true) {
    TextSink failure, error;
    RunResult result;
    result.out.assign(out_count, -1.0);
    result.status = t->run(plan.get(), host, values.empty() ? nullptr : values.data(),
                           values.size(), result.out.empty() ? nullptr : result.out.data(),
                           result.out.size(), &result.outcome,
                           failure_sink ? failure.get() : nullptr, error.get());
    result.failure = failure.text();
    result.error = error.text();
    return result;
}

struct BatchResult {
    praenora_status status = -1;
    std::vector<std::int32_t> outcomes;
    std::vector<double> out;
    std::string error;
};

BatchResult run_batch(const praenora_simulation_v0* t, const Plan& plan,
                      const std::vector<double>& values, std::size_t set_count,
                      std::size_t out_count, const praenora_host_api* host = nullptr) {
    TextSink error;
    BatchResult result;
    result.outcomes.assign(set_count, -1);
    result.out.assign(out_count, -1.0);
    result.status = t->run_batch(plan.get(), host, values.empty() ? nullptr : values.data(),
                                 values.size(), set_count,
                                 result.out.empty() ? nullptr : result.out.data(),
                                 result.out.size(),
                                 result.outcomes.empty() ? nullptr : result.outcomes.data(),
                                 error.get());
    result.error = error.text();
    return result;
}

Call open_kernel(const praenora_simulation_v0* t, const Session& session, std::string_view options,
                 Kernel& kernel, const praenora_host_api* host = nullptr) {
    TextSink error;
    Call call;
    call.status = t->open_kernel(session.get(), host, bytes(options), kernel.get(), error.get());
    call.error = error.text();
    if (call.status != PRAENORA_STATUS_OK) {
        EXPECT_EQ(kernel->model, nullptr) << "a failed open_kernel left a model";
    }
    return call;
}

// ---------------------------------------------------------------------------------------------
// Documents and expectations.

json parse(const std::string& text) { return json::parse(text, nullptr, false); }

// Whether `text` is the canonical JSON of the document it holds: compact, keys sorted.
bool canonical(const std::string& text) {
    const json document = parse(text);
    return !document.is_discarded() && document.dump() == text;
}

// Expects `call` to have failed with `code` and an error document of the pmp-error shape with
// details.service, and, when given, details.pointer `pointer`.
void expect_failure(praenora_status status, const std::string& error, praenora_status code,
                    const std::optional<std::string>& pointer = std::nullopt) {
    EXPECT_EQ(status, code) << praenora_status_name(status) << ": " << error;
    const json document = parse(error);
    ASSERT_TRUE(document.is_object()) << "error document: '" << error << "'";
    EXPECT_EQ(document.value("code", -1), code) << error;
    EXPECT_TRUE(document.contains("message") && document.at("message").is_string() &&
                !document.at("message").get<std::string>().empty())
        << error;
    ASSERT_TRUE(document.contains("details") && document.at("details").is_object()) << error;
    EXPECT_EQ(document.at("details").value("service", ""), kService) << error;
    if (pointer.has_value()) {
        EXPECT_EQ(document.at("details").value("pointer", "<none>"), *pointer) << error;
    }
}

void expect_failure(const Call& call, praenora_status code,
                    const std::optional<std::string>& pointer = std::nullopt) {
    expect_failure(call.status, call.error, code, pointer);
}

// Expects a failure document of a numerical failure: code FAILED and one of the reasons.
void expect_numerical_failure(const json& error) {
    ASSERT_TRUE(error.is_object()) << error;
    EXPECT_EQ(error.value("code", -1), PRAENORA_STATUS_FAILED) << error;
    EXPECT_TRUE(error.contains("message") && error.at("message").is_string()) << error;
    ASSERT_TRUE(error.contains("details") && error.at("details").is_object()) << error;
    const json& details = error.at("details");
    EXPECT_EQ(details.value("service", ""), kService) << error;
    const std::string reason = details.value("reason", "");
    EXPECT_TRUE(reason == "integrator" || reason == "step_limit" || reason == "nonfinite")
        << "reason '" << reason << "': " << error;
}

// Expects a refused model text: DATA_INVALID, with details.reason "model" or details.engine_error.
void expect_model_refused(const std::string& error) {
    const json details = parse(error).value("details", json::object());
    EXPECT_TRUE(details.value("reason", "") == "model" || details.contains("engine_error"))
        << error;
}

// The model's initial state for the values `parameters` of the case's parameters.
std::vector<double> initial_for(const SimulationCase& c, const std::vector<double>& parameters) {
    return c.initial_state_of ? c.initial_state_of(parameters) : c.initial_state;
}

bool near(double a, double b, const SimulationCase& c) {
    const double scale = std::max(std::fabs(a), std::fabs(b));
    return std::fabs(a - b) <= std::max(c.absolute_tolerance, c.relative_tolerance * scale);
}

void expect_near(const std::vector<double>& actual, const std::vector<double>& expected,
                 const SimulationCase& c, const std::string& what) {
    ASSERT_EQ(actual.size(), expected.size()) << what;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        EXPECT_TRUE(near(actual[i], expected[i], c))
            << what << " [" << i << "]: " << actual[i] << " differs from " << expected[i];
    }
}

// Whether two buffers hold the same doubles, bit for bit apart from NaN payloads.
bool same_values(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!(a[i] == b[i] || (std::isnan(a[i]) && std::isnan(b[i])))) {
            return false;
        }
    }
    return true;
}

bool all_nan(const double* values, std::size_t count) {
    return std::all_of(values, values + count, [](double v) { return std::isnan(v); });
}

std::vector<std::string> strings_of(const json& value) {
    std::vector<std::string> result;
    if (value.is_array()) {
        for (const json& item : value) {
            result.push_back(item.is_string() ? item.get<std::string>() : item.dump());
        }
    }
    return result;
}

bool contains_all(const std::vector<std::string>& all, const std::vector<std::string>& some) {
    return std::all_of(some.begin(), some.end(), [&all](const std::string& id) {
        return std::find(all.begin(), all.end(), id) != all.end();
    });
}

// The output times of the checks' runs.
std::vector<double> output_times(const SimulationCase& c) {
    const double end = c.end_time;
    return {0.0, end / 8, end / 4, end / 2, end};
}

std::vector<std::string> expected_columns(const SimulationCase& c, bool states = true) {
    std::vector<std::string> columns = {"time"};
    if (states) {
        columns.insert(columns.end(), c.states.begin(), c.states.end());
    }
    columns.insert(columns.end(), c.quantities.begin(), c.quantities.end());
    return columns;
}

// The exact output of a run, row-major, with the columns of expected_columns().
std::vector<double> exact_output(const SimulationCase& c, double start,
                                 const std::vector<double>& times,
                                 const std::vector<double>& parameters,
                                 const std::vector<double>& initial, bool states = true) {
    std::vector<double> out;
    for (const double t : times) {
        const std::vector<double> y = c.solution(start, t, parameters, initial);
        const std::vector<double> q = c.quantity_values(t, y, parameters);
        out.push_back(t);
        if (states) {
            out.insert(out.end(), y.begin(), y.end());
        }
        out.insert(out.end(), q.begin(), q.end());
    }
    return out;
}

// The rows of a simulate result, flattened row-major.
std::vector<double> flat_rows(const json& result) {
    std::vector<double> out;
    for (const json& row : result.at("rows")) {
        for (const json& value : row) {
            out.push_back(value.get<double>());
        }
    }
    return out;
}

// A request for the checks' runs: from 0 to the end time, the case's solver, states and
// quantities at output_times().
json base_request(const SimulationCase& c) {
    return {{"time_domain", {{"start", 0.0}, {"end", c.end_time}}},
            {"solver", c.solver},
            {"output",
             {{"save_at", "explicit_times"},
              {"times", output_times(c)},
              {"quantities", c.quantities},
              {"include_states", true}}}};
}

// base_request() with the case's parameters at `parameters` and the state at `initial`.
json request_with(const SimulationCase& c, const std::vector<double>& parameters,
                  const std::vector<double>& initial) {
    json request = base_request(c);
    for (std::size_t i = 0; i < c.parameters.size(); ++i) {
        request["parameter_values"][c.parameters[i]] = parameters[i];
    }
    for (std::size_t i = 0; i < c.states.size(); ++i) {
        request["initial_conditions"][c.states[i]] = initial[i];
    }
    return request;
}

// A plan of the checks' runs whose inputs are the case's parameters, then its states.
json base_plan(const SimulationCase& c) {
    json plan = base_request(c);
    plan["parameters"] = c.parameters;
    plan["initial_conditions"] = c.states;
    return plan;
}

std::vector<double> concat(std::vector<double> a, const std::vector<double>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

std::vector<double> scaled(std::vector<double> values, double factor) {
    for (double& value : values) {
        value *= factor;
    }
    return values;
}

std::size_t output_size(const SimulationCase& c) {
    return output_times(c).size() * expected_columns(c).size();
}

// The result of simulate() for `request`, as a document; a test failure when the call fails.
json simulated(const praenora_simulation_v0* t, const Session& session, const json& request) {
    const Call call = simulate(t, session, request.dump());
    EXPECT_EQ(call.status, PRAENORA_STATUS_OK) << call.error;
    return parse(call.output);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// describe and the table.

TEST_P(SimulationService, TableAndDescribeNameTheServiceAndItsEngine) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    EXPECT_EQ(t->header.abi_major, PRAENORA_SIMULATION_SERVICE_MAJOR);
    EXPECT_EQ(t->header.reserved, 0u);
    EXPECT_GE(t->header.struct_size, PRAENORA_SIMULATION_V0_BASE_SIZE);
    EXPECT_TRUE(t->describe != nullptr && t->open_session != nullptr &&
                t->describe_model != nullptr && t->simulate != nullptr && t->prepare != nullptr &&
                t->run != nullptr && t->run_batch != nullptr && t->open_kernel != nullptr &&
                t->close_kernel != nullptr && t->close_plan != nullptr &&
                t->close_session != nullptr)
        << "every function of the table is required";

    TextSink out;
    ASSERT_EQ(t->describe(t->context, out.get()), PRAENORA_STATUS_OK);
    EXPECT_TRUE(canonical(out.text())) << out.text();
    const json described = parse(out.text());
    ASSERT_TRUE(described.is_object()) << out.text();
    EXPECT_EQ(described.value("service", ""), kService);
    EXPECT_EQ(described.value("version", json()),
              json({{"major", PRAENORA_SIMULATION_SERVICE_MAJOR},
                    {"minor", t->header.abi_minor}}));
    // The engine is the one of the provider's manifest, which the run fingerprint records.
    std::ifstream manifest_file(c.provider_manifest, std::ios::binary);
    const json manifest = json::parse(manifest_file);
    EXPECT_EQ(described.value("engine", json()), manifest.at("engine"));
    const std::vector<std::string> formats = strings_of(described.value("model_formats", json()));
    const std::vector<std::string> methods = strings_of(described.value("methods", json()));
    EXPECT_TRUE(contains_all(formats, {c.model_format})) << out.text();
    EXPECT_TRUE(contains_all(methods, {c.solver.at("method").get<std::string>()})) << out.text();

    // Free of side effects: the same document every time, from any thread.
    std::string from_thread;
    std::thread([&] {
        TextSink again;
        if (t->describe(t->context, again.get()) == PRAENORA_STATUS_OK) {
            from_thread = again.text();
        }
    }).join();
    EXPECT_EQ(from_thread, out.text());
}

// ---------------------------------------------------------------------------------------------
// Sessions.

TEST_P(SimulationService, SessionsOpenAndDescribeTheModel) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    CheckHost host;
    Session session;
    const Call opened = open(t, c.model, options_of(c), session, host.api());
    ASSERT_EQ(opened.status, PRAENORA_STATUS_OK) << opened.error;
    ASSERT_TRUE(session);

    const Call described = describe_model(t, session, host.api());
    ASSERT_EQ(described.status, PRAENORA_STATUS_OK) << described.error;
    EXPECT_TRUE(canonical(described.output)) << described.output;
    const json model = parse(described.output);
    ASSERT_TRUE(model.is_object()) << described.output;
    EXPECT_EQ(strings_of(model.value("states", json())), c.states) << described.output;
    EXPECT_TRUE(contains_all(strings_of(model.value("parameters", json())), c.parameters))
        << described.output;
    EXPECT_TRUE(contains_all(strings_of(model.value("quantities", json())), c.quantities))
        << described.output;
    EXPECT_EQ(model.value("events", json()), json(false)) << described.output;
    EXPECT_EQ(describe_model(t, session).output, described.output);

    // Two sessions of one model are independent of each other.
    Session other;
    ASSERT_EQ(open(t, c.model, options_of(c), other).status, PRAENORA_STATUS_OK);
    EXPECT_EQ(describe_model(t, other).output, described.output);
    other.close();
    EXPECT_EQ(describe_model(t, session).output, described.output);

    // {} and empty options name the provider's first model format.
    TextSink out;
    ASSERT_EQ(t->describe(t->context, out.get()), PRAENORA_STATUS_OK);
    const std::vector<std::string> formats = strings_of(parse(out.text()).value("model_formats",
                                                                                json()));
    if (!formats.empty() && formats.front() == c.model_format) {
        for (const std::string_view options : {std::string_view("{}"), std::string_view()}) {
            Session plain;
            const Call call = open(t, c.model, options, plain);
            EXPECT_EQ(call.status, PRAENORA_STATUS_OK) << "options '" << options << "': "
                                                       << call.error;
        }
    }
}

TEST_P(SimulationService, OpenSessionRefusesWhatItCannotOpen) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    Session session;
    expect_failure(open(t, c.model, R"({"format":"no-such-format"})", session),
                   PRAENORA_STATUS_UNSUPPORTED, "/format");
    expect_failure(open(t, c.model, json{{"format", c.model_format}, {"bogus", 1}}.dump(), session),
                   PRAENORA_STATUS_INVALID_ARGUMENT, "/bogus");
    expect_failure(open(t, c.model, json{{"format", 7}}.dump(), session),
                   PRAENORA_STATUS_INVALID_ARGUMENT, "/format");
    expect_failure(open(t, c.model, "{", session), PRAENORA_STATUS_INVALID_ARGUMENT, "");
    expect_failure(open(t, c.model, "[]", session), PRAENORA_STATUS_INVALID_ARGUMENT, "");
    for (const std::string_view text : {std::string_view("not a model"), std::string_view()}) {
        const Call refused = open(t, text, options_of(c), session);
        expect_failure(refused, PRAENORA_STATUS_DATA_INVALID);
        expect_model_refused(refused.error);
    }
    EXPECT_FALSE(session);

    const std::string options = options_of(c);
    TextSink error;
    EXPECT_EQ(t->open_session(t->context, nullptr, bytes(c.model), bytes(options), nullptr,
                              error.get()),
              PRAENORA_STATUS_INVALID_ARGUMENT);
    praenora_simulation_session* opened = nullptr;
    TextSink error2;
    EXPECT_EQ(t->open_session(t->context, nullptr, praenora_bytes{nullptr, 3}, bytes(options),
                              &opened, error2.get()),
              PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(opened, nullptr);
}

// ---------------------------------------------------------------------------------------------
// Level A, JSON.

TEST_P(SimulationService, SimulateMatchesTheExactSolution) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    CheckHost host;
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    const Call call = simulate(t, session, base_request(c).dump(), host.api());
    ASSERT_EQ(call.status, PRAENORA_STATUS_OK) << call.error;
    EXPECT_TRUE(canonical(call.output)) << call.output;
    const json result = parse(call.output);
    ASSERT_TRUE(result.is_object()) << call.output;
    EXPECT_EQ(result.value("ok", false), true) << call.output;
    EXPECT_EQ(strings_of(result.value("columns", json())), expected_columns(c));
    ASSERT_TRUE(result.contains("rows") && result.at("rows").is_array()) << call.output;
    ASSERT_EQ(result.at("rows").size(), output_times(c).size());
    for (const json& row : result.at("rows")) {
        EXPECT_EQ(row.size(), expected_columns(c).size()) << row;
    }
    // The solver settings used, after defaults: at least the method.
    ASSERT_TRUE(result.contains("solver") && result.at("solver").is_object()) << call.output;
    EXPECT_EQ(result.at("solver").value("method", ""), c.solver.at("method")) << call.output;

    const std::vector<double> values = flat_rows(result);
    const std::vector<double> exact =
        exact_output(c, 0.0, output_times(c), c.parameter_values, c.initial_state);
    expect_near(values, exact, c, "rows");
    // The time column holds the output times exactly.
    const std::size_t columns = expected_columns(c).size();
    for (std::size_t row = 0; row < output_times(c).size(); ++row) {
        EXPECT_EQ(values[row * columns], output_times(c)[row]) << "row " << row;
    }
}

TEST_P(SimulationService, SimulateAppliesParametersInitialConditionsAndTheStartTime) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    const std::vector<double> parameters = scaled(c.parameter_values, 1.25);
    const std::vector<double> initial = scaled(c.initial_state, 0.5);
    const json result = simulated(t, session, request_with(c, parameters, initial));
    ASSERT_TRUE(result.is_object() && result.value("ok", false)) << result;
    expect_near(flat_rows(result), exact_output(c, 0.0, output_times(c), parameters, initial), c,
                "rows with parameter_values and initial_conditions");

    // Initial values take effect at the start time.
    json later = request_with(c, parameters, initial);
    const double start = c.end_time / 4;
    const std::vector<double> times = {start, c.end_time / 2, c.end_time * 1.5};
    later["time_domain"] = {{"start", start}, {"end", c.end_time * 1.5}};
    later["output"]["times"] = times;
    const json moved = simulated(t, session, later);
    ASSERT_TRUE(moved.is_object() && moved.value("ok", false)) << moved;
    expect_near(flat_rows(moved), exact_output(c, start, times, parameters, initial), c,
                "rows from a later start time");
}

TEST_P(SimulationService, SimulateOnARegularGrid) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    json request = base_request(c);
    const double step = c.end_time / 8;
    request["output"] = {{"save_at", "regular_grid"},
                         {"step", step},
                         {"quantities", c.quantities},
                         {"include_states", false}};
    const json result = simulated(t, session, request);
    ASSERT_TRUE(result.is_object() && result.value("ok", false)) << result;
    EXPECT_EQ(strings_of(result.at("columns")), expected_columns(c, false));
    std::vector<double> times;
    for (int i = 0; i <= 8; ++i) {
        times.push_back(0.0 + i * step);
    }
    ASSERT_EQ(result.at("rows").size(), times.size()) << result;
    const std::vector<double> values = flat_rows(result);
    expect_near(values,
                exact_output(c, 0.0, times, c.parameter_values, c.initial_state, false), c,
                "rows on the grid");
    for (std::size_t row = 0; row < times.size(); ++row) {
        EXPECT_EQ(values[row * (1 + c.quantities.size())], times[row]) << "row " << row;
    }
}

TEST_P(SimulationService, OutputTimesFollowTheRules) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    // The rows of `request`, which must succeed, against the exact solution at `times`.
    const auto expect_times = [&](const json& request, const std::vector<double>& times) {
        SCOPED_TRACE(request.dump());
        const json result = simulated(t, session, request);
        ASSERT_TRUE(result.is_object() && result.value("ok", false)) << result;
        ASSERT_EQ(result.at("rows").size(), times.size()) << result;
        const std::vector<double> values = flat_rows(result);
        expect_near(values, exact_output(c, 0.0, times, c.parameter_values, c.initial_state), c,
                    "rows");
        const std::size_t columns = expected_columns(c).size();
        for (std::size_t row = 0; row < times.size(); ++row) {
            EXPECT_EQ(values[row * columns], times[row]) << "row " << row;
        }
    };
    // The grid of the specification: start + i * step for i <= floor((end - start) / step + 1e-9),
    // a last time beyond end replaced by end.
    const auto grid = [](double start, double end, double step) {
        std::vector<double> times;
        const double last = std::floor((end - start) / step + 1e-9);
        for (double i = 0; i <= last; ++i) {
            times.push_back(std::min(start + i * step, end));
        }
        return times;
    };

    // A step that does not divide the time domain: the last time is before the end.
    json request = base_request(c);
    const double step = c.end_time * 0.15;
    request["output"] = {{"save_at", "regular_grid"},
                         {"step", step},
                         {"quantities", c.quantities},
                         {"include_states", true}};
    expect_times(request, grid(0.0, c.end_time, step));
    // A last time that lands just above the end (3 * 0.1 > 0.3): it is the end.
    request["time_domain"]["end"] = 0.3;
    request["output"]["step"] = 0.1;
    const std::vector<double> short_grid = grid(0.0, 0.3, 0.1);
    ASSERT_EQ(short_grid.size(), 4u);
    EXPECT_EQ(short_grid.back(), 0.3);
    expect_times(request, short_grid);
    // Explicit times need not begin at the start time, and one is enough.
    request = base_request(c);
    request["output"]["times"] = {c.end_time / 4, c.end_time};
    expect_times(request, {c.end_time / 4, c.end_time});
    request["output"]["times"] = {c.end_time / 2};
    expect_times(request, {c.end_time / 2});
}

TEST_P(SimulationService, EachSimulationStartsFromTheOriginalState) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    const std::string request = base_request(c).dump();
    const Call first = simulate(t, session, request);
    ASSERT_EQ(first.status, PRAENORA_STATUS_OK) << first.error;
    // Other values in between change nothing for later calls.
    const Call other = simulate(
        t, session,
        request_with(c, scaled(c.parameter_values, 3.0), scaled(c.initial_state, 2.0)).dump());
    ASSERT_EQ(other.status, PRAENORA_STATUS_OK) << other.error;
    EXPECT_NE(other.output, first.output);
    EXPECT_EQ(simulate(t, session, request).output, first.output);
    // A new session of the model gives the same bytes.
    const Session fresh = open_case(t, c);
    ASSERT_TRUE(fresh);
    EXPECT_EQ(simulate(t, fresh, request).output, first.output);
}

TEST_P(SimulationService, NumericalFailureIsAResult) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    const Call call =
        simulate(t, session, request_with(c, c.failing_parameters, c.initial_state).dump());
    ASSERT_EQ(call.status, PRAENORA_STATUS_OK) << call.error;
    EXPECT_TRUE(canonical(call.output)) << call.output;
    const json result = parse(call.output);
    ASSERT_TRUE(result.is_object()) << call.output;
    EXPECT_EQ(result.value("ok", true), false) << call.output;
    EXPECT_FALSE(result.contains("rows")) << call.output;
    expect_numerical_failure(result.value("error", json()));
    // The session goes on working.
    EXPECT_EQ(simulate(t, session, base_request(c).dump()).status, PRAENORA_STATUS_OK);
}

TEST_P(SimulationService, SimulateRefusesUnknownFieldsAndIds) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    const auto refused = [&](const json& request, const std::string& pointer) {
        SCOPED_TRACE(request.dump());
        expect_failure(simulate(t, session, request.dump()), PRAENORA_STATUS_INVALID_ARGUMENT,
                       pointer);
    };
    json request = base_request(c);
    request["bogus"] = 1;
    refused(request, "/bogus");
    request = base_request(c);
    request["time_domain"]["bogus"] = 1;
    refused(request, "/time_domain/bogus");
    request = base_request(c);
    request["solver"]["bogus"] = 1;
    refused(request, "/solver/bogus");
    request = base_request(c);
    request["output"]["bogus"] = 1;
    refused(request, "/output/bogus");
    request = base_request(c);
    request["initial_conditions"]["no_such_state"] = 1.0;
    refused(request, "/initial_conditions/no_such_state");
    request = base_request(c);
    request["parameter_values"]["no_such_parameter"] = 1.0;
    refused(request, "/parameter_values/no_such_parameter");
    request = base_request(c);
    request["output"]["quantities"] = {"no_such_quantity"};
    refused(request, "/output/quantities/0");
    request = base_request(c);
    request["parameter_values"][c.parameters.front()] = "1";
    refused(request, "/parameter_values/" + c.parameters.front());
    request = base_request(c);
    request.erase("time_domain");
    refused(request, "/time_domain");
    request = base_request(c);
    request["time_domain"]["end"] = 0.0;
    request["output"]["times"] = {0.0};
    refused(request, "/time_domain/end");
    request = base_request(c);
    request["output"]["times"] = {0.0, c.end_time * 2};
    refused(request, "/output/times/1");
    request = base_request(c);
    request["output"]["times"] = {c.end_time, 0.0};
    refused(request, "/output/times/1");
    request = base_request(c);
    request["output"].erase("save_at");
    refused(request, "/output/save_at");
    request = base_request(c);
    request["output"]["save_at"] = "sometimes";
    refused(request, "/output/save_at");
    request = base_request(c);
    request["output"]["step"] = 0.5; // only for regular_grid
    refused(request, "/output/step");
    request = base_request(c);
    request["output"] = {{"save_at", "regular_grid"}, {"step", 0.0}};
    refused(request, "/output/step");
    request = base_request(c);
    request["output"] = {{"save_at", "regular_grid"}, {"step", 0.5}, {"times", {0.0}}};
    refused(request, "/output/times");
    request = base_request(c);
    request["output"]["quantities"] = {c.quantities.front(), c.quantities.front()};
    refused(request, "/output/quantities/1");
    // Below the precision of the start time, start + i * step does not increase.
    request = base_request(c);
    request["time_domain"] = {{"start", 1e17}, {"end", 1e17 + 64}};
    request["output"] = {{"save_at", "regular_grid"}, {"step", 1.0}};
    refused(request, "/output/step");
    refused(json::array(), "");
    expect_failure(simulate(t, session, "{\"time_domain\":"), PRAENORA_STATUS_INVALID_ARGUMENT,
                   "");
}

TEST_P(SimulationService, SimulateRefusesWhatItDoesNotMap) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    const auto unsupported = [&](const json& request, const std::string& pointer) {
        SCOPED_TRACE(request.dump());
        expect_failure(simulate(t, session, request.dump()), PRAENORA_STATUS_UNSUPPORTED,
                       pointer);
    };
    json request = base_request(c);
    request["inputs"] = json::object();
    unsupported(request, "/inputs");
    request = base_request(c);
    request["events_enabled"] = true;
    unsupported(request, "/events_enabled");
    request = base_request(c);
    request["solver"]["method"] = "no-such-method";
    unsupported(request, "/solver/method");
    request = base_request(c);
    request["solver"]["event_time_tolerance"] = 1e-6;
    unsupported(request, "/solver/event_time_tolerance");
    request = base_request(c);
    request["output"]["save_at"] = "target_times";
    request["output"].erase("times");
    unsupported(request, "/output/save_at");
    request = base_request(c);
    request["output"]["dense_output"] = true;
    unsupported(request, "/output/dense_output");
    request = base_request(c);
    request["checks"] = {{"reject_nonfinite", false}};
    unsupported(request, "/checks/reject_nonfinite");

    // What is always on may be asked for.
    request = base_request(c);
    request["checks"] = {{"reject_nonfinite", true}, {"enforce_time_domain", true}};
    request["output"]["dense_output"] = false;
    const Call call = simulate(t, session, request.dump());
    EXPECT_EQ(call.status, PRAENORA_STATUS_OK) << call.error;
}

// ---------------------------------------------------------------------------------------------
// Level A, plans.

TEST_P(SimulationService, PlanDescribesItsInputsAndShape) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    Plan plan;
    const Call call = prepare(t, session, base_plan(c).dump(), plan);
    ASSERT_EQ(call.status, PRAENORA_STATUS_OK) << call.error;
    ASSERT_TRUE(plan);
    EXPECT_TRUE(canonical(call.output)) << call.output;
    const json info = parse(call.output);
    ASSERT_TRUE(info.is_object()) << call.output;
    std::vector<std::string> inputs = c.parameters;
    inputs.insert(inputs.end(), c.states.begin(), c.states.end());
    EXPECT_EQ(strings_of(info.value("inputs", json())), inputs) << call.output;
    EXPECT_EQ(strings_of(info.value("columns", json())), expected_columns(c)) << call.output;
    EXPECT_EQ(info.value("rows", json()), json(output_times(c).size())) << call.output;
    ASSERT_TRUE(info.contains("solver") && info.at("solver").is_object()) << call.output;
    EXPECT_EQ(info.at("solver").value("method", ""), c.solver.at("method")) << call.output;
    // The solver settings are those simulate() reports for the same settings.
    EXPECT_EQ(info.at("solver"), simulated(t, session, base_request(c)).value("solver", json()));

    // Inputs in the order the plan names them, any subset.
    json subset = base_request(c);
    subset["parameters"] = {c.parameters.back()};
    subset["initial_conditions"] = {c.states.back()};
    Plan small;
    const Call second = prepare(t, session, subset.dump(), small);
    ASSERT_EQ(second.status, PRAENORA_STATUS_OK) << second.error;
    EXPECT_EQ(strings_of(parse(second.output).value("inputs", json())),
              std::vector<std::string>({c.parameters.back(), c.states.back()}));
    json none = base_request(c);
    Plan fixed;
    const Call third = prepare(t, session, none.dump(), fixed);
    ASSERT_EQ(third.status, PRAENORA_STATUS_OK) << third.error;
    EXPECT_EQ(parse(third.output).value("inputs", json()), json::array());
}

TEST_P(SimulationService, PlanRunsMatchSimulate) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    CheckHost host;
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    Plan plan;
    ASSERT_EQ(prepare(t, session, base_plan(c).dump(), plan).status, PRAENORA_STATUS_OK);

    const std::vector<double> nominal = concat(c.parameter_values, c.initial_state);
    const RunResult first = run(t, plan, nominal, output_size(c), host.api());
    ASSERT_EQ(first.status, PRAENORA_STATUS_OK) << first.error;
    EXPECT_EQ(first.outcome, PRAENORA_SIMULATION_RUN_OK);
    EXPECT_TRUE(first.failure.empty()) << first.failure;
    // The same numbers as simulate() with the same settings, bit for bit.
    EXPECT_TRUE(same_values(first.out, flat_rows(simulated(t, session, base_request(c)))));
    expect_near(first.out, exact_output(c, 0.0, output_times(c), c.parameter_values,
                                        c.initial_state),
                c, "run");

    const std::vector<double> parameters = scaled(c.parameter_values, 1.1);
    const std::vector<double> initial = scaled(c.initial_state, 0.9);
    const RunResult second = run(t, plan, concat(parameters, initial), output_size(c));
    ASSERT_EQ(second.status, PRAENORA_STATUS_OK) << second.error;
    EXPECT_EQ(second.outcome, PRAENORA_SIMULATION_RUN_OK);
    EXPECT_TRUE(same_values(
        second.out, flat_rows(simulated(t, session, request_with(c, parameters, initial)))));
    expect_near(second.out, exact_output(c, 0.0, output_times(c), parameters, initial), c,
                "run with other inputs");

    // Each run starts from the model's original state.
    EXPECT_TRUE(same_values(run(t, plan, nominal, output_size(c)).out, first.out));

    // A plan with one input leaves the others at the model's values.
    json subset = base_request(c);
    subset["parameters"] = {c.parameters.front()};
    Plan small;
    ASSERT_EQ(prepare(t, session, subset.dump(), small).status, PRAENORA_STATUS_OK);
    json request = base_request(c);
    request["parameter_values"][c.parameters.front()] = parameters.front();
    const RunResult one = run(t, small, {parameters.front()}, output_size(c));
    ASSERT_EQ(one.status, PRAENORA_STATUS_OK) << one.error;
    EXPECT_TRUE(same_values(one.out, flat_rows(simulated(t, session, request))));
}

TEST_P(SimulationService, ParameterValuesTakeEffectAsInitialValues) {
    // Parameter values take effect as the model's initial values: its initial assignments are
    // evaluated with them; then initial conditions replace state values at the start.
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    const std::vector<double> parameters = scaled(c.parameter_values, 1.5);
    const std::vector<double> initial = initial_for(c, parameters);

    // simulate: parameter values alone, then with initial conditions on top.
    json request = base_request(c);
    for (std::size_t i = 0; i < c.parameters.size(); ++i) {
        request["parameter_values"][c.parameters[i]] = parameters[i];
    }
    const json result = simulated(t, session, request);
    ASSERT_TRUE(result.is_object() && result.value("ok", false)) << result;
    expect_near(flat_rows(result), exact_output(c, 0.0, output_times(c), parameters, initial), c,
                "rows with parameter values");
    const std::vector<double> replaced = scaled(c.initial_state, 0.25);
    expect_near(flat_rows(simulated(t, session, request_with(c, parameters, replaced))),
                exact_output(c, 0.0, output_times(c), parameters, replaced), c,
                "rows with parameter values and initial conditions");

    // A plan whose inputs are the parameters: the same numbers as simulate.
    json plan_json = base_request(c);
    plan_json["parameters"] = c.parameters;
    Plan plan;
    ASSERT_EQ(prepare(t, session, plan_json.dump(), plan).status, PRAENORA_STATUS_OK);
    const RunResult ran = run(t, plan, parameters, output_size(c));
    ASSERT_EQ(ran.status, PRAENORA_STATUS_OK) << ran.error;
    EXPECT_TRUE(same_values(ran.out, flat_rows(result)));

    // A kernel: initial_state follows set_parameters; rhs and quantities depend only on their
    // arguments and the current parameters, whatever was called before.
    Kernel kernel(t);
    ASSERT_EQ(open_kernel(t, session,
                          json{{"parameters", c.parameters}, {"quantities", c.quantities}}.dump(),
                          kernel)
                  .status,
              PRAENORA_STATUS_OK);
    void* model = kernel->model;
    const std::size_t n = c.states.size();
    std::vector<double> y(n, kNaN);
    ASSERT_EQ(kernel->set_parameters(model, parameters.data()), PRAENORA_STATUS_OK);
    ASSERT_EQ(kernel->initial_state(model, y.data()), PRAENORA_STATUS_OK);
    expect_near(y, initial, c, "initial_state");
    const double time = c.end_time / 2;
    std::vector<double> state = c.solution(0.0, time, parameters, initial);
    const auto rates_and_quantities = [&] {
        std::vector<double> values(n + c.quantities.size(), kNaN);
        EXPECT_EQ(kernel->rhs(model, time, state.data(), values.data()), PRAENORA_STATUS_OK);
        EXPECT_EQ(kernel->quantities(model, time, state.data(), values.data() + n),
                  PRAENORA_STATUS_OK);
        return values;
    };
    const std::vector<double> before = rates_and_quantities();
    const auto rates_end = before.begin() + static_cast<std::ptrdiff_t>(n);
    expect_near(std::vector<double>(before.begin(), rates_end),
                c.derivative(time, state, parameters), c, "rhs");
    ASSERT_EQ(kernel->initial_state(model, y.data()), PRAENORA_STATUS_OK);
    EXPECT_TRUE(same_values(rates_and_quantities(), before)) << "initial_state changed rhs";
    // Other values and back: the same results as before.
    const std::vector<double> other = scaled(c.parameter_values, 0.5);
    ASSERT_EQ(kernel->set_parameters(model, other.data()), PRAENORA_STATUS_OK);
    ASSERT_EQ(kernel->initial_state(model, y.data()), PRAENORA_STATUS_OK);
    expect_near(y, initial_for(c, other), c, "initial_state with other values");
    ASSERT_EQ(kernel->set_parameters(model, parameters.data()), PRAENORA_STATUS_OK);
    EXPECT_TRUE(same_values(rates_and_quantities(), before)) << "set_parameters left history";
}

TEST_P(SimulationService, BatchRunsInOrderWithAnOutcomeEach) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    CheckHost host;
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    Plan plan;
    ASSERT_EQ(prepare(t, session, base_plan(c).dump(), plan).status, PRAENORA_STATUS_OK);
    const std::vector<double> nominal = concat(c.parameter_values, c.initial_state);
    const std::vector<double> failing = concat(c.failing_parameters, c.initial_state);
    const std::vector<double> other =
        concat(scaled(c.parameter_values, 0.8), scaled(c.initial_state, 1.5));
    const std::size_t size = output_size(c);

    const BatchResult batch =
        run_batch(t, plan, concat(concat(nominal, failing), other), 3, 3 * size, host.api());
    ASSERT_EQ(batch.status, PRAENORA_STATUS_OK) << batch.error;
    EXPECT_EQ(batch.outcomes,
              std::vector<std::int32_t>({PRAENORA_SIMULATION_RUN_OK, PRAENORA_SIMULATION_RUN_FAILED,
                                         PRAENORA_SIMULATION_RUN_OK}));
    const auto part = [&](std::size_t set) {
        const auto first = batch.out.begin() + static_cast<std::ptrdiff_t>(set * size);
        return std::vector<double>(first, first + static_cast<std::ptrdiff_t>(size));
    };
    // Each set's output is that of run() with its values.
    EXPECT_TRUE(same_values(part(0), run(t, plan, nominal, size).out));
    EXPECT_TRUE(all_nan(batch.out.data() + size, size)) << "a failed run's output is NaN";
    EXPECT_TRUE(same_values(part(2), run(t, plan, other, size).out));
    // Polled before work starts and between the sets.
    EXPECT_GE(host.polls(), 3);

    // No set: nothing to do.
    const BatchResult empty = run_batch(t, plan, {}, 0, 0);
    EXPECT_EQ(empty.status, PRAENORA_STATUS_OK) << empty.error;
}

TEST_P(SimulationService, RunReportsNumericalFailures) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    Plan plan;
    ASSERT_EQ(prepare(t, session, base_plan(c).dump(), plan).status, PRAENORA_STATUS_OK);
    const std::vector<double> failing = concat(c.failing_parameters, c.initial_state);

    const RunResult failed = run(t, plan, failing, output_size(c));
    ASSERT_EQ(failed.status, PRAENORA_STATUS_OK) << failed.error;
    EXPECT_EQ(failed.outcome, PRAENORA_SIMULATION_RUN_FAILED);
    EXPECT_TRUE(all_nan(failed.out.data(), failed.out.size()));
    EXPECT_TRUE(canonical(failed.failure)) << failed.failure;
    expect_numerical_failure(parse(failed.failure));
    // The failure simulate() reports for the same values.
    const json simulated_failure =
        simulated(t, session, request_with(c, c.failing_parameters, c.initial_state))
            .value("error", json());
    const json run_failure = parse(failed.failure);
    ASSERT_TRUE(run_failure.is_object() && simulated_failure.is_object()) << simulated_failure;
    EXPECT_EQ(run_failure.at("details").value("reason", ""),
              simulated_failure.at("details").value("reason", ""));

    // failure_json may be NULL.
    const RunResult quiet = run(t, plan, failing, output_size(c), nullptr, false);
    EXPECT_EQ(quiet.status, PRAENORA_STATUS_OK) << quiet.error;
    EXPECT_EQ(quiet.outcome, PRAENORA_SIMULATION_RUN_FAILED);
    // The plan goes on working.
    EXPECT_EQ(run(t, plan, concat(c.parameter_values, c.initial_state), output_size(c)).outcome,
              PRAENORA_SIMULATION_RUN_OK);
}

TEST_P(SimulationService, RunsRefuseWrongBuffers) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    Plan plan;
    ASSERT_EQ(prepare(t, session, base_plan(c).dump(), plan).status, PRAENORA_STATUS_OK);
    const std::vector<double> nominal = concat(c.parameter_values, c.initial_state);
    const std::size_t size = output_size(c);

    std::vector<double> fewer = nominal;
    fewer.pop_back();
    RunResult result = run(t, plan, fewer, size);
    expect_failure(result.status, result.error, PRAENORA_STATUS_INVALID_ARGUMENT);
    result = run(t, plan, concat(nominal, {1.0}), size);
    expect_failure(result.status, result.error, PRAENORA_STATUS_INVALID_ARGUMENT);
    result = run(t, plan, nominal, size - 1);
    expect_failure(result.status, result.error, PRAENORA_STATUS_INVALID_ARGUMENT);
    result = run(t, plan, nominal, size + 1);
    expect_failure(result.status, result.error, PRAENORA_STATUS_INVALID_ARGUMENT);
    std::vector<double> not_finite = nominal;
    not_finite[1] = kNaN;
    result = run(t, plan, not_finite, size);
    expect_failure(result.status, result.error, PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(parse(result.error).at("details").value("index", -1), 1) << result.error;
    // In a batch, the index into all values; no set runs.
    const BatchResult later = run_batch(t, plan, concat(nominal, not_finite), 2, 2 * size);
    expect_failure(later.status, later.error, PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(parse(later.error).at("details").value("index", std::size_t{0}), nominal.size() + 1)
        << later.error;
    EXPECT_EQ(later.outcomes, std::vector<std::int32_t>({-1, -1}));

    std::vector<double> out(size);
    std::int32_t outcome = -1;
    TextSink error;
    EXPECT_EQ(t->run(plan.get(), nullptr, nominal.data(), nominal.size(), out.data(), out.size(),
                     nullptr, nullptr, error.get()),
              PRAENORA_STATUS_INVALID_ARGUMENT);
    TextSink error2;
    EXPECT_EQ(t->run(plan.get(), nullptr, nullptr, nominal.size(), out.data(), out.size(),
                     &outcome, nullptr, error2.get()),
              PRAENORA_STATUS_INVALID_ARGUMENT);
    TextSink error3;
    EXPECT_EQ(t->run(plan.get(), nullptr, nominal.data(), nominal.size(), nullptr, out.size(),
                     &outcome, nullptr, error3.get()),
              PRAENORA_STATUS_INVALID_ARGUMENT);

    // Batches: every count must match, and products must not overflow.
    BatchResult batch = run_batch(t, plan, nominal, 2, 2 * size);
    expect_failure(batch.status, batch.error, PRAENORA_STATUS_INVALID_ARGUMENT);
    batch = run_batch(t, plan, concat(nominal, nominal), 2, 2 * size - 1);
    expect_failure(batch.status, batch.error, PRAENORA_STATUS_INVALID_ARGUMENT);
    std::vector<double> values = concat(nominal, nominal);
    std::vector<double> outputs(2 * size);
    TextSink error4;
    EXPECT_EQ(t->run_batch(plan.get(), nullptr, values.data(), values.size(), 2, outputs.data(),
                           outputs.size(), nullptr, error4.get()),
              PRAENORA_STATUS_INVALID_ARGUMENT);
    std::int32_t outcomes[2] = {-1, -1};
    const std::uint64_t huge = std::numeric_limits<std::uint64_t>::max() / 2 + 1;
    TextSink error5;
    EXPECT_EQ(t->run_batch(plan.get(), nullptr, values.data(), values.size(), huge,
                           outputs.data(), outputs.size(), outcomes, error5.get()),
              PRAENORA_STATUS_INVALID_ARGUMENT);
    // Nothing ran.
    EXPECT_EQ(outcomes[0], -1);
}

TEST_P(SimulationService, PrepareRefusesUnknownIdsAndFields) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    const auto refused = [&](const json& plan_json, praenora_status code,
                             const std::string& pointer) {
        SCOPED_TRACE(plan_json.dump());
        Plan plan;
        expect_failure(prepare(t, session, plan_json.dump(), plan), code, pointer);
        EXPECT_FALSE(plan);
    };
    json plan_json = base_plan(c);
    plan_json["parameters"] = {"no_such_parameter"};
    refused(plan_json, PRAENORA_STATUS_INVALID_ARGUMENT, "/parameters/0");
    plan_json = base_plan(c);
    plan_json["initial_conditions"] = {c.states.front(), "no_such_state"};
    refused(plan_json, PRAENORA_STATUS_INVALID_ARGUMENT, "/initial_conditions/1");
    plan_json = base_plan(c);
    plan_json["parameters"] = {c.parameters.front(), c.parameters.front()};
    refused(plan_json, PRAENORA_STATUS_INVALID_ARGUMENT, "/parameters/1");
    plan_json = base_plan(c);
    plan_json["parameter_values"] = json::object(); // requests only
    refused(plan_json, PRAENORA_STATUS_INVALID_ARGUMENT, "/parameter_values");
    plan_json = base_plan(c);
    plan_json["output"]["bogus"] = true;
    refused(plan_json, PRAENORA_STATUS_INVALID_ARGUMENT, "/output/bogus");
    plan_json = base_plan(c);
    plan_json["output"] = {{"save_at", "solver_steps"}};
    refused(plan_json, PRAENORA_STATUS_UNSUPPORTED, "/output/save_at");
    plan_json = base_plan(c);
    plan_json["inputs"] = json::object();
    refused(plan_json, PRAENORA_STATUS_UNSUPPORTED, "/inputs");
    plan_json = base_plan(c);
    plan_json["checks"] = {{"reject_nonfinite", false}};
    refused(plan_json, PRAENORA_STATUS_UNSUPPORTED, "/checks/reject_nonfinite");
    plan_json = base_plan(c);
    plan_json["checks"] = {{"reject_nonfinite", true}, {"enforce_time_domain", true}};
    Plan checked;
    const Call accepted = prepare(t, session, plan_json.dump(), checked);
    EXPECT_EQ(accepted.status, PRAENORA_STATUS_OK) << accepted.error;

    const std::string text = base_plan(c).dump();
    TextSink info, error;
    EXPECT_EQ(t->prepare(session.get(), nullptr, bytes(text), nullptr, info.get(), error.get()),
              PRAENORA_STATUS_INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------------------------
// Level B, kernels.

TEST_P(SimulationService, KernelMatchesTheExactDerivative) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    Kernel kernel(t);
    const Call call = open_kernel(
        t, session, json{{"parameters", c.parameters}, {"quantities", c.quantities}}.dump(),
        kernel);
    ASSERT_EQ(call.status, PRAENORA_STATUS_OK) << call.error;
    ASSERT_NE(kernel->model, nullptr);
    EXPECT_EQ(kernel->struct_size, PRAENORA_SIMULATION_KERNEL_V0_SIZE);
    EXPECT_EQ(kernel->reserved, 0u);
    ASSERT_EQ(kernel->state_count, c.states.size());
    ASSERT_EQ(kernel->parameter_count, c.parameters.size());
    ASSERT_EQ(kernel->quantity_count, c.quantities.size());
    ASSERT_TRUE(kernel->rhs != nullptr && kernel->set_parameters != nullptr &&
                kernel->initial_state != nullptr && kernel->quantities != nullptr);
    void* model = kernel->model;
    const std::size_t n = c.states.size();

    std::vector<double> y0(n, kNaN);
    ASSERT_EQ(kernel->initial_state(model, y0.data()), PRAENORA_STATUS_OK);
    expect_near(y0, c.initial_state, c, "initial_state");
    std::vector<double> dydt(n, kNaN);
    ASSERT_EQ(kernel->rhs(model, 0.0, y0.data(), dydt.data()), PRAENORA_STATUS_OK);
    expect_near(dydt, c.derivative(0.0, y0, c.parameter_values), c, "rhs at the initial state");

    const double time = c.end_time / 2;
    const std::vector<double> y = c.solution(0.0, time, c.parameter_values, c.initial_state);
    std::vector<double> state = y;
    ASSERT_EQ(kernel->rhs(model, time, state.data(), dydt.data()), PRAENORA_STATUS_OK);
    expect_near(dydt, c.derivative(time, y, c.parameter_values), c, "rhs");
    // rhs changes nothing: not its input, not the next result, not the initial state.
    EXPECT_EQ(state, y);
    std::vector<double> again(n, kNaN);
    ASSERT_EQ(kernel->rhs(model, time, state.data(), again.data()), PRAENORA_STATUS_OK);
    EXPECT_TRUE(same_values(again, dydt));
    std::vector<double> q(c.quantities.size(), kNaN);
    ASSERT_EQ(kernel->quantities(model, time, state.data(), q.data()), PRAENORA_STATUS_OK);
    expect_near(q, c.quantity_values(time, y, c.parameter_values), c, "quantities");

    // Other parameters.
    const std::vector<double> parameters = scaled(c.parameter_values, 1.5);
    ASSERT_EQ(kernel->set_parameters(model, parameters.data()), PRAENORA_STATUS_OK);
    ASSERT_EQ(kernel->rhs(model, time, state.data(), dydt.data()), PRAENORA_STATUS_OK);
    expect_near(dydt, c.derivative(time, y, parameters), c, "rhs with other parameters");
    ASSERT_EQ(kernel->quantities(model, time, state.data(), q.data()), PRAENORA_STATUS_OK);
    expect_near(q, c.quantity_values(time, y, parameters), c, "quantities with other parameters");
    ASSERT_EQ(kernel->initial_state(model, y0.data()), PRAENORA_STATUS_OK);
    expect_near(y0, initial_for(c, parameters), c, "initial_state with other parameters");

    // Non-finite values are computed, not refused.
    const std::vector<double> nan_parameters(c.parameters.size(), kNaN);
    ASSERT_EQ(kernel->set_parameters(model, nan_parameters.data()), PRAENORA_STATUS_OK);
    ASSERT_EQ(kernel->rhs(model, time, state.data(), dydt.data()), PRAENORA_STATUS_OK);
    EXPECT_TRUE(std::any_of(dydt.begin(), dydt.end(), [](double v) { return !std::isfinite(v); }));

    // NULL pointers.
    EXPECT_EQ(kernel->rhs(nullptr, time, state.data(), dydt.data()),
              PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(kernel->rhs(model, time, nullptr, dydt.data()), PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(kernel->rhs(model, time, state.data(), nullptr), PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(kernel->set_parameters(model, nullptr), PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(kernel->set_parameters(nullptr, parameters.data()),
              PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(kernel->initial_state(model, nullptr), PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(kernel->initial_state(nullptr, y0.data()), PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(kernel->quantities(model, time, nullptr, q.data()),
              PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(kernel->quantities(model, time, state.data(), nullptr),
              PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(kernel->quantities(nullptr, time, state.data(), q.data()),
              PRAENORA_STATUS_INVALID_ARGUMENT);
}

TEST_P(SimulationService, KernelUsesTheOrderTheModuleChose) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    std::vector<std::string> parameters(c.parameters.rbegin(), c.parameters.rend());
    std::vector<std::string> quantities(c.quantities.rbegin(), c.quantities.rend());
    Kernel kernel(t);
    ASSERT_EQ(open_kernel(t, session,
                          json{{"parameters", parameters}, {"quantities", quantities}}.dump(),
                          kernel)
                  .status,
              PRAENORA_STATUS_OK);
    const std::vector<double> values = scaled(c.parameter_values, 0.75);
    const std::vector<double> reversed(values.rbegin(), values.rend());
    ASSERT_EQ(kernel->set_parameters(kernel->model, reversed.data()), PRAENORA_STATUS_OK);
    const double time = c.end_time / 4;
    std::vector<double> y = c.solution(0.0, time, values, c.initial_state);
    std::vector<double> dydt(c.states.size(), kNaN);
    ASSERT_EQ(kernel->rhs(kernel->model, time, y.data(), dydt.data()), PRAENORA_STATUS_OK);
    expect_near(dydt, c.derivative(time, y, values), c, "rhs");
    std::vector<double> q(quantities.size(), kNaN);
    ASSERT_EQ(kernel->quantities(kernel->model, time, y.data(), q.data()), PRAENORA_STATUS_OK);
    const std::vector<double> expected = c.quantity_values(time, y, values);
    expect_near(q, std::vector<double>(expected.rbegin(), expected.rend()), c, "quantities");
}

TEST_P(SimulationService, KernelOptions) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    const json model = parse(describe_model(t, session).output);
    ASSERT_TRUE(model.is_object());

    // Without lists: every parameter and quantity, in describe_model's order.
    for (const std::string_view options : {std::string_view("{}"), std::string_view()}) {
        Kernel all(t);
        ASSERT_EQ(open_kernel(t, session, options, all).status, PRAENORA_STATUS_OK) << options;
        EXPECT_EQ(all->parameter_count, model.at("parameters").size());
        EXPECT_EQ(all->quantity_count, model.at("quantities").size());
    }
    // Empty lists: none, and the arrays of no elements may be NULL.
    Kernel none(t);
    ASSERT_EQ(open_kernel(t, session, R"({"parameters":[],"quantities":[]})", none).status,
              PRAENORA_STATUS_OK);
    EXPECT_EQ(none->parameter_count, 0u);
    EXPECT_EQ(none->quantity_count, 0u);
    EXPECT_EQ(none->set_parameters(none->model, nullptr), PRAENORA_STATUS_OK);
    std::vector<double> y = c.initial_state;
    EXPECT_EQ(none->quantities(none->model, 0.0, y.data(), nullptr), PRAENORA_STATUS_OK);

    const auto refused = [&](const json& options, const std::string& pointer) {
        SCOPED_TRACE(options.dump());
        Kernel kernel(t);
        expect_failure(open_kernel(t, session, options.dump(), kernel),
                       PRAENORA_STATUS_INVALID_ARGUMENT, pointer);
    };
    refused({{"parameters", {"no_such_parameter"}}}, "/parameters/0");
    refused({{"parameters", {c.parameters.front(), c.parameters.front()}}}, "/parameters/1");
    refused({{"quantities", {"no_such_quantity"}}}, "/quantities/0");
    refused({{"bogus", true}}, "/bogus");
    refused(json::array(), "");

    Kernel small(t);
    small->struct_size = static_cast<std::uint32_t>(PRAENORA_SIMULATION_KERNEL_V0_SIZE - 1);
    TextSink error;
    EXPECT_EQ(t->open_kernel(session.get(), nullptr, bytes("{}"), small.get(), error.get()),
              PRAENORA_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(small->model, nullptr);
    TextSink error2;
    EXPECT_EQ(t->open_kernel(session.get(), nullptr, bytes("{}"), nullptr, error2.get()),
              PRAENORA_STATUS_INVALID_ARGUMENT);
}

TEST_P(SimulationService, ModelsWithEventsHaveNoKernel) {
    // A kernel is the model's continuous right-hand side only: a model with events has none.
    const SimulationCase& c = GetParam();
    if (c.model_with_events.empty()) {
        GTEST_SKIP() << "the parameter set has no model with events";
    }
    const praenora_simulation_v0* t = table();
    Session session;
    const Call opened = open(t, c.model_with_events, options_of(c), session);
    ASSERT_EQ(opened.status, PRAENORA_STATUS_OK) << opened.error;
    const Call described = describe_model(t, session);
    ASSERT_EQ(described.status, PRAENORA_STATUS_OK) << described.error;
    EXPECT_EQ(parse(described.output).value("events", json()), json(true)) << described.output;
    Kernel kernel(t);
    const Call refused = open_kernel(t, session, "{}", kernel);
    expect_failure(refused, PRAENORA_STATUS_UNSUPPORTED);
    EXPECT_EQ(parse(refused.error).at("details").value("reason", ""), "events") << refused.error;
}

// ---------------------------------------------------------------------------------------------
// Threading, cancellation, sinks, limits and lifetime.

TEST_P(SimulationService, BusySessionsAndPlansRefuseOtherCalls) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    Plan plan, other_plan;
    ASSERT_EQ(prepare(t, session, base_plan(c).dump(), plan).status, PRAENORA_STATUS_OK);
    ASSERT_EQ(prepare(t, session, base_plan(c).dump(), other_plan).status, PRAENORA_STATUS_OK);
    const std::string request = base_request(c).dump();
    const std::vector<double> nominal = concat(c.parameter_values, c.initial_state);

    {
        // A session that is busy with simulate().
        CheckHost blocking;
        blocking.block_next_poll();
        praenora_status first = -1;
        Worker worker(blocking,
                      [&] { first = simulate(t, session, request, blocking.api()).status; });
        ASSERT_TRUE(blocking.wait_until_blocked());
        expect_failure(describe_model(t, session), PRAENORA_STATUS_INVALID_STATE);
        expect_failure(simulate(t, session, request), PRAENORA_STATUS_INVALID_STATE);
        Plan refused;
        expect_failure(prepare(t, session, base_plan(c).dump(), refused),
                       PRAENORA_STATUS_INVALID_STATE);
        Kernel kernel(t);
        expect_failure(open_kernel(t, session, "{}", kernel), PRAENORA_STATUS_INVALID_STATE);
        // Its plans are not busy.
        EXPECT_EQ(run(t, plan, nominal, output_size(c)).status, PRAENORA_STATUS_OK);
        worker.finish();
        EXPECT_EQ(first, PRAENORA_STATUS_OK);
    }
    {
        // A plan that is busy with run().
        CheckHost blocking;
        blocking.block_next_poll();
        praenora_status first = -1;
        Worker worker(blocking, [&] {
            first = run(t, plan, nominal, output_size(c), blocking.api()).status;
        });
        ASSERT_TRUE(blocking.wait_until_blocked());
        const RunResult refused = run(t, plan, nominal, output_size(c));
        expect_failure(refused.status, refused.error, PRAENORA_STATUS_INVALID_STATE);
        const BatchResult batch = run_batch(t, plan, nominal, 1, output_size(c));
        expect_failure(batch.status, batch.error, PRAENORA_STATUS_INVALID_STATE);
        // Another plan of the session, and the session, are not.
        EXPECT_EQ(run(t, other_plan, nominal, output_size(c)).status, PRAENORA_STATUS_OK);
        EXPECT_EQ(describe_model(t, session).status, PRAENORA_STATUS_OK);
        worker.finish();
        EXPECT_EQ(first, PRAENORA_STATUS_OK);
    }
    // Once the calls returned, both work again.
    EXPECT_EQ(simulate(t, session, request).status, PRAENORA_STATUS_OK);
    EXPECT_EQ(run(t, plan, nominal, output_size(c)).status, PRAENORA_STATUS_OK);
}

TEST_P(SimulationService, CancelledCallsReturnCancelled) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    CheckHost cancelled;
    cancelled.cancel_from_poll(1);
    Session refused;
    expect_failure(open(t, c.model, options_of(c), refused, cancelled.api()),
                   PRAENORA_STATUS_CANCELLED);
    EXPECT_FALSE(refused);

    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    Plan plan;
    ASSERT_EQ(prepare(t, session, base_plan(c).dump(), plan).status, PRAENORA_STATUS_OK);
    const std::vector<double> nominal = concat(c.parameter_values, c.initial_state);
    expect_failure(describe_model(t, session, cancelled.api()), PRAENORA_STATUS_CANCELLED);
    expect_failure(simulate(t, session, base_request(c).dump(), cancelled.api()),
                   PRAENORA_STATUS_CANCELLED);
    Plan not_prepared;
    expect_failure(prepare(t, session, base_plan(c).dump(), not_prepared, cancelled.api()),
                   PRAENORA_STATUS_CANCELLED);
    Kernel kernel(t);
    expect_failure(open_kernel(t, session, "{}", kernel, cancelled.api()),
                   PRAENORA_STATUS_CANCELLED);
    const RunResult one = run(t, plan, nominal, output_size(c), cancelled.api());
    expect_failure(one.status, one.error, PRAENORA_STATUS_CANCELLED);
    const BatchResult batch = run_batch(t, plan, nominal, 1, output_size(c), cancelled.api());
    expect_failure(batch.status, batch.error, PRAENORA_STATUS_CANCELLED);

    // Between the sets of a batch.
    CheckHost later;
    later.cancel_from_poll(2);
    const BatchResult stopped = run_batch(t, plan, concat(concat(nominal, nominal), nominal), 3,
                                          3 * output_size(c), later.api());
    expect_failure(stopped.status, stopped.error, PRAENORA_STATUS_CANCELLED);

    // The session and the plan are not left busy.
    EXPECT_EQ(simulate(t, session, base_request(c).dump()).status, PRAENORA_STATUS_OK);
    EXPECT_EQ(run(t, plan, nominal, output_size(c)).status, PRAENORA_STATUS_OK);
}

TEST_P(SimulationService, SinksAreRequired) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    Plan plan;
    ASSERT_EQ(prepare(t, session, base_plan(c).dump(), plan).status, PRAENORA_STATUS_OK);
    const std::string model = c.model;
    const std::string options = options_of(c);
    const std::string request = base_request(c).dump();
    const std::string plan_json = base_plan(c).dump();
    const std::vector<double> nominal = concat(c.parameter_values, c.initial_state);
    std::vector<double> out(output_size(c));
    std::int32_t outcome = -1;
    praenora_sink no_write{nullptr, nullptr};

    for (praenora_sink* missing : {static_cast<praenora_sink*>(nullptr), &no_write}) {
        SCOPED_TRACE(missing == nullptr ? "NULL sink" : "sink without write");
        TextSink sink;
        EXPECT_EQ(t->describe(t->context, missing), PRAENORA_STATUS_INVALID_ARGUMENT);
        praenora_simulation_session* opened = nullptr;
        EXPECT_EQ(t->open_session(t->context, nullptr, bytes(model), bytes(options), &opened,
                                  missing),
                  PRAENORA_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(opened, nullptr);
        EXPECT_EQ(t->describe_model(session.get(), nullptr, missing, sink.get()),
                  PRAENORA_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(t->describe_model(session.get(), nullptr, sink.get(), missing),
                  PRAENORA_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(t->simulate(session.get(), nullptr, bytes(request), missing, sink.get()),
                  PRAENORA_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(t->simulate(session.get(), nullptr, bytes(request), sink.get(), missing),
                  PRAENORA_STATUS_INVALID_ARGUMENT);
        praenora_simulation_plan* prepared = nullptr;
        EXPECT_EQ(t->prepare(session.get(), nullptr, bytes(plan_json), &prepared, missing,
                             sink.get()),
                  PRAENORA_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(t->prepare(session.get(), nullptr, bytes(plan_json), &prepared, sink.get(),
                             missing),
                  PRAENORA_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(prepared, nullptr);
        EXPECT_EQ(t->run(plan.get(), nullptr, nominal.data(), nominal.size(), out.data(),
                         out.size(), &outcome, nullptr, missing),
                  PRAENORA_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(t->run_batch(plan.get(), nullptr, nominal.data(), nominal.size(), 1, out.data(),
                               out.size(), &outcome, missing),
                  PRAENORA_STATUS_INVALID_ARGUMENT);
        Kernel kernel(t);
        EXPECT_EQ(t->open_kernel(session.get(), nullptr, bytes("{}"), kernel.get(), missing),
                  PRAENORA_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(kernel->model, nullptr);
    }
    // A failure_json without a write function is refused too; NULL is allowed.
    TextSink error;
    EXPECT_EQ(t->run(plan.get(), nullptr, nominal.data(), nominal.size(), out.data(), out.size(),
                     &outcome, &no_write, error.get()),
              PRAENORA_STATUS_INVALID_ARGUMENT);
}

TEST_P(SimulationService, RefusedWritesEndTheCall) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    const praenora_status refusal = PRAENORA_STATUS_IO_ERROR; // what the sinks below return
    TextSink error;
    {
        TextSink refusing(refusal);
        EXPECT_EQ(t->describe(t->context, refusing.get()), refusal);
    }
    {
        TextSink refusing(refusal);
        EXPECT_EQ(t->describe_model(session.get(), nullptr, refusing.get(), error.get()),
                  refusal);
    }
    {
        TextSink refusing(refusal);
        const std::string request = base_request(c).dump();
        EXPECT_EQ(t->simulate(session.get(), nullptr, bytes(request), refusing.get(),
                              error.get()),
                  refusal);
    }
    {
        TextSink refusing(refusal);
        const std::string plan_json = base_plan(c).dump();
        praenora_simulation_plan* prepared = nullptr;
        EXPECT_EQ(t->prepare(session.get(), nullptr, bytes(plan_json), &prepared, refusing.get(),
                             error.get()),
                  refusal);
        EXPECT_EQ(prepared, nullptr) << "no plan when its info cannot be written";
    }
    {
        // An error document that cannot be written leaves the status as it is.
        TextSink refusing(refusal);
        const std::string request = R"({"bogus":1})";
        TextSink result;
        EXPECT_EQ(t->simulate(session.get(), nullptr, bytes(request), result.get(),
                              refusing.get()),
                  PRAENORA_STATUS_INVALID_ARGUMENT);
    }
    {
        Plan plan;
        ASSERT_EQ(prepare(t, session, base_plan(c).dump(), plan).status, PRAENORA_STATUS_OK);
        TextSink refusing(refusal);
        const std::vector<double> failing = concat(c.failing_parameters, c.initial_state);
        std::vector<double> out(output_size(c));
        std::int32_t outcome = -1;
        EXPECT_EQ(t->run(plan.get(), nullptr, failing.data(), failing.size(), out.data(),
                         out.size(), &outcome, refusing.get(), error.get()),
                  refusal);
    }
}

TEST_P(SimulationService, LimitsAreEnforced) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    {
        const std::string model(PRAENORA_SIMULATION_MAX_MODEL_SIZE + 1, ' ');
        Session session;
        expect_failure(open(t, model, options_of(c), session),
                       PRAENORA_STATUS_RESOURCE_EXHAUSTED);
    }
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    const std::string document(PRAENORA_SIMULATION_MAX_DOCUMENT_SIZE + 1, ' ');
    expect_failure(simulate(t, session, document), PRAENORA_STATUS_RESOURCE_EXHAUSTED);
    {
        Plan plan;
        expect_failure(prepare(t, session, document, plan), PRAENORA_STATUS_RESOURCE_EXHAUSTED);
        Kernel kernel(t);
        expect_failure(open_kernel(t, session, document, kernel),
                       PRAENORA_STATUS_RESOURCE_EXHAUSTED);
        Session other;
        expect_failure(open(t, c.model, document, other), PRAENORA_STATUS_RESOURCE_EXHAUSTED);
    }
    // More output values than a run may have: a grid of 2^22 + 1 rows, with the time column.
    json request = base_request(c);
    const double step = c.end_time / static_cast<double>(PRAENORA_SIMULATION_MAX_RUN_VALUES);
    request["output"] = {{"save_at", "regular_grid"}, {"step", step}, {"include_states", false}};
    expect_failure(simulate(t, session, request.dump()), PRAENORA_STATUS_RESOURCE_EXHAUSTED);
    Plan plan;
    expect_failure(prepare(t, session, request.dump(), plan), PRAENORA_STATUS_RESOURCE_EXHAUSTED);
}

TEST_P(SimulationService, PlansAndKernelsOutliveTheirSession) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    Session session = open_case(t, c);
    ASSERT_TRUE(session);
    const std::vector<double> before = flat_rows(simulated(t, session, base_request(c)));
    Plan plan;
    ASSERT_EQ(prepare(t, session, base_plan(c).dump(), plan).status, PRAENORA_STATUS_OK);
    Kernel kernel(t);
    ASSERT_EQ(open_kernel(t, session, json{{"parameters", c.parameters}}.dump(), kernel).status,
              PRAENORA_STATUS_OK);
    session.close();

    const RunResult result =
        run(t, plan, concat(c.parameter_values, c.initial_state), output_size(c));
    ASSERT_EQ(result.status, PRAENORA_STATUS_OK) << result.error;
    EXPECT_TRUE(same_values(result.out, before));
    std::vector<double> y(c.states.size(), kNaN), dydt(c.states.size(), kNaN);
    ASSERT_EQ(kernel->initial_state(kernel->model, y.data()), PRAENORA_STATUS_OK);
    ASSERT_EQ(kernel->rhs(kernel->model, 0.0, y.data(), dydt.data()), PRAENORA_STATUS_OK);
    expect_near(dydt, c.derivative(0.0, c.initial_state, c.parameter_values), c, "rhs");

    plan.close();
    t->close_kernel(kernel.get());
    EXPECT_EQ(kernel->model, nullptr) << "close_kernel sets model to NULL";
    t->close_kernel(nullptr);
    t->close_plan(nullptr);
    t->close_session(nullptr);
}

TEST_P(SimulationService, NullObjectsAreRefused) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    const Session none;
    expect_failure(describe_model(t, none), PRAENORA_STATUS_INVALID_ARGUMENT);
    expect_failure(simulate(t, none, base_request(c).dump()), PRAENORA_STATUS_INVALID_ARGUMENT);
    Plan plan;
    expect_failure(prepare(t, none, base_plan(c).dump(), plan), PRAENORA_STATUS_INVALID_ARGUMENT);
    Kernel kernel(t);
    expect_failure(open_kernel(t, none, "{}", kernel), PRAENORA_STATUS_INVALID_ARGUMENT);
    const Plan no_plan;
    const RunResult one =
        run(t, no_plan, concat(c.parameter_values, c.initial_state), output_size(c));
    expect_failure(one.status, one.error, PRAENORA_STATUS_INVALID_ARGUMENT);
    const BatchResult batch = run_batch(t, no_plan, {}, 0, 0);
    expect_failure(batch.status, batch.error, PRAENORA_STATUS_INVALID_ARGUMENT);
}

TEST_P(SimulationService, DistinctObjectsWorkConcurrently) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    constexpr std::size_t kThreads = 4;
    constexpr int kRounds = 10;
    const Session session = open_case(t, c);
    ASSERT_TRUE(session);
    const std::size_t size = output_size(c);
    const std::string request = base_request(c).dump();

    // One plan and one kernel per thread, and the results of a sequential pass.
    std::vector<Plan> plans(kThreads);
    std::vector<std::unique_ptr<Kernel>> kernels;
    std::vector<std::vector<double>> inputs, outputs, rates;
    const double time = c.end_time / 2;
    const std::vector<double> y = c.solution(0.0, time, c.parameter_values, c.initial_state);
    for (std::size_t i = 0; i < kThreads; ++i) {
        ASSERT_EQ(prepare(t, session, base_plan(c).dump(), plans[i]).status, PRAENORA_STATUS_OK);
        kernels.push_back(std::make_unique<Kernel>(t));
        ASSERT_EQ(open_kernel(t, session, json{{"parameters", c.parameters}}.dump(), *kernels[i])
                      .status,
                  PRAENORA_STATUS_OK);
        const double factor = 1.0 + 0.1 * static_cast<double>(i);
        inputs.push_back(concat(scaled(c.parameter_values, factor), c.initial_state));
        const RunResult result = run(t, plans[i], inputs[i], size);
        ASSERT_EQ(result.status, PRAENORA_STATUS_OK) << result.error;
        outputs.push_back(result.out);
        const std::vector<double> parameters = scaled(c.parameter_values, factor);
        ASSERT_EQ((*kernels[i])->set_parameters((*kernels[i])->model, parameters.data()),
                  PRAENORA_STATUS_OK);
        std::vector<double> dydt(c.states.size(), kNaN);
        std::vector<double> state = y;
        ASSERT_EQ((*kernels[i])->rhs((*kernels[i])->model, time, state.data(), dydt.data()),
                  PRAENORA_STATUS_OK);
        rates.push_back(dydt);
    }
    const std::string simulated_once = simulate(t, session, request).output;

    // The same work on kThreads threads at once, each with sessions of its own as well.
    std::vector<std::string> problems(kThreads);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            CheckHost host;
            Session own;
            const Call opened = open(t, c.model, options_of(c), own, host.api());
            if (opened.status != PRAENORA_STATUS_OK) {
                problems[i] = "open_session: " + opened.error;
                return;
            }
            for (int round = 0; round < kRounds; ++round) {
                if (simulate(t, own, request, host.api()).output != simulated_once) {
                    problems[i] = "simulate differs";
                    return;
                }
                const RunResult result = run(t, plans[i], inputs[i], size, host.api());
                if (result.status != PRAENORA_STATUS_OK || !same_values(result.out, outputs[i])) {
                    problems[i] = "run differs: " + result.error;
                    return;
                }
                const BatchResult batch =
                    run_batch(t, plans[i], concat(inputs[i], inputs[i]), 2, 2 * size, host.api());
                if (batch.status != PRAENORA_STATUS_OK ||
                    !same_values(batch.out, concat(outputs[i], outputs[i]))) {
                    problems[i] = "run_batch differs: " + batch.error;
                    return;
                }
                Kernel& kernel = *kernels[i];
                std::vector<double> state = y;
                std::vector<double> dydt(c.states.size(), kNaN);
                if (kernel->rhs(kernel->model, time, state.data(), dydt.data()) !=
                        PRAENORA_STATUS_OK ||
                    !same_values(dydt, rates[i])) {
                    problems[i] = "rhs differs";
                    return;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (std::size_t i = 0; i < kThreads; ++i) {
        EXPECT_EQ(problems[i], "") << "thread " << i;
    }
}

TEST_P(SimulationService, OneSessionUnderConcurrentCalls) {
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    constexpr std::size_t kThreads = 4;
    constexpr int kRounds = 20;
    Session session = open_case(t, c);
    ASSERT_TRUE(session);
    const std::string request = base_request(c).dump();
    const std::string expected = simulate(t, session, request).output;
    ASSERT_FALSE(expected.empty());

    // Calls racing on one session: each is served, or refused as busy; nothing else.
    std::vector<std::string> problems(kThreads);
    std::vector<int> served(kThreads, 0);
    {
        std::vector<std::thread> threads;
        for (std::size_t i = 0; i < kThreads; ++i) {
            threads.emplace_back([&, i] {
                for (int round = 0; round < kRounds && problems[i].empty(); ++round) {
                    const Call call = simulate(t, session, request);
                    if (call.status == PRAENORA_STATUS_OK && call.output == expected) {
                        ++served[i];
                    } else if (call.status != PRAENORA_STATUS_INVALID_STATE) {
                        problems[i] = std::string(praenora_status_name(call.status)) + ": " +
                                      call.error + call.output;
                    }
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
    }
    for (std::size_t i = 0; i < kThreads; ++i) {
        EXPECT_EQ(problems[i], "") << "thread " << i;
    }
    EXPECT_GT(std::accumulate(served.begin(), served.end(), 0), 0);

    // The session closed while its plans and kernels run on other threads.
    std::vector<Plan> plans(kThreads);
    std::vector<std::unique_ptr<Kernel>> kernels;
    const std::vector<double> nominal = concat(c.parameter_values, c.initial_state);
    const double time = c.end_time / 2;
    const std::vector<double> y = c.solution(0.0, time, c.parameter_values, c.initial_state);
    std::vector<double> reference_out, reference_rates(c.states.size(), kNaN);
    for (std::size_t i = 0; i < kThreads; ++i) {
        ASSERT_EQ(prepare(t, session, base_plan(c).dump(), plans[i]).status, PRAENORA_STATUS_OK);
        kernels.push_back(std::make_unique<Kernel>(t));
        ASSERT_EQ(open_kernel(t, session, "{}", *kernels[i]).status, PRAENORA_STATUS_OK);
    }
    reference_out = run(t, plans[0], nominal, output_size(c)).out;
    std::vector<double> state = y;
    ASSERT_EQ((*kernels[0])->rhs((*kernels[0])->model, time, state.data(), reference_rates.data()),
              PRAENORA_STATUS_OK);

    std::mutex mutex;
    std::condition_variable changed;
    std::size_t started = 0;
    bool closed = false;
    std::vector<std::string> late(kThreads);
    {
        std::vector<std::thread> threads;
        for (std::size_t i = 0; i < kThreads; ++i) {
            threads.emplace_back([&, i] {
                bool waited = false;
                for (int round = 0; round < kRounds; ++round) {
                    const RunResult result = run(t, plans[i], nominal, output_size(c));
                    std::vector<double> local = y;
                    std::vector<double> dydt(c.states.size(), kNaN);
                    Kernel& kernel = *kernels[i];
                    const praenora_status rhs =
                        kernel->rhs(kernel->model, time, local.data(), dydt.data());
                    if (result.status != PRAENORA_STATUS_OK ||
                        !same_values(result.out, reference_out) || rhs != PRAENORA_STATUS_OK ||
                        !same_values(dydt, reference_rates)) {
                        late[i] = "round " + std::to_string(round) + ": " + result.error;
                        break;
                    }
                    if (round == 0) {
                        // Wait until the session is closed, so later rounds run without it.
                        std::unique_lock<std::mutex> lock(mutex);
                        ++started;
                        waited = true;
                        changed.notify_all();
                        changed.wait(lock, [&] { return closed; });
                    }
                }
                if (!waited) { // failed in its first round: do not hold the others up
                    const std::lock_guard<std::mutex> lock(mutex);
                    ++started;
                    changed.notify_all();
                }
            });
        }
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return started == kThreads; });
            session.close();
            closed = true;
            changed.notify_all();
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
    }
    for (std::size_t i = 0; i < kThreads; ++i) {
        EXPECT_EQ(late[i], "") << "thread " << i;
    }
}

TEST_P(SimulationService, ShortHostApisAreRespected) {
    // A provider checks that the host's struct_size covers a callback before it calls it.
    const SimulationCase& c = GetParam();
    const praenora_simulation_v0* t = table();
    for (const std::size_t size : {offsetof(praenora_host_api, log),
                                   offsetof(praenora_host_api, cancel_requested)}) {
        SCOPED_TRACE("struct_size " + std::to_string(size));
        CheckHost host;
        host.cancel_from_poll(1); // a call that polls anyway is cancelled
        host.set_struct_size(size);
        Session session;
        ASSERT_EQ(open(t, c.model, options_of(c), session, host.api()).status, PRAENORA_STATUS_OK);
        EXPECT_EQ(describe_model(t, session, host.api()).status, PRAENORA_STATUS_OK);
        EXPECT_EQ(simulate(t, session, base_request(c).dump(), host.api()).status,
                  PRAENORA_STATUS_OK);
        Plan plan;
        ASSERT_EQ(prepare(t, session, base_plan(c).dump(), plan, host.api()).status,
                  PRAENORA_STATUS_OK);
        const std::vector<double> nominal = concat(c.parameter_values, c.initial_state);
        EXPECT_EQ(run(t, plan, nominal, output_size(c), host.api()).status, PRAENORA_STATUS_OK);
        EXPECT_EQ(run_batch(t, plan, nominal, 1, output_size(c), host.api()).status,
                  PRAENORA_STATUS_OK);
        Kernel kernel(t);
        EXPECT_EQ(open_kernel(t, session, "{}", kernel, host.api()).status, PRAENORA_STATUS_OK);
        EXPECT_EQ(host.polls(), 0);
        if (size <= offsetof(praenora_host_api, log)) {
            EXPECT_EQ(host.logged(), 0u);
        }
    }
}

} // namespace praenora::test
