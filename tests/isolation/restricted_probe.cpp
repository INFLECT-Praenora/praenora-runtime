// The program of Isolation.ARestrictedModuleHostOfASandboxedRuntimeCannotStart, which starts it in
// a sandbox of its own (sandbox-exec, with a profile that allows everything): a runtime that runs
// in a sandbox already. It loads fx-sandbox-probe in a module host under the restricted profile,
// which that module host, sandboxed with it, cannot apply; and writes to its descriptor 3 one line
// of JSON: {"loaded": true}, or the load's error {"code", "message", "file", "details"}. Exit code
// 0 when it got that far, 1 otherwise.
#include "discovery/manifest.hpp"
#include "isolation/child_settings.hpp"
#include "loader/loaded_module.hpp"

#include "module_fixtures.hpp"

#include <praenora/runtime/error.hpp>

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <cstdio>
#include <exception>
#include <string>

int main() {
    nlohmann::json answer;
    try {
        praenora::detail::ChildSettings settings;
        settings.sandbox = praenora::detail::SandboxProfile::restricted;
        static_cast<void>(praenora::detail::LoadedModule::load_in_child(
            praenora::detail::load_manifest(praenora::test::fixture_manifest("fx-sandbox-probe")),
            settings));
        answer = {{"loaded", true}};
    } catch (const praenora::Error& error) {
        answer = {{"code", static_cast<int>(error.code())},
                  {"message", error.what()},
                  {"file", error.location().file},
                  {"details", nlohmann::json::parse(error.details_json(), nullptr, false)}};
    } catch (const std::exception& error) {
        std::fprintf(stderr, "restricted probe: %s\n", error.what());
        return 1;
    }
    const std::string line = answer.dump() + "\n";
    return ::write(3, line.data(), line.size()) == static_cast<ssize_t>(line.size()) ? 0 : 1;
}
