#include "loader/loaded_module.hpp"

#include "core/paths.hpp"
#include "core/semver.hpp"
#include "core/sha256.hpp"
#include "document/json_util.hpp"
#include "document/schema_registry.hpp"
#include "loader/output_sink.hpp"
#include "platform/platform.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/sdk/status.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace praenora::detail {

namespace fs = std::filesystem;

namespace {

// describe() documents are small; this bounds what a broken module can make the host keep.
constexpr std::size_t kMaxDescribeBytes = std::size_t{1} << 20;

// Raises errors about one manifest's module, located in the manifest.
class Failure {
public:
    explicit Failure(const ModuleManifest& manifest)
        : module_(manifest.name + "@" + to_string(manifest.version)),
          file_(path_to_utf8(manifest.manifest_path)) {}

    [[noreturn]] void operator()(ErrorCode code, const std::string& message,
                                 const std::string& pointer = "",
                                 std::string details_json = "{}") const {
        SourceLocation location;
        location.file = file_;
        location.pointer = pointer;
        throw Error(code, "module " + module_ + ": " + message, std::move(location),
                    std::move(details_json));
    }

private:
    std::string module_;
    std::string file_;
};

std::string status_text(praenora_status status) {
    return std::to_string(status) + " (" + praenora_status_name(status) + ")";
}

// The names of the required v0 functions that `api` lacks.
std::vector<std::string> missing_functions(const praenora_module_api_v0& api) {
    std::vector<std::string> missing;
    const auto check = [&missing](bool present, const char* name) {
        if (!present) {
            missing.emplace_back(name);
        }
    };
    check(api.describe != nullptr, "describe");
    check(api.create != nullptr, "create");
    check(api.initialize != nullptr, "initialize");
    check(api.invoke != nullptr, "invoke");
    check(api.finalize != nullptr, "finalize");
    check(api.destroy != nullptr, "destroy");
    return missing;
}

std::string joined(const std::vector<std::string>& items) {
    std::string text;
    for (const auto& item : items) {
        text += (text.empty() ? "" : ", ") + item;
    }
    return text;
}

// Capabilities in a canonical order, for comparing the manifest's with describe()'s.
std::vector<CapabilityRef> sorted(std::vector<CapabilityRef> capabilities) {
    std::sort(capabilities.begin(), capabilities.end(),
              [](const CapabilityRef& a, const CapabilityRef& b) {
                  return a.id != b.id ? a.id < b.id : a.version < b.version;
              });
    return capabilities;
}

std::string capability_list(const std::vector<CapabilityRef>& capabilities) {
    std::vector<std::string> names;
    for (const auto& capability : capabilities) {
        names.push_back(capability.id + "@" + to_string(capability.version));
    }
    return "[" + joined(names) + "]";
}

bool same_capabilities(const std::vector<CapabilityRef>& a, const std::vector<CapabilityRef>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].version != b[i].version) {
            return false;
        }
    }
    return true;
}

// Runs describe() and checks its document against the pmp-describe schema.
nlohmann::json describe_document(const praenora_module_api_v0& api, const Failure& fail) {
    OutputSink out(kMaxDescribeBytes);
    const praenora_status status = api.describe(out.sink());
    if (status != PRAENORA_STATUS_OK) {
        fail(ErrorCode::MODULE_IDENTITY_MISMATCH,
             "describe() failed with status " + status_text(status) +
                 (out.refused()
                      ? "; its output exceeded " + std::to_string(kMaxDescribeBytes) + " bytes"
                      : std::string()));
    }
    nlohmann::json document;
    try {
        document = parse_json_text(out.text(), "describe()");
    } catch (const Error& error) {
        fail(ErrorCode::MODULE_IDENTITY_MISMATCH,
             std::string("describe() did not write JSON: ") + error.what());
    }
    const auto& schema =
        SchemaRegistry::builtin().for_kind("pmp-describe", parse_semver(kPmpVersion));
    const auto issues = schema.validate(document);
    if (!issues.empty()) {
        nlohmann::json list = nlohmann::json::array();
        for (const auto& issue : issues) {
            list.push_back({{"pointer", issue.pointer}, {"message", issue.message}});
        }
        const auto& first = issues.front();
        fail(ErrorCode::MODULE_IDENTITY_MISMATCH,
             "describe() wrote a document that violates the pmp-describe schema: " +
                 (first.pointer.empty() ? std::string("(root)") : first.pointer) + ": " +
                 first.message,
             "", nlohmann::json{{"issues", std::move(list)}}.dump());
    }
    return document;
}

// A version describe() reports. The schema's pattern admits numbers that do not fit in 64 bits,
// which cannot be compared: such a describe() gives no usable identity either.
SemVer describe_version(const std::string& text, const std::string& what, const Failure& fail) {
    try {
        return parse_semver(text);
    } catch (const Error& error) {
        fail(ErrorCode::MODULE_IDENTITY_MISMATCH,
             "describe() reports " + what + " '" + text +
                 "', which is not a usable semantic version: " + error.what());
    }
}

// Compares describe()'s identity with the manifest's.
void check_identity(const ModuleManifest& manifest, const nlohmann::json& describe,
                    const Failure& fail) {
    std::vector<std::string> differences;
    nlohmann::json details = nlohmann::json::array();
    const auto differ = [&](const char* field, const std::string& in_manifest,
                            const std::string& in_describe) {
        differences.push_back(std::string(field) + " (manifest " + in_manifest + ", describe() " +
                              in_describe + ")");
        details.push_back({{"field", field}, {"manifest", in_manifest}, {"describe", in_describe}});
    };

    const std::string name = describe.at("name").get<std::string>();
    if (name != manifest.name) {
        differ("name", manifest.name, name);
    }
    const SemVer version =
        describe_version(describe.at("version").get<std::string>(), "version", fail);
    if (version != manifest.version) {
        differ("version", to_string(manifest.version), to_string(version));
    }
    const nlohmann::json& abi_major = describe.at("abi").at("major");
    if (!abi_major.is_number_unsigned() || abi_major.get<std::uint64_t>() != manifest.abi_major) {
        differ("abi.major", std::to_string(manifest.abi_major), abi_major.dump());
    }
    std::vector<CapabilityRef> described;
    for (const auto& capability : describe.at("capabilities")) {
        const std::string id = capability.at("id").get<std::string>();
        described.push_back({id, describe_version(capability.at("version").get<std::string>(),
                                                  "the version of capability " + id, fail)});
    }
    const auto expected = sorted(manifest.capabilities);
    described = sorted(std::move(described));
    if (!same_capabilities(expected, described)) {
        differ("capabilities", capability_list(expected), capability_list(described));
    }

    if (!differences.empty()) {
        fail(ErrorCode::MODULE_IDENTITY_MISMATCH,
             "describe() disagrees with the manifest: " + joined(differences), "",
             nlohmann::json{{"differences", std::move(details)}}.dump());
    }
}

} // namespace

LoadedModule::LoadedModule(ModuleManifest manifest, SharedLibrary library,
                           const praenora_module_api_v0& api, nlohmann::json describe,
                           std::string library_sha256)
    : manifest_(std::move(manifest)), library_(std::move(library)), api_(api),
      describe_(std::move(describe)), library_sha256_(std::move(library_sha256)) {}

std::string check_library_file(const ModuleManifest& manifest) {
    const Failure fail(manifest);
    const std::string library_file = path_to_utf8(manifest.library_path);
    std::error_code ec;
    if (!fs::is_regular_file(manifest.library_path, ec)) {
        fail(ErrorCode::LIBRARY_NOT_FOUND,
             "the module library '" + library_file + "' does not exist or is not a file",
             "/library/path");
    }
    std::string digest;
    try {
        digest = sha256_file_hex(manifest.library_path);
    } catch (const Error& error) {
        // Unreadable (for example no permission): located like the other library errors.
        fail(error.code(), "cannot hash the module library: " + std::string(error.what()),
             "/library/path", error.details_json());
    }
    if (manifest.library_sha256 && *manifest.library_sha256 != digest) {
        fail(ErrorCode::LIBRARY_DIGEST_MISMATCH,
             "the module library '" + library_file + "' has SHA-256 " + digest +
                 ", but the manifest requires " + *manifest.library_sha256,
             "/library/sha256");
    }
    return digest;
}

praenora_module_api_v0 copy_module_table(const praenora_api_header& header) noexcept {
    // After the required part, the table holds only function pointers, all of one size.
    constexpr std::size_t kBase = PRAENORA_MODULE_API_V0_BASE_SIZE;
    constexpr std::size_t kFunction = sizeof(praenora_module_api_v0::diagnostics);
    static_assert(offsetof(praenora_module_api_v0, diagnostics) == kBase &&
                      offsetof(praenora_module_api_v0, self_test) == kBase + kFunction &&
                      sizeof(praenora_module_api_v0) == kBase + 2 * kFunction,
                  "the optional part of the table is whole function pointers");
    const std::size_t covered =
        std::min<std::size_t>(header.struct_size, sizeof(praenora_module_api_v0));
    const std::size_t size =
        covered <= kBase ? covered : kBase + (covered - kBase) / kFunction * kFunction;
    praenora_module_api_v0 api{};
    std::memcpy(&api, &header, size);
    return api;
}

std::shared_ptr<LoadedModule> LoadedModule::load(const ModuleManifest& manifest) {
    return load(manifest, nullptr);
}

std::shared_ptr<LoadedModule> LoadedModule::load(const ModuleManifest& manifest, Check* reached) {
    const Failure fail(manifest);
    const std::string library_file = path_to_utf8(manifest.library_path);
    const auto reach = [reached](Check check) {
        if (reached != nullptr) {
            *reached = check;
        }
    };

    // 1. The file and its digest, before any of its code runs.
    reach(Check::library_file);
    const std::string digest = check_library_file(manifest);

    // 2. The library.
    reach(Check::library_open);
    std::optional<SharedLibrary> opened;
    try {
        opened.emplace(SharedLibrary::open(manifest.library_path, manifest.dependency_paths));
    } catch (const Error& error) {
        SourceLocation location;
        location.file = path_to_utf8(manifest.manifest_path);
        location.pointer = "/library/path";
        throw Error(error.code(), error.what(), std::move(location), error.details_json());
    }
    SharedLibrary library = std::move(*opened);
    if (const auto keep = environment_variable("PRAENORA_DEBUG_KEEP_LIBRARIES");
        keep && *keep == "1") {
        library.pin();
    }

    // 3.-4. The entry point and the table it returns for ABI major 0.
    reach(Check::entry_point);
    void* const entry = library.symbol(PRAENORA_MODULE_ENTRY_POINT);
    if (entry == nullptr) {
        fail(ErrorCode::ENTRY_POINT_MISSING,
             "the library '" + library_file + "' does not export " + PRAENORA_MODULE_ENTRY_POINT);
    }
    praenora_module_get_api_fn get_api = nullptr;
    static_assert(sizeof(get_api) == sizeof(entry), "code and data pointers have one size");
    std::memcpy(&get_api, &entry, sizeof get_api);
    reach(Check::table);
    const praenora_api_header* header = get_api(PRAENORA_ABI_MAJOR);
    if (header == nullptr) {
        fail(ErrorCode::API_TABLE_INVALID,
             std::string(PRAENORA_MODULE_ENTRY_POINT) + "(" + std::to_string(PRAENORA_ABI_MAJOR) +
                 ") returned NULL: the module has no function table for ABI major " +
                 std::to_string(PRAENORA_ABI_MAJOR));
    }

    // 5.-7. The header, then the required functions.
    if (header->abi_major != PRAENORA_ABI_MAJOR) {
        fail(ErrorCode::ABI_INCOMPATIBLE,
             "the function table reports ABI major " + std::to_string(header->abi_major) +
                 "; this runtime implements ABI major " + std::to_string(PRAENORA_ABI_MAJOR));
    }
    if (header->reserved != 0) {
        fail(ErrorCode::API_TABLE_INVALID, "the function table's header.reserved is " +
                                               std::to_string(header->reserved) + "; it must be 0");
    }
    if (header->struct_size < PRAENORA_MODULE_API_V0_BASE_SIZE) {
        fail(ErrorCode::API_TABLE_INVALID, "the function table's struct_size " +
                                               std::to_string(header->struct_size) + " is below " +
                                               std::to_string(PRAENORA_MODULE_API_V0_BASE_SIZE) +
                                               ", the size of the ABI v0 required functions");
    }
    const praenora_module_api_v0 api = copy_module_table(*header);
    reach(Check::required_functions);
    const auto missing = missing_functions(api);
    if (!missing.empty()) {
        fail(ErrorCode::API_TABLE_INVALID, std::string("the function table's required function") +
                                               (missing.size() > 1 ? "s " : " ") + joined(missing) +
                                               (missing.size() > 1 ? " are" : " is") + " NULL");
    }

    // 8.-9. The identity.
    reach(Check::describe);
    nlohmann::json describe = describe_document(api, fail);
    reach(Check::identity);
    check_identity(manifest, describe, fail);

    return std::shared_ptr<LoadedModule>(
        new LoadedModule(manifest, std::move(library), api, std::move(describe), digest));
}

const praenora_module_api_v0& LoadedModule::api() const noexcept { return api_; }

const nlohmann::json& LoadedModule::describe() const noexcept { return describe_; }

const std::string& LoadedModule::library_sha256() const noexcept { return library_sha256_; }

const ModuleManifest& LoadedModule::manifest() const noexcept { return manifest_; }

void LoadedModule::pin() noexcept { library_.pin(); }

bool LoadedModule::pinned() const noexcept { return library_.pinned(); }

} // namespace praenora::detail
