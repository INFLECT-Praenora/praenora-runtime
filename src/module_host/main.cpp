// praenora-module-host: runs one module instance for the Praenora runtime in a process of its own
// (docs/specs/process-isolation.md). Everything it does is the runtime's own code
// (isolation/module_host_server.hpp).
#include "isolation/module_host_server.hpp"

int main(int argc, char** argv) { return praenora::detail::run_module_host(argc, argv); }
