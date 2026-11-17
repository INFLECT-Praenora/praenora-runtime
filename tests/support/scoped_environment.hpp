// An environment variable set (or removed) for the lifetime of an object, for tests of what the
// runtime reads from the environment. Uses only the standard library and the operating system, so
// white-box and black-box tests alike can use it.
#ifndef PRAENORA_TESTS_SUPPORT_SCOPED_ENVIRONMENT_HPP
#define PRAENORA_TESTS_SUPPORT_SCOPED_ENVIRONMENT_HPP

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

} // namespace praenora::test

#endif // PRAENORA_TESTS_SUPPORT_SCOPED_ENVIRONMENT_HPP
