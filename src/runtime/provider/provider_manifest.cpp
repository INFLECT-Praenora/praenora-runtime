#include "provider/provider_manifest.hpp"

#include "core/sha256.hpp"
#include "discovery/manifest_reader.hpp"

#include <praenora/runtime/error.hpp>

#include <cstddef>
#include <utility>

namespace praenora::detail {

ProviderManifest load_provider_manifest(const std::filesystem::path& file) {
    const ManifestReader reader(file, "provider-manifest", "provider manifest",
                                "provider manifests");
    const nlohmann::json& json = reader.json();

    ProviderManifest manifest;
    manifest.manifest_path = absolute_manifest_path(file);
    manifest.directory = manifest.manifest_path.parent_path();
    manifest.manifest_sha256 = sha256_hex(reader.bytes());

    manifest.name = json.at("provider").at("name").get<std::string>();
    manifest.version = reader.version_at("/provider/version");
    manifest.runtime_version = reader.version_at("/runtime_version");
    manifest.abi_major = reader.uint32_at("/abi/major");
    manifest.abi_minor = reader.uint32_at("/abi/minor");

    const auto& library = json.at("library");
    manifest.library_path =
        reader.relative_path_at("/library/path", manifest.directory, "library.path");
    if (library.contains("sha256")) {
        manifest.library_sha256 = library.at("sha256").get<std::string>();
    }

    manifest.os = json.at("platform").at("os").get<std::string>();
    manifest.arch = json.at("platform").at("arch").get<std::string>();

    const auto& services = json.at("services");
    for (std::size_t i = 0; i < services.size(); ++i) {
        const std::string pointer = "/services/" + std::to_string(i);
        ServiceVersion service{services[i].at("name").get<std::string>(),
                               reader.uint32_at(pointer + "/major"),
                               reader.uint32_at(pointer + "/minor")};
        for (std::size_t k = 0; k < manifest.services.size(); ++k) {
            const auto& earlier = manifest.services[k];
            if (earlier.name == service.name && earlier.major == service.major) {
                reader.fail(ErrorCode::MANIFEST_INVALID, pointer,
                            "service " + service.name + "@" + std::to_string(service.major) +
                                " is listed twice (also at /services/" + std::to_string(k) + ")");
            }
        }
        manifest.services.push_back(std::move(service));
    }

    manifest.engine = json.at("engine");
    return manifest;
}

} // namespace praenora::detail
