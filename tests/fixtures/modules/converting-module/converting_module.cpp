// converting-module: a module that uses the model conversion service praenora.model-conversion@0
// (include/praenora/sdk/model_conversion.h), which its manifest requires, and the simulation
// service praenora.simulation@0 (include/praenora/sdk/simulation.h), which it can use, end to end
// through the runtime and service providers (the fixture providers test-conversion and
// test-simulation in the tests).
//
// create() gets the conversion service's table from the host API, and the simulation service's
// only when parameter `simulate` is given: without it, the simulation service is never asked for.
// initialize() converts the source of parameter `model` from format `from` to format `to` (and
// the module of parameter `module`, when given); with `simulate`, it then opens a session for the
// converted text with {"format": <to>}, runs `simulate` (a request of the simulation service) and
// closes the session. Each invoke() writes to output out
//   {"sha256": <the converted text's SHA-256>, "bytes": <its size>, "report": <the conversion's
//    report>, "result": <the simulation's result, with simulate only>}.
// A call of a service that fails fails the module call with the service's status, a message that
// quotes the service's, and its error document as details.cause.
#include <praenora/sdk/model_conversion.h>
#include <praenora/sdk/module.hpp>
#include <praenora/sdk/simulation.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace {

namespace sdk = praenora::sdk;

using nlohmann::json;

// A sink that appends what it receives to a string.
struct Collected {
    std::string text;
    praenora_sink sink{this, &Collected::write};

    Collected() = default;
    Collected(const Collected&) = delete;
    Collected& operator=(const Collected&) = delete;

    static praenora_status PRAENORA_CALL write(void* context, const char* data,
                                               std::size_t size) noexcept {
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
};

praenora_bytes bytes(const std::string& text) { return praenora_bytes{text.data(), text.size()}; }

json parse(std::string_view text, const char* what) {
    try {
        return json::parse(text.begin(), text.end());
    } catch (const json::exception& e) {
        throw sdk::Error(PRAENORA_STATUS_INVALID_ARGUMENT,
                         std::string("converting-module: ") + what + " is not JSON: " + e.what());
    }
}

// Fails the module call with the status of a failed service call, its message and its error
// document.
[[noreturn]] void service_failed(const char* call, praenora_status status, const Collected& error) {
    const json cause = json::parse(error.text, nullptr, false);
    std::string message = std::string("converting-module: ") + call + " failed";
    if (cause.is_object() && cause.contains("message") && cause.at("message").is_string()) {
        message += ": " + cause.at("message").get<std::string>();
    }
    throw sdk::Error(status, message,
                     json{{"cause", cause.is_discarded() ? json(error.text) : cause}}.dump());
}

// SHA-256 (FIPS 180-4) of `data`, as 64 lowercase hexadecimal digits.
std::string sha256_hex(std::string_view data) {
    static constexpr std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    // The message, padded: a 1 bit, zeros, and its length in bits as a big-endian uint64.
    std::string message(data);
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8u;
    message.push_back(static_cast<char>(0x80));
    while (message.size() % 64 != 56) {
        message.push_back('\0');
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(static_cast<char>((bits >> shift) & 0xffu));
    }
    const auto rotr = [](std::uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); };
    const auto byte = [&message](std::size_t i) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(message[i]));
    };
    for (std::size_t block = 0; block < message.size(); block += 64) {
        std::uint32_t w[64];
        for (std::size_t t = 0; t < 16; ++t) {
            const std::size_t i = block + 4 * t;
            w[t] = (byte(i) << 24) | (byte(i + 1) << 16) | (byte(i + 2) << 8) | byte(i + 3);
        }
        for (std::size_t t = 16; t < 64; ++t) {
            const std::uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
            const std::uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6],
                      hh = h[7];
        for (std::size_t t = 0; t < 64; ++t) {
            const std::uint32_t t1 =
                hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[t] + w[t];
            const std::uint32_t t2 =
                (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (const std::uint32_t word : h) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            hex.push_back(digits[(word >> shift) & 0xfu]);
        }
    }
    return hex;
}

class ConvertingModule final : public sdk::Module {
public:
    explicit ConvertingModule(const sdk::CreateContext& context) : host_(context.host) {
        const json parameters =
            parse(context.create_json, "the create document").value("parameters", json::object());
        model_ = parameters.value("model", std::string());
        from_ = parameters.value("from", std::string("test-decay-text"));
        to_ = parameters.value("to", std::string("test-decay"));
        if (parameters.contains("module")) {
            module_ = parameters.at("module").get<std::string>();
        }
        if (parameters.contains("simulate")) {
            simulate_ = parameters.at("simulate").dump();
        }

        // The tables, once; tables live until the process ends.
        conversion_ = static_cast<const praenora_model_conversion_v0*>(host_.service(
            PRAENORA_MODEL_CONVERSION_SERVICE, PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR));
        if (conversion_ == nullptr ||
            conversion_->header.abi_major != PRAENORA_MODEL_CONVERSION_SERVICE_MAJOR ||
            conversion_->header.reserved != 0u ||
            conversion_->header.struct_size < PRAENORA_MODEL_CONVERSION_V0_BASE_SIZE) {
            throw sdk::Error(PRAENORA_STATUS_UNSUPPORTED,
                             "converting-module: the runtime serves no usable "
                             "praenora.model-conversion@0");
        }
        if (!simulate_.empty()) {
            simulation_ = static_cast<const praenora_simulation_v0*>(
                host_.service(PRAENORA_SIMULATION_SERVICE, PRAENORA_SIMULATION_SERVICE_MAJOR));
            if (simulation_ == nullptr ||
                simulation_->header.abi_major != PRAENORA_SIMULATION_SERVICE_MAJOR ||
                simulation_->header.reserved != 0u ||
                simulation_->header.struct_size < PRAENORA_SIMULATION_V0_BASE_SIZE) {
                throw sdk::Error(PRAENORA_STATUS_UNSUPPORTED,
                                 "converting-module: parameter simulate needs "
                                 "praenora.simulation@0, which the runtime does not serve");
            }
        }
    }

    void initialize(const sdk::InitializeContext&) override {
        const praenora_host_api* host = host_.api();
        json options = {{"from", from_}, {"to", to_}};
        if (module_.has_value()) {
            options["module"] = *module_;
        }
        const std::string options_json = options.dump();
        Collected text, report, error;
        praenora_status status =
            conversion_->convert(conversion_->context, host, bytes(options_json), bytes(model_),
                                 &text.sink, &report.sink, &error.sink);
        if (status != PRAENORA_STATUS_OK) {
            service_failed("convert", status, error);
        }
        converted_ = text.text;
        report_ = parse(report.text, "the conversion's report");
        if (simulation_ == nullptr) {
            return;
        }

        const std::string session_options = json{{"format", to_}}.dump();
        praenora_simulation_session* session = nullptr;
        status = simulation_->open_session(simulation_->context, host, bytes(converted_),
                                           bytes(session_options), &session, &error.sink);
        if (status != PRAENORA_STATUS_OK) {
            service_failed("open_session", status, error);
        }
        // Closed when this function ends, whatever happens in it.
        struct Closer {
            const praenora_simulation_v0* table;
            praenora_simulation_session* session;
            ~Closer() { table->close_session(session); }
        } closer{simulation_, session};
        Collected result;
        status = simulation_->simulate(session, host, bytes(simulate_), &result.sink, &error.sink);
        if (status != PRAENORA_STATUS_OK) {
            service_failed("simulate", status, error);
        }
        result_ = parse(result.text, "the simulation's result");
    }

    std::string invoke(std::string_view invocation_json) override {
        // The invocation names no input this module reads, but it must be a document.
        static_cast<void>(parse(invocation_json, "the invocation document"));
        json data = {
            {"sha256", sha256_hex(converted_)}, {"bytes", converted_.size()}, {"report", report_}};
        if (simulation_ != nullptr) {
            data["result"] = result_;
        }
        json document;
        document["outputs"]["out"]["data"] = std::move(data);
        return document.dump();
    }

private:
    const sdk::Host& host_;
    const praenora_model_conversion_v0* conversion_ = nullptr;
    const praenora_simulation_v0* simulation_ = nullptr; // with parameter simulate only
    std::string model_, from_, to_, simulate_;
    std::optional<std::string> module_;
    std::string converted_;
    json report_, result_;
};

} // namespace

PRAENORA_SDK_DEFINE_MODULE(ConvertingModule)
