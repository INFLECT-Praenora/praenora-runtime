// The json-document source: one static payload, delivered every tick.
#include "io/input_source.hpp"

#include "core/paths.hpp"
#include "core/sha256.hpp"
#include "document/yaml_loader.hpp"

#include <praenora/runtime/error.hpp>

#include <utility>

namespace praenora::detail {

namespace {

using nlohmann::json;

class JsonDocumentSource final : public InputSource {
public:
    JsonDocumentSource(const InputBinding& binding, const MessageTypeDef& port_type) {
        const std::string origin = path_to_utf8(binding.path);
        // A file beyond the document limit is refused before it is read.
        check_document_size(binding.path);
        std::string bytes;
        try {
            bytes = read_file_bytes(binding.path);
        } catch (const Error& e) {
            SourceLocation location;
            location.file = origin;
            throw Error(ErrorCode::TARGET_DATA_UNAVAILABLE,
                        std::string("cannot read the document: ") + e.what(), std::move(location));
        }
        digest_ = sha256_hex(bytes);
        LoadedDocument document = parse_document_text(bytes, origin);

        const auto issues = port_type.compiled->validate(document.json);
        if (!issues.empty()) {
            const auto& first = issues.front();
            json list = json::array();
            for (const auto& issue : issues) {
                list.push_back({{"pointer", issue.pointer}, {"message", issue.message}});
            }
            std::string message = "the document does not satisfy message type " + port_type.id +
                                  "@" + to_string(port_type.version) + ": " +
                                  (first.pointer.empty() ? std::string("(root)") : first.pointer) +
                                  ": " + first.message;
            if (issues.size() > 1) {
                message += " (and " + std::to_string(issues.size() - 1) + " more problem" +
                           (issues.size() > 2 ? "s" : "") + ")";
            }
            throw Error(ErrorCode::INPUT_BINDING_INVALID, message, locate(document, first.pointer),
                        json{{"issues", std::move(list)}}.dump());
        }
        payload_ = std::move(document.json);
    }

    json batch(std::uint64_t /*tick*/, double /*window_start*/, double /*window_end*/) override {
        return payload_;
    }

    bool replay() const override { return false; }

    bool exhausted_after(double /*window_end*/) const override { return true; }

    const std::string& digest() const override { return digest_; }

private:
    json payload_;
    std::string digest_;
};

} // namespace

std::unique_ptr<InputSource> make_json_document_source(const InputBinding& binding,
                                                       const MessageTypeDef& port_type) {
    try {
        return std::make_unique<JsonDocumentSource>(binding, port_type);
    } catch (const Error& e) {
        throw Error(e.code(), "input '" + binding.input + "': " + e.what(), e.location(),
                    e.details_json());
    }
}

} // namespace praenora::detail
