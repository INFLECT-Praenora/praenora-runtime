// praenora-module-host: runs one module instance for the Praenora runtime in a process of its own
// (docs/specs/process-isolation.md). Everything it does is the runtime's own code
// (isolation/module_host_server.hpp), but what it does first, before any thread starts: the
// variables of the Python environment, when its tree has the provider python
// (src/frontend/python_environment.hpp).
#include "isolation/module_host_server.hpp"

#include <python_environment.hpp>

#ifndef PRAENORA_PYTHON_ENVIRONMENT_FILE
#error "PRAENORA_PYTHON_ENVIRONMENT_FILE must be defined (src/module_host/CMakeLists.txt)"
#endif

int main(int argc, char** argv) {
    praenora::frontend::apply_python_environment(praenora::frontend::current_program_path(),
                                                 praenora::frontend::ProgramDirectory::libexec,
                                                 PRAENORA_PYTHON_ENVIRONMENT_FILE);
    return praenora::detail::run_module_host(argc, argv);
}
