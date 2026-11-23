// The OpenAPI 3.1.0 document of the control API, generated from its route table (routes(),
// control_api.hpp) and committed as spec/openapi/praenora-control-api.openapi.json.
#ifndef PRAENORA_SERVER_OPENAPI_HPP
#define PRAENORA_SERVER_OPENAPI_HPP

#include <praenora/runtime/runtime.hpp>

#include <string>

namespace praenora::server {

// The document: info.version the API version (0.1.0), the security scheme "bearer" (HTTP bearer
// tokens), one operation per route with its operationId, summary, scope (x-praenora-scope:
// "none", "read" or "control"), parameters, request body and responses; components.schemas holds
// the API's own schemas, valid in JSON Schema draft-07 and 2020-12 alike (no "definitions", no
// "dependencies"), and, verbatim with their $schema and $id, the runtime's embedded
// status-snapshot, run-record and pmp-envelope schemas (Runtime::schema()). Written with keys
// sorted, two-space indentation and a final newline, the layout of spec/asyncapi/'s documents.
// Throws what Runtime::schema() throws.
std::string openapi_document(const Runtime& runtime);

} // namespace praenora::server

#endif // PRAENORA_SERVER_OPENAPI_HPP
