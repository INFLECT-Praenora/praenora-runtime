// The acceptance rules of an invoke() result, shared by the engine (engine/engine.hpp) and the
// contract checks (api/module_check.hpp), so the checks accept exactly the results a run does.
#ifndef PRAENORA_RUNTIME_ENGINE_RESULT_CHECK_HPP
#define PRAENORA_RUNTIME_ENGINE_RESULT_CHECK_HPP

#include "catalog/catalog.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace praenora::detail {

// Why a result is not acceptable: what is wrong, in words that follow "it" or name an output (for
// example "it lacks the required output 'out'"), the JSON pointer into the result, and the
// details: {"raw_prefix"} (not JSON: its first 200 bytes), {"issues"} (pmp-result), {"port"} (an
// undeclared or missing output) or {"port", "type", "issues"} (data of the wrong type).
struct ResultProblem {
    std::string message, pointer;
    nlohmann::json details;
};

// An acceptable result: the data of each output it holds, by port (in port order), and its
// diagnostics object, if it has one.
struct AcceptedResult {
    std::map<std::string, nlohmann::json> outputs;
    std::optional<nlohmann::json> diagnostics;
};

// The message type of a port.
using PortTypeLookup = std::function<const MessageTypeDef&(const PortDef& port)>;

// The first `limit` bytes of `text`, without a UTF-8 sequence cut in two at the end.
std::string utf8_prefix(const std::string& text, std::size_t limit);

// Schema issues as JSON: [{"pointer", "message"}, ...], as error details list them.
nlohmann::json issues_json(const std::vector<SchemaIssue>& issues);

// Checks the text `result` that a step of `capability` returned, in this order: it is JSON, it
// satisfies pmp-result, it names only output ports of the capability, it holds every required
// one, and, with `validate_payloads`, each output's data satisfies its port's message type
// (`type_of`). Returns the first problem; nullopt, with `accepted` filled in, when the result is
// acceptable.
std::optional<ResultProblem> check_invoke_result(const std::string& result,
                                                 const CapabilityDef& capability,
                                                 bool validate_payloads,
                                                 const PortTypeLookup& type_of,
                                                 AcceptedResult& accepted);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_ENGINE_RESULT_CHECK_HPP
