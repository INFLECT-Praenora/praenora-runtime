// praenora_simulation_print: loads a provider of praenora.simulation@0 through the runtime's
// provider loader, opens a model and prints what a fixed sequence of calls computes, every double
// exactly (as a C99 hexadecimal floating-point number), so that two processes can be compared
// byte for byte (tests/providers/run_twice.cmake).
//
//   praenora_simulation_print <provider manifest> <model format> <model file> <end time>
//
// The calls: describe_model; simulate from 0 to the end time on a grid of 16 intervals with every
// state and quantity; a kernel's initial state and its right-hand side there; a plan whose inputs
// are the states, run as a batch of three sets (the initial state times 0.5, 1 and 2). The model
// must have no events (it needs a kernel). Exit code 0 when every call succeeded, 1 otherwise,
// with the problem on standard error.
#include "provider/provider_loader.hpp"
#include "provider/provider_manifest.hpp"

#include <praenora/runtime/error.hpp>
#include <praenora/sdk/simulation.h>

#include <nlohmann/json.hpp>

#include <cstdint>
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
#include <vector>

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

void check(praenora_status status, const TextSink& error, const char* what) {
    if (status != PRAENORA_STATUS_OK) {
        throw std::runtime_error(std::string(what) + " failed with status " +
                                 std::to_string(status) + ": " + error.text());
    }
}

void print_values(const char* what, const std::vector<double>& values) {
    std::printf("%s", what);
    for (const double value : values) {
        std::printf(" %a", value);
    }
    std::printf("\n");
}

int run(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: praenora_simulation_print <provider manifest> <model format> "
                     "<model file> <end time>\n";
        return 2;
    }
    const std::filesystem::path manifest_path = std::filesystem::u8path(argv[1]);
    const std::string format = argv[2];
    std::ifstream model_file(std::filesystem::u8path(argv[3]), std::ios::binary);
    const std::string model((std::istreambuf_iterator<char>(model_file)),
                            std::istreambuf_iterator<char>());
    const double end = std::stod(argv[4]);

    const praenora::detail::ProviderManifest manifest =
        praenora::detail::load_provider_manifest(manifest_path);
    const praenora::detail::LoadedProvider& provider = praenora::detail::load_provider(
        manifest,
        {praenora::detail::ServiceVersion{PRAENORA_SIMULATION_SERVICE,
                                          PRAENORA_SIMULATION_SERVICE_MAJOR,
                                          PRAENORA_SIMULATION_SERVICE_MINOR}},
        std::nullopt, nullptr);
    const auto* table = static_cast<const praenora_simulation_v0*>(
        provider.service(PRAENORA_SIMULATION_SERVICE, PRAENORA_SIMULATION_SERVICE_MAJOR));
    if (table == nullptr) {
        throw std::runtime_error("the provider offers no praenora.simulation@0 table");
    }

    praenora_simulation_session* session = nullptr;
    {
        TextSink error;
        const std::string options = json{{"format", format}}.dump();
        check(table->open_session(table->context, nullptr, bytes(model), bytes(options), &session,
                                  error.get()),
              error, "open_session");
    }
    TextSink described, error;
    check(table->describe_model(session, nullptr, described.get(), error.get()), error,
          "describe_model");
    std::printf("describe_model %s\n", described.text().c_str());
    const json ids = json::parse(described.text());
    const std::size_t states = ids.at("states").size();

    const json request = {
        {"time_domain", {{"start", 0.0}, {"end", end}}},
        {"output",
         {{"save_at", "regular_grid"}, {"step", end / 16}, {"quantities", ids.at("quantities")}}}};
    TextSink result;
    check(table->simulate(session, nullptr, bytes(request.dump()), result.get(), error.get()),
          error, "simulate");
    const json simulated = json::parse(result.text());
    std::printf("simulate %s\n", result.text().c_str());
    if (!simulated.value("ok", false)) {
        throw std::runtime_error("the simulation failed: " + result.text());
    }
    std::vector<double> rows;
    for (const json& row : simulated.at("rows")) {
        for (const json& value : row) {
            rows.push_back(value.get<double>());
        }
    }
    print_values("simulate rows", rows);

    praenora_simulation_kernel kernel{};
    kernel.struct_size = static_cast<std::uint32_t>(sizeof(kernel));
    check(table->open_kernel(session, nullptr, bytes("{}"), &kernel, error.get()), error,
          "open_kernel");
    std::vector<double> y(states), dydt(states);
    if (kernel.initial_state(kernel.model, y.data()) != PRAENORA_STATUS_OK ||
        kernel.rhs(kernel.model, end / 2, y.data(), dydt.data()) != PRAENORA_STATUS_OK) {
        throw std::runtime_error("a kernel function failed");
    }
    print_values("kernel initial_state", y);
    print_values("kernel rhs", dydt);
    table->close_kernel(&kernel);

    praenora_simulation_plan* plan = nullptr;
    json plan_document = request;
    plan_document["initial_conditions"] = ids.at("states");
    TextSink info;
    check(table->prepare(session, nullptr, bytes(plan_document.dump()), &plan, info.get(),
                         error.get()),
          error, "prepare");
    table->close_session(session);
    const json shape = json::parse(info.text());
    const std::size_t size =
        shape.at("rows").get<std::size_t>() * shape.at("columns").size();
    std::vector<double> values;
    for (const double factor : {0.5, 1.0, 2.0}) {
        for (const double value : y) {
            values.push_back(value * factor);
        }
    }
    std::vector<double> out(3 * size);
    std::vector<std::int32_t> outcomes(3, -1);
    check(table->run_batch(plan, nullptr, values.data(), values.size(), 3, out.data(), out.size(),
                           outcomes.data(), error.get()),
          error, "run_batch");
    table->close_plan(plan);
    std::printf("run_batch outcomes %d %d %d\n", outcomes[0], outcomes[1], outcomes[2]);
    print_values("run_batch out", out);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const praenora::Error& error) {
        std::cerr << "praenora_simulation_print: " << error.what() << " " << error.details_json()
                  << "\n";
    } catch (const std::exception& error) {
        std::cerr << "praenora_simulation_print: " << error.what() << "\n";
    }
    return 1;
}
