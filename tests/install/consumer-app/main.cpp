// consumer_app: an application built outside praenora-runtime against the installed runtime
// library (Praenora::Runtime).
//
//   consumer_app <module directory> <catalog directory>
//   consumer_app <module directory> <catalog directory> <configuration> <output directory>
//
// With two arguments it looks for module consumer-echo 0.1.0 on the module directory, with the
// catalog directory added to the protocol catalog (for the test capability the module
// implements), loads its library and prints its describe document on standard output. With four
// it runs the workflow configuration with the same directories, writing the run's files into the
// output directory, and prints the run's final state, such as "completed". It sets no variable of
// its environment, as an application that embeds the runtime need not: what a module host of the
// installation sets itself shows in the run's outputs. Exit code 0 when it did, 1 on an error
// (described on standard error) or a run that did not complete, 2 on wrong usage.
#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/runtime.hpp>
#include <praenora/runtime/status.hpp>
#include <praenora/runtime/workflow.hpp>

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3 && argc != 5) {
        std::cerr << "usage: consumer_app <module directory> <catalog directory> "
                     "[<configuration> <output directory>]\n";
        return 2;
    }
    try {
        praenora::RuntimeOptions options;
        options.module_paths = {argv[1]};
        options.catalog_paths = {argv[2]};
        const praenora::Runtime runtime(options);
        if (argc == 5) {
            praenora::WorkflowOptions run;
            run.output_dir = argv[4];
            const auto workflow = runtime.open(argv[3], run);
            const praenora::RunSummary summary = workflow->run();
            std::cout << praenora::to_string(summary.final_state) << '\n';
            return summary.final_state == praenora::WorkflowState::completed ? 0 : 1;
        }
        const praenora::ModuleInfo module = runtime.inspect_module("consumer-echo", "0.1.0", true);
        std::cout << module.describe_json << '\n';
        return 0;
    } catch (const praenora::Error& error) {
        std::cerr << "consumer_app: " << error.what() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "consumer_app: unexpected error: " << error.what() << '\n';
    }
    return 1;
}
