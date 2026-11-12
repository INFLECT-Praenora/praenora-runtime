// Envelopes: the PMP wrapper the runtime puts around every message. A module returns only the
// `data` of each output; the runtime adds the message's identity, type, source endpoint, tick and
// logical time. Envelopes never carry wall-clock time.
#ifndef PRAENORA_RUNTIME_ENGINE_ENVELOPE_HPP
#define PRAENORA_RUNTIME_ENGINE_ENVELOPE_HPP

#include "catalog/catalog.hpp"
#include "core/semver.hpp"
#include "document/schema.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace praenora::detail {

// The prefix of every envelope id of a run: the first 16 hex digits of its fingerprint. Throws
// praenora::Error(INVALID_ARGUMENT) for a fingerprint shorter than that.
std::string fingerprint16(std::string_view fingerprint);

// {"pmp_version", "id", "type", "type_version", "source", "tick", "logical_time", "data"}, where
// pmp_version is this runtime's PMP version, id is "<fingerprint16>:<tick>:<source>" (deterministic
// and unique within a run) and source is the endpoint the message comes from: "<step>.<port>" for a
// step's output, "inputs.<name>" for a workflow input.
nlohmann::json make_envelope(std::string_view fingerprint16, std::uint64_t tick,
                             double logical_time, const Endpoint& source, const std::string& type,
                             const SemVer& type_version, nlohmann::json data);

// The pmp-envelope schema of this runtime's PMP version (from the built-in schema registry).
const CompiledSchema& envelope_schema();

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_ENGINE_ENVELOPE_HPP
