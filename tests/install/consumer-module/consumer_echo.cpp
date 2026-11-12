// consumer-echo: a module built outside praenora-runtime against the installed module SDK only
// (the C++ adapter; praenora_add_module() defines its identity). It implements the test
// capability test.echo@0.1.0 without a JSON library, so it does not read its input: every
// invocation writes an empty object to output out. The install test loads it and reads its
// describe document.
#include <praenora/sdk/module.hpp>

#include <string>
#include <string_view>

namespace {

class ConsumerEcho final : public praenora::sdk::Module {
public:
    explicit ConsumerEcho(const praenora::sdk::CreateContext&) {}

    std::string invoke(std::string_view) override { return R"({"outputs":{"out":{"data":{}}}})"; }
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(ConsumerEcho)
