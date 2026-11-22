#include "io/output_recorder.hpp"

#include "core/paths.hpp"
#include "document/json_util.hpp"

#include <praenora/runtime/error.hpp>

#include <optional>
#include <system_error>
#include <utility>

namespace praenora::detail {

namespace fs = std::filesystem;

namespace {

// Workflow output names are port names: [a-z][a-z0-9_]*. They become file names, so nothing else
// is accepted.
bool is_port_name(const std::string& name) {
    if (name.empty() || name.front() < 'a' || name.front() > 'z') {
        return false;
    }
    for (const char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

std::string quoted(const fs::path& path) { return "'" + path_to_utf8(path) + "'"; }

// An IO_ERROR located at `path`.
Error io_error(std::string message, const fs::path& path) {
    SourceLocation location;
    location.file = path_to_utf8(path);
    return Error(ErrorCode::IO_ERROR, std::move(message), std::move(location));
}

} // namespace

OutputRecorder::OutputRecorder(fs::path dir, std::vector<std::string> ports) {
    const fs::path outputs = dir / "outputs";
    std::error_code ec;
    fs::create_directories(outputs, ec);
    if (ec) {
        throw io_error("cannot create directory " + quoted(outputs) + ": " + ec.message(), outputs);
    }
    for (const std::string& port : ports) {
        if (!is_port_name(port)) {
            throw Error(ErrorCode::INVALID_ARGUMENT,
                        "'" + port + "' is not a workflow output name ([a-z][a-z0-9_]*)");
        }
        if (files_.count(port) != 0) {
            throw Error(ErrorCode::INVALID_ARGUMENT,
                        "workflow output '" + port + "' is to be recorded twice");
        }
        auto file = std::make_unique<File>();
        file->path = outputs / path_from_utf8(port + ".jsonl");
        file->stream.open(file->path, std::ios::binary | std::ios::trunc);
        if (!file->stream) {
            throw io_error("cannot create " + quoted(file->path), file->path);
        }
        file->info.port = port;
        file->info.path = "outputs/" + port + ".jsonl";
        files_.emplace(port, std::move(file));
    }
}

OutputRecorder::~OutputRecorder() = default;

void OutputRecorder::record(const std::string& port, const nlohmann::json& envelope) {
    if (done_) {
        throw Error(ErrorCode::INVALID_STATE,
                    "workflow output '" + port + "' cannot be recorded: recording is finished");
    }
    const auto it = files_.find(port);
    if (it == files_.end()) {
        throw Error(ErrorCode::INVALID_ARGUMENT, "workflow output '" + port + "' is not recorded");
    }
    File& file = *it->second;
    std::string line = canonical_json(envelope);
    line += '\n';
    file.stream.write(line.data(), static_cast<std::streamsize>(line.size()));
    if (!file.stream) {
        throw io_error("cannot write " + quoted(file.path), file.path);
    }
    file.digest.update(line.data(), line.size());
    ++file.info.messages;
}

void OutputRecorder::flush() {
    if (done_) {
        return;
    }
    for (auto& entry : files_) {
        File& file = *entry.second;
        file.stream.flush();
        if (!file.stream) {
            throw io_error("cannot write " + quoted(file.path), file.path);
        }
    }
}

std::vector<OutputRecorder::FileInfo> OutputRecorder::finish() {
    if (done_) {
        return finished_;
    }
    done_ = true;
    std::optional<Error> problem;
    for (auto& entry : files_) { // sorted by port
        File& file = *entry.second;
        file.stream.close();
        if (!file.stream && !problem) {
            problem = io_error("cannot write " + quoted(file.path), file.path);
        }
        file.info.sha256 = file.digest.finish_hex();
        finished_.push_back(file.info);
    }
    if (problem) {
        throw *problem;
    }
    return finished_;
}

} // namespace praenora::detail
