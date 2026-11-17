// fx-service: a module that uses runtime services. In each invoke it looks up test.echo@0 (the
// test service of the fixture provider test-echo, tests/support/test_echo.h) and every service
// its parameter `lookups` names (["<name>@<major>", ...]) through the host API, and echoes the
// data of input `in` ({} when absent) through test.echo when the table it got has an echo
// function. Output out.data is
//   {"echo": <the echoed data, or null without a usable test.echo@0>,
//    "found": {"<name>@<major>": <whether get_service() returned a table>, ...}}
// for test.echo@0 and each lookup. With the parameter require_echo true (default false), an
// invoke without a usable test.echo@0 fails with NOT_FOUND. create() logs "fx-service:create" at
// info level. One source, built as several fixture modules that declare different services
// (tests/fixtures/modules/CMakeLists.txt).
#include "test_echo.h"

#include <praenora/sdk/module.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace sdk = praenora::sdk;

using nlohmann::json;

// A sink that appends what it receives to a string.
struct Collected {
    std::string text;
};

praenora_status PRAENORA_CALL collect(void* context, const char* data, std::size_t size) {
    if (context == nullptr || (data == nullptr && size != 0)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    try {
        static_cast<Collected*>(context)->text.append(data == nullptr ? "" : data, size);
        return PRAENORA_STATUS_OK;
    } catch (...) {
        return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
    }
}

json parse(std::string_view text, const char* what) {
    try {
        return json::parse(text.begin(), text.end());
    } catch (const json::parse_error& e) {
        throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT,
                         std::string("fx-service: ") + what + " is not JSON: " + e.what());
    }
}

// A service named "<name>@<major>".
struct Lookup {
    std::string key, name;
    std::uint32_t major = 0;
};

Lookup lookup_of(const std::string& key) {
    const std::size_t at = key.rfind('@');
    if (at == std::string::npos || at == 0 || at + 1 == key.size()) {
        throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT,
                         "fx-service: lookup '" + key + "' is not <name>@<major>");
    }
    try {
        return Lookup{key, key.substr(0, at),
                      static_cast<std::uint32_t>(std::stoul(key.substr(at + 1)))};
    } catch (const std::exception&) {
        throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT,
                         "fx-service: lookup '" + key + "' has no numeric major");
    }
}

class ServiceModule final : public sdk::Module {
public:
    explicit ServiceModule(const sdk::CreateContext& context) : host_(context.host) {
        host_.log(sdk::LogLevel::info, "fx-service:create");
        const json create = parse(context.create_json, "the create document");
        const json parameters = create.value("parameters", json::object());
        for (const auto& key : parameters.value("lookups", json::array())) {
            if (!key.is_string()) {
                throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT,
                                 "fx-service: every lookup is a string \"<name>@<major>\"");
            }
            lookups_.push_back(lookup_of(key.get<std::string>()));
        }
        require_echo_ = parameters.value("require_echo", false);
    }

    std::string invoke(std::string_view invocation_json) override {
        const json invocation = parse(invocation_json, "the invocation document");
        json data = json::object();
        const auto inputs = invocation.find("inputs");
        if (inputs != invocation.end() && inputs->is_object()) {
            const auto in = inputs->find("in");
            if (in != inputs->end() && in->is_object() && in->contains("data")) {
                data = in->at("data");
            }
        }

        json found = json::object();
        const auto* echo = static_cast<const praenora_test_echo_v0*>(
            host_.service(PRAENORA_TEST_ECHO_SERVICE, 0));
        found[std::string(PRAENORA_TEST_ECHO_SERVICE) + "@0"] = echo != nullptr;
        for (const Lookup& lookup : lookups_) {
            found[lookup.key] = host_.service(lookup.name, lookup.major) != nullptr;
        }

        json echoed = nullptr;
        constexpr std::size_t kEchoEnd =
            offsetof(praenora_test_echo_v0, echo) + sizeof(praenora_test_echo_v0::echo);
        if (echo != nullptr && echo->header.struct_size >= kEchoEnd && echo->echo != nullptr) {
            const std::string text = data.dump();
            Collected collected;
            praenora_sink sink{&collected, &collect};
            const praenora_status status = echo->echo(
                echo->context, nullptr, praenora_bytes{text.data(), text.size()}, &sink);
            if (status != PRAENORA_STATUS_OK) {
                throw sdk::Error(status, "fx-service: test.echo failed");
            }
            echoed = parse(collected.text, "what test.echo wrote");
        } else if (require_echo_) {
            throw sdk::Error(PRAENORA_STATUS_NOT_FOUND,
                             "fx-service: the runtime serves no usable test.echo@0");
        }

        json result;
        result["outputs"]["out"]["data"] = {{"echo", std::move(echoed)}, {"found", found}};
        return result.dump();
    }

private:
    const sdk::Host& host_;
    std::vector<Lookup> lookups_;
    bool require_echo_ = false;
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(ServiceModule)
