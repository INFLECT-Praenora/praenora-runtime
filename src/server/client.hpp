// The client of the control API (`praenora status --server`), over cpp-httplib's client:
// client.cpp is, with listener.cpp, the only source of praenora_server that includes httplib.h;
// this header names none of its types.
#ifndef PRAENORA_SERVER_CLIENT_HPP
#define PRAENORA_SERVER_CLIENT_HPP

#include "address.hpp"

#include <praenora/runtime/error.hpp>

#include <optional>
#include <string>

namespace praenora::server {

// What the server answered.
struct ClientAnswer {
    int status = 0;
    std::string content_type;
    std::string body;
};

// The time a client waits to connect, and to send a request or read its answer.
inline constexpr int kClientConnectSeconds = 5;
inline constexpr int kClientIoSeconds = 10;

// GETs `path` (for example "/v0/runs") from the control API listening on `address`: through its
// Unix socket, or over TCP to its IP address (never resolved), with the address as the Host
// header; with `token`, "Authorization: Bearer <token>". Throws praenora::Error(IO_ERROR), naming
// the address, when the connection fails or no answer comes in time.
ClientAnswer get(const ListenAddress& address, const std::string& path,
                 const std::optional<std::string>& token);

// The error that `answer`, one that is not a success, reports: its error document
// (Error::to_json()) as a praenora::Error, with its code (by its name), message, location and
// details; or, for a body that is no error document, IO_ERROR naming `address` and the status.
Error answer_error(const ListenAddress& address, const ClientAnswer& answer);

} // namespace praenora::server

#endif // PRAENORA_SERVER_CLIENT_HPP
