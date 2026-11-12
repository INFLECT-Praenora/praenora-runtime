#include "instance/services.hpp"

#include <praenora/runtime/error.hpp>

#include <utility>

namespace praenora::detail {

void ServiceRegistry::add(std::string name, std::uint32_t major, const void* table) {
    if (name.empty()) {
        throw Error(ErrorCode::INVALID_ARGUMENT, "a runtime service needs a name");
    }
    if (table == nullptr) {
        throw Error(ErrorCode::INVALID_ARGUMENT,
                    "runtime service " + name + "@" + std::to_string(major) + " has no table");
    }
    auto& majors = services_[name];
    if (!majors.emplace(major, table).second) {
        throw Error(ErrorCode::INVALID_ARGUMENT, "runtime service " + name + "@" +
                                                     std::to_string(major) +
                                                     " is already registered");
    }
}

const void* ServiceRegistry::find(std::string_view name, std::uint32_t major) const noexcept {
    const auto service = services_.find(name);
    if (service == services_.end()) {
        return nullptr;
    }
    const auto table = service->second.find(major);
    return table == service->second.end() ? nullptr : table->second;
}

} // namespace praenora::detail
