#include "json_compare.hpp"

// The runtime's header-only comparison, the one implementation of the rule (an include directory
// private to this library).
#include "core/json_near.hpp"

namespace praenora::test {

bool json_near(const nlohmann::json& a, const nlohmann::json& b, double rel, double abs,
               std::string* where) {
    return praenora::detail::json_near(a, b, rel, abs, where);
}

} // namespace praenora::test
