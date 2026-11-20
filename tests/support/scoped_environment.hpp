// An environment variable set (or removed) for the lifetime of an object, for tests of what the
// runtime reads from the environment, and the sanitizer options of the module hosts that a test
// makes crash (OwnSignals). Uses only the standard library and the operating system, so white-box
// and black-box tests alike can use it.
#ifndef PRAENORA_TESTS_SUPPORT_SCOPED_ENVIRONMENT_HPP
#define PRAENORA_TESTS_SUPPORT_SCOPED_ENVIRONMENT_HPP

#include <cstdlib>
#include <optional>
#include <string>

namespace praenora::test {

// Sets the environment variable `name` to `value`, or removes it for nullopt, and restores its
// earlier value (or its absence) when destroyed. Names and values are UTF-8. On Windows the
// process environment is changed with SetEnvironmentVariableW, which is where the runtime reads it
// (GetEnvironmentVariableW). Not thread-safe: tests change the environment one at a time.
class ScopedEnvironment {
public:
    ScopedEnvironment(std::string name, const std::optional<std::string>& value);
    ~ScopedEnvironment();
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
    std::string name_;
    std::optional<std::string> previous_;
};

// For a scope, the module hosts started meanwhile end by the signal a crash raises, also in a
// sanitizer build, whose handlers would otherwise report it and end the process another way, and
// without a leak check as they exit: options appended to ASAN_OPTIONS and TSAN_OPTIONS, which
// sanitizers read as a process starts (so this process is not affected).
class OwnSignals {
public:
    OwnSignals()
        : address_("ASAN_OPTIONS",
                   appended("ASAN_OPTIONS", "handle_segv=0:handle_abort=0:detect_leaks=0")),
          thread_("TSAN_OPTIONS", appended("TSAN_OPTIONS", "handle_segv=0:handle_abort=0")) {}
    OwnSignals(const OwnSignals&) = delete;
    OwnSignals& operator=(const OwnSignals&) = delete;

private:
    static std::string appended(const char* name, const char* options) {
        const char* value = std::getenv(name);
        return value != nullptr && value[0] != '\0' ? std::string(value) + ":" + options
                                                    : std::string(options);
    }

    ScopedEnvironment address_, thread_;
};

} // namespace praenora::test

#endif // PRAENORA_TESTS_SUPPORT_SCOPED_ENVIRONMENT_HPP
