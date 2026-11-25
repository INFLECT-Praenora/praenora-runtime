// The loader of the Python core: what a provider that runs Python does in its start() to load the
// process's one Python core and start it (docs/specs/python-service.md, the providers that run
// Python). Part of the static library praenora_python_provider_support, linked into every such
// provider; it includes no Python header and links no Python, so a provider library loads in a
// process without the environment, and a missing or wrong environment is a start failure with a
// clear cause.
//
// The library is compiled with the environment the build uses and the core's build identity
// (PRAENORA_PYTHON_ENVIRONMENT_PACKAGE, PRAENORA_PYTHON_ENVIRONMENT_LOCK_SHA256,
// PRAENORA_PYTHON_CORE_BUILD_ID), which it gives the providers that link it too: a provider
// built in this tree starts only that environment and that core.
#ifndef PRAENORA_PROVIDERS_PYTHON_COMMON_LOADER_HPP
#define PRAENORA_PROVIDERS_PYTHON_COMMON_LOADER_HPP

#include "python_core.h"

#include <string>

namespace praenora::detail::python {

class Loader {
public:
    // Loads and starts the Python core for the provider whose library holds this code, with its
    // provider host `host`, in four steps:
    //   1. finds the provider library's own file, reads the environment file beside it
    //      (python-environment.json: {"environment": <dir>, "core": <file>}, each relative to the
    //      file's directory or absolute) and the environment's manifest
    //      (<environment>/praenora-runtime-pyenv.json), and checks that the environment's package
    //      and lock are the provider's, and that the core and the environment's libpython exist;
    //   2. unless the core is loaded in the process already: refuses a process that runs another
    //      Python interpreter (POSIX: a function Py_IsInitialized in the process's global scope
    //      that is not the environment's libpython's; Windows: a loaded python3*.dll that is not in
    //      the environment's Python directory): UNSUPPORTED, saying to run the step in a module
    //      host;
    //   3. unless the core is loaded already: loads the environment's libpython by its absolute
    //      path, into the global scope (dlopen(RTLD_NOW | RTLD_GLOBAL), which the extension
    //      modules of Linux need; Windows: LoadLibraryExW with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
    //      LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    //   4. loads the core by its absolute path (RTLD_NOW | RTLD_LOCAL; on Windows the flags of
    //      step 3, so its import of the Python DLL is the one loaded), checks its table and its
    //      build identity against the provider's (UNSUPPORTED, naming both, before anything else of
    //      it is called), and calls its start() with the environment's directory, the directory
    //      that holds praenora/sdk beside the core (<core dir>/python) and `host`.
    // Returns the core's table, started, or nullptr with *status set to the failure's status and
    // an error document {"code", "message", "details"} written to error_json (when it has a write
    // function; a start the core refused writes the core's own document). The causes: NOT_FOUND
    // for a missing file (the environment file, the environment's manifest, the core,
    // libpython), IO_ERROR for one that cannot be read, DATA_INVALID for one whose contents are
    // wrong, UNSUPPORTED for another environment, another core or another interpreter, INTERNAL
    // for a library that does not load; each message names the files and directories involved.
    // Idempotent: after a success, every later call returns the same table at once. Thread-safe
    // (calls take turns; the runtime starts providers one at a time anyway). Never throws.
    static const praenora_python_core_v0* start(const praenora_provider_host* host,
                                                praenora_sink* error_json,
                                                praenora_status* status) noexcept;

    // The directory of the provider library that holds this code, as start() finds the library's
    // own file in step 1: absolute, in UTF-8. Throws std::runtime_error, saying why, when the
    // system cannot tell.
    static std::string library_directory();
};

} // namespace praenora::detail::python

#endif // PRAENORA_PROVIDERS_PYTHON_COMMON_LOADER_HPP
