// A fake of the Python core with the core's interface (src/providers/python/core/
// python_core.h) and another build identity: the test of a changed core puts it in a copy of the
// provider python's directory under the core's file name, and the provider's start must refuse it
// before it calls any other function of it. Links no Python. Every function but build_id fails.
#define PRAENORA_PYTHON_CORE_IMPLEMENTATION
#include "python_core.h"

#include <cstdint>

namespace {

// Not the build identity of any core: the test checks that the refusal names it.
constexpr char kBuildId[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

praenora_status PRAENORA_CALL start(const praenora_provider_host*, praenora_bytes, praenora_bytes,
                                    praenora_sink*) {
    return PRAENORA_STATUS_INTERNAL;
}

praenora_status PRAENORA_CALL describe(praenora_sink*) { return PRAENORA_STATUS_INTERNAL; }

praenora_status PRAENORA_CALL create_object(const praenora_provider_host*, const praenora_host_api*,
                                            praenora_bytes, praenora_bytes, praenora_bytes,
                                            praenora_bytes, praenora_python_object** out_object,
                                            praenora_sink*) {
    if (out_object != nullptr) {
        *out_object = nullptr;
    }
    return PRAENORA_STATUS_INTERNAL;
}

praenora_status PRAENORA_CALL call(praenora_python_object*, const praenora_host_api*,
                                   praenora_bytes, praenora_bytes, praenora_sink*, praenora_sink*) {
    return PRAENORA_STATUS_INTERNAL;
}

void PRAENORA_CALL release_object(praenora_python_object*) {}

praenora_status PRAENORA_CALL build_id(praenora_sink* out_text) {
    if (out_text == nullptr || out_text->write == nullptr) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    return out_text->write(out_text->context, kBuildId, sizeof kBuildId - 1);
}

const praenora_python_core_v0 kTable = {
    {PRAENORA_PYTHON_CORE_MAJOR, PRAENORA_PYTHON_CORE_MINOR,
     static_cast<std::uint32_t>(sizeof(praenora_python_core_v0)), 0u},
    start,
    describe,
    create_object,
    call,
    release_object,
    build_id,
};

} // namespace

const praenora_python_core_v0* PRAENORA_CALL praenora_python_core_get_api(void) { return &kTable; }
