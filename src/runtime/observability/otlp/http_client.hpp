// How the OTLP exporter sends a request: the interface OtlpTransport, which tests implement with
// fakes, and the HTTP client over cpp-httplib (http_client.cpp, the only source of the runtime
// that includes httplib.h). No TLS: endpoints are http only.
#ifndef PRAENORA_RUNTIME_OBSERVABILITY_OTLP_HTTP_CLIENT_HPP
#define PRAENORA_RUNTIME_OBSERVABILITY_OTLP_HTTP_CLIENT_HPP

#include "observability/otlp/otlp_settings.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace praenora::detail {

// What a POST got: the status of the answer, or 0 and why there is none (the connection failed or
// timed out).
struct HttpResponse {
    int status = 0;
    std::string error;                               // with status 0
    std::optional<std::int64_t> retry_after_seconds; // a Retry-After of delay-seconds, if any
};

class OtlpTransport {
public:
    virtual ~OtlpTransport() = default;

    // POSTs `body` with Content-Type application/json and `headers` to `endpoint`, giving up after
    // about `timeout` (connecting, writing and reading together). Never throws. Called from one
    // thread at a time.
    virtual HttpResponse post(const HttpEndpoint& endpoint, const HttpHeaders& headers,
                              const std::string& body, std::chrono::milliseconds timeout) = 0;
};

// The value of a Retry-After header in seconds: delay-seconds (digits only, surrounding spaces
// allowed); nullopt for an HTTP date or anything else.
std::optional<std::int64_t> parse_retry_after(std::string_view value);

// A transport that sends each request on a connection of its own, with cpp-httplib, resolving a
// host name with the system's resolver (getaddrinfo, which the timeout does not bound).
std::unique_ptr<OtlpTransport> make_http_transport();

// The transport that a Runtime's exporter uses: make_http_transport(), unless a test has set a
// factory with set_otlp_transport_factory_for_testing(). Thread-safe.
std::unique_ptr<OtlpTransport> make_otlp_transport();

// For white-box tests: makes make_otlp_transport() call `factory` (an empty function restores the
// HTTP transport). Thread-safe.
void set_otlp_transport_factory_for_testing(
    std::function<std::unique_ptr<OtlpTransport>()> factory);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_OBSERVABILITY_OTLP_HTTP_CLIENT_HPP
