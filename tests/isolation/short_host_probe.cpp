// The program of Isolation.AProviderHostTooShortForTheStoreOffersNone, a process of its own, so
// that the provider it starts is started there once and in no other process of the tests (a
// provider is started exactly once in a process). It opens the provider library its argument
// names (test-store), starts it with a provider host as short as a runtime's before the store's
// callbacks were appended (struct_size ends before load_compiled, and the fields beyond hold
// callbacks that must never be called), and writes to its descriptor 3 one line for each of
// test-store's answers to a probe, a save and a load, and then "store called" or "store not
// called". Exit code 0 when it got that far, 1 otherwise.
#include "provider/provider_abi.h"

#include "test_echo.h"

#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <string>

namespace {

std::atomic<bool> g_store_called{false};

std::int32_t PRAENORA_CALL short_host_load(void*, const char*, std::size_t, praenora_sink*) {
    g_store_called = true;
    return 0;
}

void PRAENORA_CALL short_host_save(void*, const char*, std::size_t, const char*, std::size_t) {
    g_store_called = true;
}

void PRAENORA_CALL short_host_log(void*, std::int32_t, const char*, std::size_t) {}

std::int32_t PRAENORA_CALL short_host_log_enabled(void*, std::int32_t) { return 0; }

bool write_line(const std::string& text) {
    const std::string line = text + "\n";
    return ::write(3, line.data(), line.size()) == static_cast<ssize_t>(line.size());
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fputs("short host probe: give the provider library\n", stderr);
        return 1;
    }
    void* library = ::dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        std::fprintf(stderr, "short host probe: %s\n", ::dlerror());
        return 1;
    }
    const auto get_api = reinterpret_cast<praenora_provider_get_api_fn>(
        ::dlsym(library, PRAENORA_PROVIDER_ENTRY_POINT));
    const praenora_provider_api_v0* api = get_api != nullptr ? get_api() : nullptr;
    if (api == nullptr) {
        std::fputs("short host probe: no provider entry point\n", stderr);
        return 1;
    }
    // Kept for good: the provider keeps its host for the rest of the process.
    static praenora_provider_host host{};
    host.struct_size = static_cast<std::uint32_t>(offsetof(praenora_provider_host, load_compiled));
    host.log = &short_host_log;
    host.log_enabled = &short_host_log_enabled;
    host.load_compiled = &short_host_load;
    host.save_compiled = &short_host_save;
    if (api->start(&host, nullptr) != PRAENORA_STATUS_OK) {
        std::fputs("short host probe: the provider did not start\n", stderr);
        return 1;
    }
    const auto* echo =
        static_cast<const praenora_test_echo_v0*>(api->get_service("test.echo", 9, 0));
    if (echo == nullptr) {
        std::fputs("short host probe: no test.echo\n", stderr);
        return 1;
    }
    for (const std::string& input : {std::string(R"({"probe": true})"),
                                     std::string(R"({"save": {"key": "short", "data": "x"}})"),
                                     std::string(R"({"load": "short"})")}) {
        std::string answer;
        praenora_sink sink{&answer, [](void* context, const char* data, std::size_t size) {
                               static_cast<std::string*>(context)->append(data, size);
                               return PRAENORA_STATUS_OK;
                           }};
        if (echo->echo(echo->context, nullptr, praenora_bytes{input.data(), input.size()}, &sink) !=
                PRAENORA_STATUS_OK ||
            !write_line(answer)) {
            return 1;
        }
    }
    return write_line(g_store_called.load() ? "store called" : "store not called") ? 0 : 1;
}
