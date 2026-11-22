// test-store: a fixture provider of the test service test.echo@0.0 (tests/support/test_echo.h)
// that drives the shared compiled models of its provider host (load_compiled and save_compiled,
// src/runtime/provider/provider_abi.h) as its input says, for the isolation tests. Its echo()
// takes a JSON object (what fx-service echoes) and writes a JSON object:
//   {"probe": true}                     -> {"load_compiled": <offered>, "save_compiled": <offered>}
//   {"save": {"key": K, "data": D}}     -> {"saved": K}, after save_compiled(K, D)
//   {"save": {"key": K, "size": N}}     -> the same with N bytes 'x' as the entry
//   {"load": K}                         -> {"found": <load_compiled's answer>, "data": D or null}
//   {"later": {"key": K, "data": D}}    -> {"later": true}; a thread of its own then waits until
//                                          the file that PRAENORA_TEST_STORE_LATER names exists
//                                          (for at most 60 s), saves D under K, loads K and logs
//                                          "test-store: between calls: found D" (or "missing")
//                                          at info through the provider host
// With a store not offered, save, load and later write {"error": "no store"}. When the environment
// variable PRAENORA_TEST_STORE_AT_START names a key, start() saves "at start" under it, loads it
// and logs "test-store: at start: found at start" (or "missing", or "no store") at info. Keys and
// entries are JSON strings (UTF-8). C++17 with nlohmann/json; no exception leaves the provider.
#include "provider_fixture.h"
#include "test_echo.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

namespace {

using nlohmann::json;

const praenora_provider_host* g_host = nullptr; // set by start(), before any service call

bool offers(const praenora_provider_host* host, std::size_t end) noexcept {
    return host != nullptr && host->struct_size >= end;
}

bool offers_load(const praenora_provider_host* host) noexcept {
    return offers(host, offsetof(praenora_provider_host, load_compiled) +
                            sizeof(praenora_provider_host::load_compiled)) &&
           host->load_compiled != nullptr;
}

bool offers_save(const praenora_provider_host* host) noexcept {
    return offers(host, offsetof(praenora_provider_host, save_compiled) +
                            sizeof(praenora_provider_host::save_compiled)) &&
           host->save_compiled != nullptr;
}

praenora_status PRAENORA_CALL append(void* context, const char* data, std::size_t size) noexcept {
    try {
        static_cast<std::string*>(context)->append(data == nullptr ? "" : data, size);
        return PRAENORA_STATUS_OK;
    } catch (...) {
        return PRAENORA_STATUS_RESOURCE_EXHAUSTED;
    }
}

void save(const std::string& key, const std::string& data) {
    g_host->save_compiled(g_host->context, key.data(), key.size(), data.data(), data.size());
}

// The entry under `key`, or null.
json load(const std::string& key) {
    std::string entry;
    praenora_sink sink{&entry, &append};
    if (g_host->load_compiled(g_host->context, key.data(), key.size(), &sink) == 0) {
        return nullptr;
    }
    return entry;
}

void log(const std::string& message) {
    provider_fixture_log(g_host, PRAENORA_LOG_INFO, message.c_str());
}

// What a save, then a load of `key`, found: "found <entry>" or "missing".
std::string round_trip(const std::string& key, const std::string& data) {
    save(key, data);
    const json found = load(key);
    return found.is_string() ? "found " + found.get<std::string>() : std::string("missing");
}

void later(std::string key, std::string data) noexcept {
    try {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv is fine here: nothing changes the environment
#endif
        const char* trigger = std::getenv("PRAENORA_TEST_STORE_LATER");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        for (int polls = 0; trigger != nullptr && polls < 6000; ++polls) {
            if (std::ifstream(trigger).good()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        log("test-store: between calls: " + round_trip(key, data));
    } catch (...) {
        // Nothing to report it to.
    }
}

json answer(const json& input) {
    if (input.contains("probe")) {
        return {{"load_compiled", offers_load(g_host)}, {"save_compiled", offers_save(g_host)}};
    }
    if (!offers_load(g_host) || !offers_save(g_host)) {
        return {{"error", "no store"}};
    }
    if (input.contains("save")) {
        const json& what = input.at("save");
        const std::string key = what.at("key").get<std::string>();
        save(key, what.contains("size") ? std::string(what.at("size").get<std::size_t>(), 'x')
                                        : what.at("data").get<std::string>());
        return {{"saved", key}};
    }
    if (input.contains("load")) {
        const json found = load(input.at("load").get<std::string>());
        return {{"found", found.is_string()}, {"data", found}};
    }
    if (input.contains("later")) {
        const json& what = input.at("later");
        std::thread(later, what.at("key").get<std::string>(), what.at("data").get<std::string>())
            .detach();
        return {{"later", true}};
    }
    return {{"error", "unknown command"}};
}

praenora_status PRAENORA_CALL echo(void*, const praenora_host_api*, praenora_bytes input,
                                   praenora_sink* output) {
    if (output == nullptr || output->write == nullptr || (input.data == nullptr && input.size)) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
    try {
        const json parsed = json::parse(input.data, input.data + input.size);
        const std::string text = answer(parsed).dump();
        return output->write(output->context, text.data(), text.size());
    } catch (...) {
        return PRAENORA_STATUS_INVALID_ARGUMENT;
    }
}

const praenora_test_echo_v0 echo_table = {
    {0u, 0u, static_cast<std::uint32_t>(sizeof(praenora_test_echo_v0)), 0u}, nullptr, &echo};

praenora_status PRAENORA_CALL start(const praenora_provider_host* host, praenora_sink*) {
    g_host = host;
    try {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv is fine here: nothing changes the environment
#endif
        const char* key = std::getenv("PRAENORA_TEST_STORE_AT_START");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        if (key != nullptr) {
            log(std::string("test-store: at start: ") + (offers_load(host) && offers_save(host)
                                                             ? round_trip(key, "at start")
                                                             : std::string("no store")));
        }
    } catch (...) {
        return PRAENORA_STATUS_INTERNAL;
    }
    provider_fixture_log(host, PRAENORA_LOG_INFO, "test-store started");
    return PRAENORA_STATUS_OK;
}

const void* PRAENORA_CALL get_service(const char* name, std::size_t name_size,
                                      std::uint32_t major) {
    if (major == 0 && provider_fixture_name_is(name, name_size, PRAENORA_TEST_ECHO_SERVICE)) {
        return &echo_table;
    }
    return nullptr;
}

const praenora_provider_api_v0 provider_table = {
    {PRAENORA_PROVIDER_ABI_MAJOR, PRAENORA_PROVIDER_ABI_MINOR,
     static_cast<std::uint32_t>(sizeof(praenora_provider_api_v0)), 0u},
    &provider_fixture_describe,
    &start,
    &get_service,
};

} // namespace

const praenora_provider_api_v0* PRAENORA_CALL praenora_provider_get_api(void) {
    return &provider_table;
}
