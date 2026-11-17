// Error records, and the JSON forms shared by the status snapshot (StatusSnapshot::to_json()) and
// the run record.
#ifndef PRAENORA_RUNTIME_OBSERVABILITY_STATUS_JSON_HPP
#define PRAENORA_RUNTIME_OBSERVABILITY_STATUS_JSON_HPP

#include <praenora/runtime/error.hpp>
#include <praenora/runtime/status.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>

namespace praenora::detail {

// The record of `error`: its code, message, location and details, the tick it happened in (if
// any) and when it was recorded. The cause is left empty.
ErrorRecord error_record_of(const Error& error, std::optional<std::uint64_t> tick,
                            std::int64_t at_unix_ns);

// An error record as status snapshots and run records write it: {"code", "name", "message",
// "tick" (null when unset), "at" (RFC 3339 UTC; null when unset), "cause", "location", "details"}.
// The cause is the module's error document when cause_json holds a JSON object, {"text":
// cause_json} for any other text, and null when cause_json is empty. The location holds the parts
// of `location` that are set ("file", "line", "column", "pointer"; a column only with a line), or
// is null when none is. The details are the JSON object of details_json, {"text": details_json}
// for any other text, and null when details_json is empty or an empty object.
nlohmann::json error_record_json(const ErrorRecord& record);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_OBSERVABILITY_STATUS_JSON_HPP
