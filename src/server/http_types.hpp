// The plain request and response of the control API: what a listener hands to
// ControlApi::handle() and what it writes back. Nothing here knows sockets.
#ifndef PRAENORA_SERVER_HTTP_TYPES_HPP
#define PRAENORA_SERVER_HTTP_TYPES_HPP

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace praenora::server {

// The kind of listener a request came through: a Unix domain socket, whose mode is the
// credential, or a TCP address.
enum class ListenerKind { unix_socket, tcp };

struct Request {
    std::string method; // as sent, for example "GET" (methods are case-sensitive)
    std::string path;   // decoded, without the query, for example "/v0/runs/1"
    std::string query;  // the text after '?', empty when there is none; no route reads it
    // Header names in lower case, values as sent; a name may appear more than once.
    std::multimap<std::string, std::string, std::less<>> headers;
    std::string body;
    ListenerKind listener = ListenerKind::tcp;

    // Every value of header `name` (lower case), in the order received.
    std::vector<std::string> header_values(std::string_view name) const {
        std::vector<std::string> values;
        const auto [first, last] = headers.equal_range(name);
        for (auto it = first; it != last; ++it) {
            values.push_back(it->second);
        }
        return values;
    }
};

struct Response {
    int status = 200;
    std::string content_type; // empty for a response without a body (204)
    // Headers besides Content-Type, in the order they are to be written.
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    // The value of the first header named `name` (as written, case-sensitive), or nullptr.
    const std::string* header(std::string_view name) const {
        for (const auto& [key, value] : headers) {
            if (key == name) {
                return &value;
            }
        }
        return nullptr;
    }
};

} // namespace praenora::server

#endif // PRAENORA_SERVER_HTTP_TYPES_HPP
