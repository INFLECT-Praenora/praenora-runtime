// praenora_python_print: loads a provider of praenora.python@0 through the runtime's provider
// loader, and prints what the provider's describe and the service's describe write, verbatim, and
// what one method of one Python object returns, so that two processes can be compared byte for
// byte (the interpreter's hash seed is 0, so its hashes of strings are the same in every process).
//
//   praenora_python_print <provider manifest> <python sources directory> <package.module:Class>
//                         <method>
//
// The output: "provider <document>", "service <document>", then "create <Class>" and
// "call <method>: <result>" for the object created from the sources (with the argument null) and
// its method called with null.
//
// Its provider mode loads a provider whose typed service table is implemented in Python, such as
// the fixture provider test-python-echo; the service is test.echo@0 (tests/support/test_echo.h),
// the only one it knows:
//
//   praenora_python_print <provider manifest> --provider test.echo@0 <text>
//
// The output: "provider <document>", "service {"name", "major", "minor", "struct_size"}" (the
// table's header), then, for <text>, for no bytes and for bytes that are not text (a NUL and bytes
// that are not UTF-8 among them), "call echo: {"input": <hex>, "output": <hex>}", the bytes given
// and the bytes echoed in lowercase hexadecimal.
//
// Exit code 0 when every call succeeded (and, in the provider mode, every echo returned its
// input), 1 otherwise, with the problem on standard error.
#include "provider/provider_loader.hpp"
#include "provider/provider_manifest.hpp"

#include "test_echo.h"
#include "text_sink.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/sdk/python.h>

#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using praenora::test::TextSink;

praenora_bytes bytes(std::string_view text) { return praenora_bytes{text.data(), text.size()}; }

void print(std::string_view text) { std::fwrite(text.data(), 1, text.size(), stdout); }

// The bytes of `text` in lowercase hexadecimal.
std::string hex(std::string_view text) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0f]);
    }
    return out;
}

// The provider mode, for test.echo@0.
int run_echo(const std::filesystem::path& manifest_path, const std::string& text) {
    const praenora::detail::ProviderManifest manifest =
        praenora::detail::load_provider_manifest(manifest_path);
    const praenora::detail::LoadedProvider& provider = praenora::detail::load_provider(
        manifest, {praenora::detail::ServiceVersion{PRAENORA_TEST_ECHO_SERVICE, 0, 0}},
        std::nullopt, nullptr);
    print("provider " + provider.describe().dump() + "\n");
    const auto* table =
        static_cast<const praenora_test_echo_v0*>(provider.service(PRAENORA_TEST_ECHO_SERVICE, 0));
    if (table == nullptr || table->echo == nullptr) {
        throw std::runtime_error("the provider offers no test.echo@0 table with an echo");
    }
    print("service {\"name\":\"" PRAENORA_TEST_ECHO_SERVICE "\",\"major\":" +
          std::to_string(table->header.abi_major) +
          ",\"minor\":" + std::to_string(table->header.abi_minor) +
          ",\"struct_size\":" + std::to_string(table->header.struct_size) + "}\n");
    const std::string not_text = std::string(1, '\0') + "\xff\xfe d\xC3\xA9j\xC3\xA0 \x80";
    for (const std::string& input : {text, std::string(), not_text}) {
        TextSink output;
        const praenora_status status =
            table->echo(table->context, nullptr, bytes(input), output.get());
        if (status != PRAENORA_STATUS_OK) {
            throw std::runtime_error("echo of " + hex(input) + " failed with status " +
                                     std::to_string(status));
        }
        print("call echo: {\"input\":\"" + hex(input) + "\",\"output\":\"" + hex(output.text()) +
              "\"}\n");
        if (output.text() != input) {
            throw std::runtime_error("the echo of " + hex(input) + " is " + hex(output.text()));
        }
    }
    return 0;
}

int run(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: praenora_python_print <provider manifest> <python sources directory> "
                     "<package.module:Class> <method>\n"
                     "       praenora_python_print <provider manifest> --provider test.echo@0 "
                     "<text>\n";
        return 2;
    }
    const std::filesystem::path manifest_path = std::filesystem::u8path(argv[1]);
    if (std::string_view(argv[2]) == "--provider") {
        if (std::string_view(argv[3]) != PRAENORA_TEST_ECHO_SERVICE "@0") {
            std::cerr << "praenora_python_print: the provider mode knows the service "
                         "test.echo@0 only, not "
                      << argv[3] << "\n";
            return 2;
        }
        return run_echo(manifest_path, argv[4]);
    }
    const std::string sources = argv[2];
    const std::string name = argv[3];
    const std::string method = argv[4];

    const praenora::detail::ProviderManifest manifest =
        praenora::detail::load_provider_manifest(manifest_path);
    const praenora::detail::LoadedProvider& provider = praenora::detail::load_provider(
        manifest,
        {praenora::detail::ServiceVersion{PRAENORA_PYTHON_SERVICE, PRAENORA_PYTHON_SERVICE_MAJOR,
                                          PRAENORA_PYTHON_SERVICE_MINOR}},
        std::nullopt, nullptr);
    print("provider " + provider.describe().dump() + "\n");
    const auto* table = static_cast<const praenora_python_v0*>(
        provider.service(PRAENORA_PYTHON_SERVICE, PRAENORA_PYTHON_SERVICE_MAJOR));
    if (table == nullptr) {
        throw std::runtime_error("the provider offers no praenora.python@0 table");
    }

    TextSink described;
    praenora_status status = table->describe(table->context, described.get());
    if (status != PRAENORA_STATUS_OK) {
        throw std::runtime_error("describe failed with status " + std::to_string(status));
    }
    print("service " + described.text() + "\n");

    TextSink create_error;
    praenora_python_object* object = nullptr;
    status = table->create_object(table->context, nullptr, bytes(sources), bytes(""), bytes(name),
                                  bytes("null"), &object, create_error.get());
    if (status != PRAENORA_STATUS_OK) {
        throw std::runtime_error("create_object " + name + " failed with status " +
                                 std::to_string(status) + ": " + create_error.text());
    }
    print("create " + name + "\n");
    TextSink result, call_error;
    status = table->call(object, bytes(method), bytes("null"), result.get(), call_error.get());
    table->release_object(object);
    if (status != PRAENORA_STATUS_OK) {
        throw std::runtime_error("call " + method + " failed with status " +
                                 std::to_string(status) + ": " + call_error.text());
    }
    print("call " + method + ": " + result.text() + "\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const praenora::Error& error) {
        std::cerr << "praenora_python_print: " << error.what() << " " << error.details_json()
                  << "\n";
    } catch (const std::exception& error) {
        std::cerr << "praenora_python_print: " << error.what() << "\n";
    }
    return 1;
}
