#include "engine/envelope.hpp"

#include "document/schema_registry.hpp"

#include <praenora/runtime/error.hpp>

#include <utility>

namespace praenora::detail {

std::string fingerprint16(std::string_view fingerprint) {
    if (fingerprint.size() < 16) {
        throw Error(ErrorCode::INVALID_ARGUMENT,
                    "a run fingerprint has 64 hex digits; got '" + std::string(fingerprint) + "'");
    }
    return std::string(fingerprint.substr(0, 16));
}

nlohmann::json make_envelope(std::string_view fingerprint16, std::uint64_t tick,
                             double logical_time, const Endpoint& source, const std::string& type,
                             const SemVer& type_version, nlohmann::json data) {
    const std::string endpoint = source.to_string();
    nlohmann::json envelope = nlohmann::json::object();
    envelope["pmp_version"] = kPmpVersion;
    envelope["id"] = std::string(fingerprint16) + ":" + std::to_string(tick) + ":" + endpoint;
    envelope["type"] = type;
    envelope["type_version"] = to_string(type_version);
    envelope["source"] = endpoint;
    envelope["tick"] = tick;
    envelope["logical_time"] = logical_time;
    envelope["data"] = std::move(data);
    return envelope;
}

const CompiledSchema& envelope_schema() {
    static const CompiledSchema& schema =
        SchemaRegistry::builtin().for_kind("pmp-envelope", parse_semver(kPmpVersion));
    return schema;
}

} // namespace praenora::detail
