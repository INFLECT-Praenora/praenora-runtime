// JSON forms shared by the status snapshot (StatusSnapshot::to_json()) and the run record.
#ifndef PRAENORA_RUNTIME_OBSERVABILITY_STATUS_JSON_HPP
#define PRAENORA_RUNTIME_OBSERVABILITY_STATUS_JSON_HPP

#include <praenora/runtime/status.hpp>

#include <nlohmann/json.hpp>

namespace praenora::detail {

// An error record as status snapshots and run records write it: {"code", "name", "message",
// "tick" (null when unset), "at" (RFC 3339 UTC; null when unset), "cause"}. The cause is the
// module's error document when cause_json holds a JSON object, {"text": cause_json} for any other
// text, and null when cause_json is empty.
nlohmann::json error_record_json(const ErrorRecord& record);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_OBSERVABILITY_STATUS_JSON_HPP
