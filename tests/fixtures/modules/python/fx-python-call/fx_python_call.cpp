// fx-python-call: a C++ module that uses the service praenora.python@0 directly. create() finds the
// directory of its own library (praenora::sdk::library_directory()) and creates an object of the
// class fx_python_call_helpers:Stats from the Python sources the build copies beside the library,
// into python/, checking them against their digest (FX_PYTHON_CALL_SOURCES_SHA256). Each invoke
// passes the list inputs.in.data.values ([] when absent) to the object's method summary, which sums
// the values with numpy, and writes what it returns to out.data.
#include <praenora/sdk/module.hpp>
#include <praenora/sdk/python.h>
#include <praenora/sdk/python_module.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace {

namespace sdk = praenora::sdk;
using nlohmann::json;

// An object of the library, whose address locates the library's file.
const char kMarker = 0;

praenora_bytes bytes(std::string_view text) { return praenora_bytes{text.data(), text.size()}; }

// A sink that keeps what is written to it.
struct Text {
    std::string text;
    praenora_sink sink{this, &Text::write};

    static praenora_status PRAENORA_CALL write(void* context, const char* data,
                                               std::size_t size) noexcept {
        try {
            static_cast<Text*>(context)->text.append(data == nullptr ? "" : data, size);
            return PRAENORA_STATUS_OK;
        } catch (...) {
            return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
        }
    }
};

class PythonCall final : public sdk::Module {
public:
    explicit PythonCall(const sdk::CreateContext& context) {
        python_ = static_cast<const praenora_python_v0*>(
            context.host.service(PRAENORA_PYTHON_SERVICE, PRAENORA_PYTHON_SERVICE_MAJOR));
        if (python_ == nullptr || python_->header.abi_major != PRAENORA_PYTHON_SERVICE_MAJOR ||
            python_->header.reserved != 0 ||
            python_->header.struct_size < PRAENORA_PYTHON_V0_BASE_SIZE) {
            throw sdk::Error(PRAENORA_STATUS_UNSUPPORTED,
                             "fx-python-call: the service praenora.python@0 is not available");
        }
        const std::string directory = sdk::library_directory(&kMarker);
        if (directory.empty()) {
            throw sdk::Error(PRAENORA_STATUS_INTERNAL, "fx-python-call: cannot find its library");
        }
        const std::string sources = directory + "/python";
        Text error;
        const praenora_status status = python_->create_object(
            python_->context, context.host.api(), bytes(sources),
            bytes(FX_PYTHON_CALL_SOURCES_SHA256), bytes("fx_python_call_helpers:Stats"),
            bytes("{}"), &object_, &error.sink);
        if (status != PRAENORA_STATUS_OK) {
            throw sdk::Error(status, "fx-python-call: create_object failed", error.text);
        }
    }

    ~PythonCall() override { python_->release_object(object_); }

    std::string invoke(std::string_view invocation_json) override {
        const json invocation =
            json::parse(invocation_json.begin(), invocation_json.end(), nullptr, false);
        if (invocation.is_discarded()) {
            throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT,
                             "fx-python-call: the invocation document is not JSON");
        }
        json values = json::array();
        const json* in = invocation.contains("inputs") && invocation["inputs"].contains("in")
                             ? &invocation["inputs"]["in"]
                             : nullptr;
        if (in != nullptr && in->contains("data") && (*in)["data"].contains("values")) {
            values = (*in)["data"]["values"];
        }
        const std::string args = json{{"values", values}}.dump();
        Text result;
        Text error;
        const praenora_status status =
            python_->call(object_, bytes("summary"), bytes(args), &result.sink, &error.sink);
        if (status != PRAENORA_STATUS_OK) {
            throw sdk::Error(status, "fx-python-call: summary failed", error.text);
        }
        json output;
        output["outputs"]["out"]["data"] = json::parse(result.text);
        return output.dump();
    }

private:
    const praenora_python_v0* python_ = nullptr;
    praenora_python_object* object_ = nullptr;
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(PythonCall)
