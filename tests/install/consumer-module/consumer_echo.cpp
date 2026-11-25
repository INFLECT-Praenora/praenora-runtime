// consumer-echo: a module built outside praenora-runtime against the installed module SDK only
// (the C++ adapter; praenora_add_module() defines its identity). It implements the test
// capability test.echo@0.1.0 without a JSON library, so it does not read its input: every
// invocation writes an empty object to output out. The install test loads it and reads its
// describe document. It also includes the headers of the simulation, model conversion and Python
// services, which an installed SDK holds, though it uses none of them.
#include <praenora/sdk/model_conversion.h>
#include <praenora/sdk/module.hpp>
#include <praenora/sdk/python.h>
#include <praenora/sdk/simulation.h>

#include <string>
#include <string_view>

static_assert(PRAENORA_SIMULATION_SERVICE_MAJOR == 0u,
              "the installed praenora/sdk/simulation.h declares praenora.simulation@0");
static_assert(PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR == 0u,
              "the installed praenora/sdk/model_conversion.h declares "
              "praenora.model-conversion@0");
static_assert(PRAENORA_PYTHON_SERVICE_MAJOR == 0u,
              "the installed praenora/sdk/python.h declares praenora.python@0");

namespace {

class ConsumerEcho final : public praenora::sdk::Module {
public:
    explicit ConsumerEcho(const praenora::sdk::CreateContext&) {}

    std::string invoke(std::string_view) override { return R"({"outputs":{"out":{"data":{}}}})"; }
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(ConsumerEcho)
