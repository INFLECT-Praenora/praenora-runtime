#include "serving.hpp"

#include <clock.hpp>

#include <frontend.hpp>

#include <praenora/runtime/error.hpp>

#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace praenora::cli {

namespace fs = std::filesystem;

const std::vector<Flag>& listen_flags() {
    static const std::vector<Flag> flags = {{"--listen", true},
                                            {"--token-file", true},
                                            {"--read-token-file", true},
                                            {"--allow-host", true, true},
                                            {"--address-file", true}};
    return flags;
}

std::string listen_flags_usage() {
    return "  --listen ADDR          serve the control API on ADDR: unix:<path> (POSIX), "
           "<IPv4>:<port>\n"
           "                         or [<IPv6>]:<port>, port 0 letting the system choose; host\n"
           "                         names are refused\n"
           "  --token-file F         the token of the read and control scopes, on the first line "
           "of F\n"
           "                         (a TCP listener; F readable by its owner only)\n"
           "  --read-token-file F    the token of the read scope only, likewise\n"
           "  --allow-host H         a name the Host header of a TCP request may give "
           "(repeatable)\n"
           "  --address-file F       write the address listened on, with its port, to F\n";
}

std::optional<ListenSettings> listen_settings(const Arguments& arguments) {
    const std::optional<std::string> listen = arguments.value("--listen");
    if (!listen) {
        for (const Flag& flag : listen_flags()) {
            if (flag.name != "--listen" && arguments.has(flag.name)) {
                throw UsageError(std::string(flag.name) + " needs --listen");
            }
        }
        return std::nullopt;
    }
    ListenSettings settings;
    settings.address = as_usage([&] { return server::parse_listen_address(*listen); });
    settings.tokens = as_usage([&] {
        return server::load_tokens(arguments.value("--token-file"),
                                   arguments.value("--read-token-file"));
    });
    as_usage([&] {
        server::check_credentials(settings.address, settings.tokens);
        return 0;
    });
    settings.allow_hosts = arguments.values("--allow-host");
    for (const std::string& name : settings.allow_hosts) {
        as_usage([&] {
            server::check_allow_host(name);
            return 0;
        });
    }
    settings.address_file = arguments.value("--address-file");
    if (settings.address_file && settings.address_file->empty()) {
        throw UsageError("--address-file needs a file name");
    }
    return settings;
}

void write_address_file(const std::string& file, const std::string& text) {
    const fs::path target = frontend::path_from_utf8(file);
    fs::path temporary = target;
    temporary += ".tmp-" + std::to_string(server::current_process_id());
    const auto failed_to = [&](const std::string& why) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return Error(ErrorCode::IO_ERROR,
                     "the address file '" + file + "' cannot be written: " + why);
    };
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << text << '\n';
        out.close();
        if (!out) {
            throw failed_to("the file beside it, to be renamed over it, cannot be written");
        }
    }
    std::error_code ec;
    fs::rename(temporary, target, ec);
    if (ec) {
        throw failed_to(ec.message());
    }
}

Server::Server(const ListenSettings& settings, const Output& output)
    : settings_(settings), listener_(settings.address) {
    const std::string listened = listener_.address().to_string();
    output.note("listening on " + listened);
    if (settings_.address_file) {
        write_address_file(*settings_.address_file, listened);
    }
}

void Server::start(const Runtime& runtime, server::RunSource& source) {
    server::ControlApiSettings api_settings;
    api_settings.listener = listener_.address();
    api_settings.tokens = settings_.tokens;
    api_settings.allow_hosts = settings_.allow_hosts;
    api_settings.started_at_unix_ns = server::now_unix_ns();
    api_settings.pid = server::current_process_id();
    api_ = std::make_unique<server::ControlApi>(runtime, source, std::move(api_settings));
    const server::ControlApi& api = *api_;
    listener_.start([&api](const server::Request& request) { return api.handle(request); });
}

Server::~Server() { stop(); }

std::string Server::address() const { return listener_.address().to_string(); }

void Server::stop() { listener_.stop(); }

} // namespace praenora::cli
