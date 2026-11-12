// consumer_app: an application built outside praenora-runtime against the installed runtime
// library (Praenora::Runtime).
//
//   consumer_app <module directory> <catalog directory>
//
// It looks for module consumer-echo 0.1.0 on the module directory, with the catalog directory
// added to the protocol catalog (for the test capability the module implements), loads its
// library and prints its describe document on standard output. Exit code 0 when it did, 1 on an
// error (described on standard error), 2 on wrong usage.
#include <praenora/runtime/error.hpp>
#include <praenora/runtime/options.hpp>
#include <praenora/runtime/runtime.hpp>

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: consumer_app <module directory> <catalog directory>\n";
        return 2;
    }
    try {
        praenora::RuntimeOptions options;
        options.module_paths = {argv[1]};
        options.catalog_paths = {argv[2]};
        const praenora::Runtime runtime(options);
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
