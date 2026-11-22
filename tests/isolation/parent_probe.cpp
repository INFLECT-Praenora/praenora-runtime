// The program of Isolation.AModuleHostWhoseRuntimeDiesEnds: it starts a module host for fx-slow,
// creates and initializes an instance there, writes the module host's process id as a line to its
// own descriptor 3, and then invokes the instance with a call that sleeps for a minute without
// looking at cancellation. The test kills this program meanwhile; the module host must then end on
// its own.
#include "discovery/manifest.hpp"
#include "instance/module_instance.hpp"
#include "loader/loaded_module.hpp"

#include "instance_fixtures.hpp"
#include "module_fixtures.hpp"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <exception>
#include <string>

int main() {
    using praenora::detail::CallOutcome;
    try {
        const auto module = praenora::detail::LoadedModule::load_in_child(
            praenora::detail::load_manifest(praenora::test::fixture_manifest("fx-slow")), {});
        praenora::detail::InstanceOptions options;
        options.step = "s";
        options.invoke_timeout = std::chrono::seconds(120);
        options.cancel_grace = std::chrono::seconds(5);
        praenora::detail::ModuleInstance instance(module, options);
        if (instance.create(praenora::test::create_document(
                                "fx-slow", {{"sleep_ms", 60000}, {"cooperative", false}}))
                    .outcome != CallOutcome::succeeded ||
            instance.initialize(praenora::test::initialize_document()).outcome !=
                CallOutcome::succeeded) {
            std::fputs("parent probe: the instance did not start\n", stderr);
            return 1;
        }
        const std::string line = std::to_string(module->code().process_id().value_or(0)) + "\n";
        if (::write(3, line.data(), line.size()) != static_cast<ssize_t>(line.size())) {
            return 1;
        }
        static_cast<void>(instance.invoke(praenora::test::invocation_document(0), 0));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "parent probe: %s\n", error.what());
        return 1;
    }
}
