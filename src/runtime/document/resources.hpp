// Files embedded in the runtime at build time (praenora_embed_resources(), NAME praenora_runtime):
// the document schemas under "schemas/" (from spec/schemas/) and the protocol catalog under
// "catalog/" (from spec/catalog/).
#ifndef PRAENORA_RUNTIME_DOCUMENT_RESOURCES_HPP
#define PRAENORA_RUNTIME_DOCUMENT_RESOURCES_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace praenora::detail {

// The bytes of the resource at `path` ('/'-separated, for example
// "schemas/workflow-config.0.1.schema.json"), if it is embedded. The bytes live as long as the
// program.
std::optional<std::string_view> find_embedded_resource(std::string_view path);

// The sorted paths of the embedded resources that start with `prefix`.
std::vector<std::string> list_embedded_resources(std::string_view prefix);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_DOCUMENT_RESOURCES_HPP
