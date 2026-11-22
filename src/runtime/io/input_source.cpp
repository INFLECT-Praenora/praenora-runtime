#include "io/input_source.hpp"

#include <praenora/runtime/error.hpp>

namespace praenora::detail {

InputSource::~InputSource() = default;

std::unique_ptr<InputSource> make_input_source(const InputBinding& binding,
                                               const MessageTypeDef& port_type,
                                               const TimeBase& time) {
    if (binding.source == "observation-csv") {
        return make_observation_csv_source(binding, port_type, time);
    }
    if (binding.source == "json-document") {
        return make_json_document_source(binding, port_type);
    }
    throw Error(ErrorCode::INPUT_BINDING_INVALID,
                "input '" + binding.input + "': unknown source '" + binding.source +
                    "'; the sources are observation-csv and json-document");
}

} // namespace praenora::detail
