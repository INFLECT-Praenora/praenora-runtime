// Runtime services: function tables the host offers to modules, looked up by name and major
// version through the host API's get_service().
#ifndef PRAENORA_RUNTIME_INSTANCE_SERVICES_HPP
#define PRAENORA_RUNTIME_INSTANCE_SERVICES_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace praenora::detail {

class ServiceRegistry {
public:
    // Registers `table`, a C struct that starts with praenora_api_header, as service `name` at
    // major version `major`. The table must outlive every instance that may use it. Throws
    // praenora::Error(INVALID_ARGUMENT) for an empty name, a NULL table, or a name and major that
    // are already registered. Register services before instances use the registry: add() must not
    // run while find() may.
    void add(std::string name, std::uint32_t major, const void* table);

    // The table registered as `name` at `major`, or nullptr. Thread-safe.
    const void* find(std::string_view name, std::uint32_t major) const noexcept;

private:
    std::map<std::string, std::map<std::uint32_t, const void*>, std::less<>> services_;
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_INSTANCE_SERVICES_HPP
