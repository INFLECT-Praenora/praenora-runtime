// The objects the core keeps for modules and providers: created from a callable of the sources
// below a path, called with JSON documents, released (docs/specs/python-service.md). A path's code
// is fixed for the process, and a top-level package comes from below its own path only.
#ifndef PRAENORA_PROVIDERS_PYTHON_CORE_OBJECTS_HPP
#define PRAENORA_PROVIDERS_PYTHON_CORE_OBJECTS_HPP

#include "provider_abi.h"

#include <praenora/sdk/python.h>

#include <string>
#include <string_view>

namespace praenora::detail::python {

// Creates an object: create_object() of python_core.h, its arguments checked by the caller but
// for their contents. Throws Failure. The interpreter must be started.
praenora_python_object* create_object(const praenora_provider_host* provider_host,
                                      const praenora_host_api* module_host, std::string_view path,
                                      std::string_view path_sha256, std::string_view name,
                                      std::string_view args_json);

// Calls a method: the result document, within the size limit. Throws Failure.
std::string call_object(praenora_python_object& object, const praenora_host_api* module_host,
                        std::string_view method, std::string_view args_json);

// Releases an object; null does nothing.
void release_object(praenora_python_object* object) noexcept;

} // namespace praenora::detail::python

#endif // PRAENORA_PROVIDERS_PYTHON_CORE_OBJECTS_HPP
