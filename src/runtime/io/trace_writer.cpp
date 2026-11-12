#include "io/trace_writer.hpp"

#include "core/paths.hpp"
#include "document/json_util.hpp"

#include <praenora/runtime/error.hpp>

#include <system_error>
#include <utility>

namespace praenora::detail {

namespace fs = std::filesystem;

namespace {

std::string quoted(const fs::path& path) { return "'" + path_to_utf8(path) + "'"; }

// An IO_ERROR located at `path`.
Error io_error(std::string message, const fs::path& path) {
    SourceLocation location;
    location.file = path_to_utf8(path);
    return Error(ErrorCode::IO_ERROR, std::move(message), std::move(location));
}

} // namespace

TraceWriter::TraceWriter(fs::path file) : file_(std::move(file)) {
    std::error_code ec;
    if (file_.has_parent_path()) {
        fs::create_directories(file_.parent_path(), ec);
        if (ec) {
            throw io_error("cannot create directory " + quoted(file_.parent_path()) + ": " +
                               ec.message(),
                           file_.parent_path());
        }
    }
    stream_.open(file_, std::ios::binary | std::ios::trunc);
    if (!stream_) {
        throw io_error("cannot create " + quoted(file_), file_);
    }
}

void TraceWriter::write(std::uint64_t tick, const std::string& step, const char* direction,
                        const std::string& port, const nlohmann::json& envelope,
                        const nlohmann::json* diagnostics) {
    nlohmann::json record = {{"tick", tick},
                             {"step", step},
                             {"direction", direction},
                             {"port", port},
                             {"envelope", envelope}};
    if (diagnostics != nullptr) {
        record["diagnostics"] = *diagnostics;
    }
    write_line(record);
    ++summary_.messages;
}

void TraceWriter::write_diagnostics(std::uint64_t tick, const std::string& step,
                                    const nlohmann::json& diagnostics) {
    write_line({{"tick", tick},
                {"step", step},
                {"direction", "out"},
                {"port", nullptr},
                {"envelope", nullptr},
                {"diagnostics", diagnostics}});
}

void TraceWriter::write_line(const nlohmann::json& record) {
    if (done_) {
        throw Error(ErrorCode::INVALID_STATE, "the message trace is already finished");
    }
    std::string text = canonical_json(record);
    text += '\n';
    stream_.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream_) {
        throw io_error("cannot write " + quoted(file_), file_);
    }
    digest_.update(text.data(), text.size());
}

void TraceWriter::flush() {
    if (done_) {
        return;
    }
    stream_.flush();
    if (!stream_) {
        throw io_error("cannot write " + quoted(file_), file_);
    }
}

TraceWriter::Summary TraceWriter::finish() {
    if (done_) {
        return summary_;
    }
    done_ = true;
    stream_.close();
    summary_.sha256 = digest_.finish_hex();
    if (!stream_) {
        throw io_error("cannot write " + quoted(file_), file_);
    }
    return summary_;
}

} // namespace praenora::detail
