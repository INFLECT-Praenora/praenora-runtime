// The JSON form of a source location: the one rule that Error::to_json() and the error records of
// status snapshots and run records (observability/status_json.hpp) both follow. Header-only, for
// any nlohmann JSON type, since the error document keeps its keys in order and the records sort
// them.
#ifndef PRAENORA_RUNTIME_CORE_LOCATION_JSON_HPP
#define PRAENORA_RUNTIME_CORE_LOCATION_JSON_HPP

#include <praenora/runtime/error.hpp>

namespace praenora::detail {

// The parts of `location` that are set, as an object {"file", "line", "column", "pointer"}: a file
// and a pointer when not empty, a line when above 0, and a column when above 0 and there is a
// line. An empty object when no part is set.
template <class Json> Json location_parts(const SourceLocation& location) {
    Json parts = Json::object();
    if (!location.file.empty()) {
        parts["file"] = location.file;
    }
    if (location.line > 0) {
        parts["line"] = location.line;
        if (location.column > 0) {
            parts["column"] = location.column;
        }
    }
    if (!location.pointer.empty()) {
        parts["pointer"] = location.pointer;
    }
    return parts;
}

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_CORE_LOCATION_JSON_HPP
