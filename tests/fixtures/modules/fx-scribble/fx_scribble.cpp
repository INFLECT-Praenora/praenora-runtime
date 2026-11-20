// fx-scribble: in invoke, writes to every descriptor from 3 to 63 what its parameter `write`
// names, then returns {"outputs":{"out":{"data":{}}}} as if nothing happened: "garbage" (bytes
// that are no frame of a module host's channel) or "huge-frame" (a well-formed frame header,
// magic "PRMH" and kind sink, that announces a payload of 4 GiB - 1 bytes and sends none);
// "nothing" (the default) writes nothing. Only for a module host, whose descriptor 3 is its
// channel to the runtime: in another process it would write into whatever those descriptors are.
#include <praenora/sdk/module.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

namespace sdk = praenora::sdk;

// A frame header of the channel: magic, kind 19 (sink), id 0, length 0xFFFFFFFF; little-endian.
constexpr unsigned char kHugeFrame[20] = {0x50, 0x52, 0x4D, 0x48, 0x13, 0x00, 0x00,
                                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};

class ScribbleModule final : public sdk::Module {
public:
    explicit ScribbleModule(const sdk::CreateContext& context) {
        const std::string_view create_json = context.create_json;
        try {
            const auto document = nlohmann::json::parse(create_json.begin(), create_json.end());
            const auto parameters = document.find("parameters");
            if (parameters != document.end() && parameters->is_object()) {
                write_ = parameters->value("write", write_);
            }
        } catch (const nlohmann::json::exception& e) {
            throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT,
                             std::string("fx-scribble: the create document is not valid JSON: ") +
                                 e.what());
        }
    }

    std::string invoke(std::string_view) override {
        if (write_ == "garbage") {
            const std::string garbage(4096, '\x5A');
            scribble(garbage.data(), garbage.size());
        } else if (write_ == "huge-frame") {
            scribble(reinterpret_cast<const char*>(kHugeFrame), sizeof kHugeFrame);
        }
        return R"({"outputs":{"out":{"data":{}}}})";
    }

private:
    static void scribble(const char* data, std::size_t size) {
#ifndef _WIN32
        for (int descriptor = 3; descriptor <= 63; ++descriptor) {
            static_cast<void>(::write(descriptor, data, size));
        }
#else
        static_cast<void>(data);
        static_cast<void>(size);
#endif
    }

    std::string write_ = "nothing";
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(ScribbleModule)
