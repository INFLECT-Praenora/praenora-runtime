// fx-crash: ends its own process on purpose, for the tests of module hosts. Its parameter `when`
// names the call that does it: "create", "initialize", "invoke", "finalize", "destroy" (the
// instance's destructor) or "diagnostics"; "never" (the default) never does. Its parameter `how`
// says how: "segv" (the default: raises SIGSEGV), "abort" (abort()) or "exit" (exit(0)). Only for
// a module host: in a test's own process it would end the test. invoke returns
// {"outputs":{"out":{"data":{}}}}. More parameters:
// - crash_after_ms: the call that ends the process first sleeps this long, not looking at
//   cancellation;
// - skip_invocations (default 0): with `when` "invoke", that many invocations return as usual
//   first, so a run's step crashes in a later tick;
// - fail_invoke (default false): invoke fails with FAILED instead of returning its outputs, so a
//   run that fails asks the instance for its diagnostics (with `when` "diagnostics", the crash);
// - spawn (default "none"): invoke first starts a process, and logs "fx-crash: child <pid>" at
//   level info: "sleep" starts `sleep 120`, "fork" forks a child that only waits for signals, and
//   "write-3" runs `/bin/sh -c 'printf garbage >&3'` and waits for it (it writes onto the channel
//   of a module host, if it inherited it);
// - log_later_ms: invoke also starts a thread that logs "fx-crash: later" at level info that many
//   milliseconds later, so a module thread logs between calls; destroy waits for it;
// - exit_blocked (default false): destroy leaves a thread behind that holds the lock of standard
//   output for good, so a process that flushes its streams as it exits never gets to exit;
// - print: each invoke writes this text to standard output through its stdio buffer, which it does
//   not flush, so the text reaches the file or pipe only when the process flushes its streams.
#include <praenora/sdk/module.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>

#ifndef _WIN32
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ; // NOLINT: the process environment, declared by POSIX
#endif

namespace {

namespace sdk = praenora::sdk;

[[noreturn]] void end_process(const std::string& how) {
    if (how == "abort") {
        std::abort();
    }
    if (how == "exit") {
        std::exit(0);
    }
    std::raise(SIGSEGV);
    std::abort(); // not reached: the default action of SIGSEGV ends the process
}

class CrashModule final : public sdk::Module {
public:
    explicit CrashModule(const sdk::CreateContext& context) : host_(context.host) {
        read_parameters(context.create_json);
        crash_in("create");
    }

    ~CrashModule() override {
        if (later_.joinable()) {
            later_.join();
        }
        if (exit_blocked_) {
            block_exit();
        }
        crash_in("destroy");
    }

    void initialize(const sdk::InitializeContext&) override { crash_in("initialize"); }

    std::string invoke(std::string_view) override {
        start_child();
        if (invocations_++ >= skip_invocations_) {
            crash_in("invoke");
        }
        if (fail_invoke_) {
            throw sdk::Error(PRAENORA_STATUS_FAILED, "fx-crash: invoke fails, as asked");
        }
        if (!print_.empty()) {
            static_cast<void>(std::fputs(print_.c_str(), stdout));
        }
        if (log_later_ms_ >= 0 && !later_.joinable()) {
            const sdk::Host host = host_;
            const std::int64_t delay = log_later_ms_;
            later_ = std::thread([host, delay] {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                host.log(sdk::LogLevel::info, "fx-crash: later");
            });
        }
        return R"({"outputs":{"out":{"data":{}}}})";
    }

    void finalize() override { crash_in("finalize"); }

    std::string diagnostics() const override {
        crash_in("diagnostics");
        return "{}";
    }

private:
    void crash_in(const char* call) const {
        if (when_ == call) {
            std::this_thread::sleep_for(std::chrono::milliseconds(crash_after_ms_));
            end_process(how_);
        }
    }

    // What the parameter `spawn` asks for.
    void start_child() const {
#ifndef _WIN32
        if (spawn_ == "none") {
            return;
        }
        pid_t child = 0;
        if (spawn_ == "fork") {
            child = ::fork();
            if (child == 0) {
                for (;;) {
                    ::pause(); // only async-signal-safe calls after fork() in a threaded process
                }
            }
        } else {
            char sleep_program[] = "sleep";
            char sleep_time[] = "120";
            char shell[] = "/bin/sh";
            char command_option[] = "-c";
            char command[] = "printf garbage >&3 2>/dev/null";
            char* sleep_argv[] = {sleep_program, sleep_time, nullptr};
            char* shell_argv[] = {shell, command_option, command, nullptr};
            const bool writes = spawn_ == "write-3";
            if (::posix_spawnp(&child, writes ? shell : sleep_program, nullptr, nullptr,
                               writes ? shell_argv : sleep_argv, environ) != 0) {
                throw sdk::Error(PRAENORA_STATUS_FAILED, "fx-crash: cannot start a process");
            }
            if (writes) {
                int status = 0;
                static_cast<void>(::waitpid(child, &status, 0));
            }
        }
        if (child < 0) {
            throw sdk::Error(PRAENORA_STATUS_FAILED, "fx-crash: cannot fork");
        }
        host_.log(sdk::LogLevel::info, "fx-crash: child " + std::to_string(child));
#endif
    }

    static void block_exit() {
#ifndef _WIN32
        std::thread([] {
            flockfile(stdout);
            for (;;) {
                std::this_thread::sleep_for(std::chrono::hours(1));
            }
        }).detach();
        // Wait until the thread holds the lock.
        while (ftrylockfile(stdout) == 0) {
            funlockfile(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
#endif
    }

    [[noreturn]] static void invalid(const std::string& what) {
        throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT, "fx-crash: " + what);
    }

    void read_parameters(std::string_view create_json) {
        nlohmann::json parameters;
        try {
            const auto document = nlohmann::json::parse(create_json.begin(), create_json.end());
            if (document.is_object() && document.contains("parameters")) {
                parameters = document.at("parameters");
            }
        } catch (const nlohmann::json::exception& e) {
            invalid(std::string("the create document is not valid JSON: ") + e.what());
        }
        if (parameters.is_null()) {
            return;
        }
        if (!parameters.is_object()) {
            invalid("parameters must be an object");
        }
        when_ = parameters.value("when", when_);
        how_ = parameters.value("how", how_);
        log_later_ms_ = parameters.value("log_later_ms", log_later_ms_);
        exit_blocked_ = parameters.value("exit_blocked", exit_blocked_);
        crash_after_ms_ = parameters.value("crash_after_ms", crash_after_ms_);
        spawn_ = parameters.value("spawn", spawn_);
        skip_invocations_ = parameters.value("skip_invocations", skip_invocations_);
        fail_invoke_ = parameters.value("fail_invoke", fail_invoke_);
        print_ = parameters.value("print", print_);
    }

    sdk::Host host_;
    std::string when_ = "never";
    std::string how_ = "segv";
    std::int64_t log_later_ms_ = -1;
    bool exit_blocked_ = false;
    std::int64_t crash_after_ms_ = 0;
    std::string spawn_ = "none";
    std::int64_t skip_invocations_ = 0;
    std::int64_t invocations_ = 0;
    bool fail_invoke_ = false;
    std::string print_;
    std::thread later_;
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(CrashModule)
