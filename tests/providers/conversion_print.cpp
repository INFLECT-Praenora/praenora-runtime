// praenora_conversion_print: loads a provider of praenora.model-conversion@0 through the runtime's
// provider loader, converts a source, converts the result back, and prints what describe and both
// conversions wrote, verbatim, so that two processes can be compared byte for byte.
//
//   praenora_conversion_print <provider manifest> <from> <source file> <to>
//
// The output: "describe <document>"; then, for the conversion from <from> to <to> (lines that
// start with "convert") and for the conversion of its text back from <to> to <from> (lines that
// start with "back"): "<which> <from> to <to>: text of <n> bytes", the text as the provider wrote
// it, a line feed, and "<which> <from> to <to>: report <report>". Exit code 0 when every call
// succeeded, 1 otherwise, with the problem on standard error.
#include "provider/provider_loader.hpp"
#include "provider/provider_manifest.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/sdk/model_conversion.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using nlohmann::json;

class TextSink {
public:
    praenora_sink* get() noexcept { return &sink_; }
    const std::string& text() const noexcept { return text_; }

private:
    static praenora_status PRAENORA_CALL write(void* context, const char* data,
                                               std::size_t size) noexcept {
        try {
            static_cast<TextSink*>(context)->text_.append(data == nullptr ? "" : data, size);
        } catch (...) {
            return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
        }
        return PRAENORA_STATUS_OK;
    }

    std::string text_;
    praenora_sink sink_{this, &TextSink::write};
};

praenora_bytes bytes(std::string_view text) { return praenora_bytes{text.data(), text.size()}; }

void print(std::string_view text) { std::fwrite(text.data(), 1, text.size(), stdout); }

// Converts `source` from `from` to `to`, prints the text and the report under `which`, and returns
// the text.
std::string convert(const praenora_model_conversion_v0* table, const std::string& which,
                    const std::string& from, const std::string& to, const std::string& source) {
    TextSink text, report, error;
    const std::string options = json{{"from", from}, {"to", to}}.dump();
    const praenora_status status =
        table->convert(table->context, nullptr, bytes(options), bytes(source), text.get(),
                       report.get(), error.get());
    if (status != PRAENORA_STATUS_OK) {
        throw std::runtime_error(which + " " + from + " to " + to + " failed with status " +
                                 std::to_string(status) + ": " + error.text());
    }
    const std::string head = which + " " + from + " to " + to + ": ";
    print(head + "text of " + std::to_string(text.text().size()) + " bytes\n");
    print(text.text());
    print("\n" + head + "report " + report.text() + "\n");
    return text.text();
}

int run(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: praenora_conversion_print <provider manifest> <from> <source file> "
                     "<to>\n";
        return 2;
    }
    const std::filesystem::path manifest_path = std::filesystem::u8path(argv[1]);
    const std::string from = argv[2];
    std::ifstream source_file(std::filesystem::u8path(argv[3]), std::ios::binary);
    if (!source_file) {
        throw std::runtime_error(std::string("cannot read the source file ") + argv[3]);
    }
    const std::string source((std::istreambuf_iterator<char>(source_file)),
                             std::istreambuf_iterator<char>());
    const std::string to = argv[4];

    const praenora::detail::ProviderManifest manifest =
        praenora::detail::load_provider_manifest(manifest_path);
    const praenora::detail::LoadedProvider& provider = praenora::detail::load_provider(
        manifest,
        {praenora::detail::ServiceVersion{PRAENORA_MODEL_CONVERSION_SERVICE,
                                          PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR,
                                          PRAENORA_MODEL_CONVERSION_SERVICE_MINOR}},
        std::nullopt, nullptr);
    const auto* table = static_cast<const praenora_model_conversion_v0*>(provider.service(
        PRAENORA_MODEL_CONVERSION_SERVICE, PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR));
    if (table == nullptr) {
        throw std::runtime_error("the provider offers no praenora.model-conversion@0 table");
    }

    TextSink described;
    const praenora_status status = table->describe(table->context, described.get());
    if (status != PRAENORA_STATUS_OK) {
        throw std::runtime_error("describe failed with status " + std::to_string(status));
    }
    print("describe " + described.text() + "\n");
    const std::string there = convert(table, "convert", from, to, source);
    convert(table, "back", to, from, there);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const praenora::Error& error) {
        std::cerr << "praenora_conversion_print: " << error.what() << " " << error.details_json()
                  << "\n";
    } catch (const std::exception& error) {
        std::cerr << "praenora_conversion_print: " << error.what() << "\n";
    }
    return 1;
}
