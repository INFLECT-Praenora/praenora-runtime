#include "isolation/channel.hpp"

#include "core/deadline.hpp"
#include "isolation/compiled_model_store.hpp"

#include <praenora/runtime/error.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace praenora::detail {

namespace {

constexpr std::size_t kMaxFrameLength = std::numeric_limits<std::uint32_t>::max();

void put_u32(unsigned char* out, std::uint32_t value) noexcept {
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<unsigned char>(value >> (8 * i));
    }
}

void put_u64(unsigned char* out, std::uint64_t value) noexcept {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<unsigned char>(value >> (8 * i));
    }
}

std::uint32_t get_u32(const unsigned char* in) noexcept {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(in[i]) << (8 * i);
    }
    return value;
}

std::uint64_t get_u64(const unsigned char* in) noexcept {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(in[i]) << (8 * i);
    }
    return value;
}

#ifndef _WIN32
// The payload grows by at most this much per read, so its size follows the bytes that arrived.
constexpr std::size_t kReadStep = std::size_t{64} << 10;

std::string system_text(int error) { return std::strerror(error); }

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0; // SO_NOSIGPIPE, set on the socket, does it instead
#endif
// The flags of a bounded write's sends, each made once poll() reported room: none to add on macOS,
// which ignores MSG_DONTWAIT on a stream socket pair, so such a send there is cut to what poll()
// promises room for (Channel::send_chunk()); elsewhere MSG_DONTWAIT, so that a send takes what the
// stream has room for and returns, however much it was given (Linux's send timeout restarts for
// each buffer a send waits for, and so bounds no whole send).
#if defined(__APPLE__)
constexpr int kBoundedSendFlags = kSendFlags;
#else
constexpr int kBoundedSendFlags = kSendFlags | MSG_DONTWAIT;
#endif
#endif

} // namespace

const char* frame_kind_name(std::uint32_t kind) noexcept {
    switch (static_cast<FrameKind>(kind)) {
    case FrameKind::hello:
        return "hello";
    case FrameKind::load:
        return "load";
    case FrameKind::create:
        return "create";
    case FrameKind::initialize:
        return "initialize";
    case FrameKind::invoke:
        return "invoke";
    case FrameKind::finalize:
        return "finalize";
    case FrameKind::destroy:
        return "destroy";
    case FrameKind::diagnostics:
        return "diagnostics";
    case FrameKind::cancel:
        return "cancel";
    case FrameKind::exit:
        return "exit";
    case FrameKind::store_reply:
        return "store_reply";
    case FrameKind::ready:
        return "ready";
    case FrameKind::loaded:
        return "loaded";
    case FrameKind::load_failed:
        return "load_failed";
    case FrameKind::sink:
        return "sink";
    case FrameKind::log:
        return "log";
    case FrameKind::lookup:
        return "lookup";
    case FrameKind::result:
        return "result";
    case FrameKind::provider_log:
        return "provider_log";
    case FrameKind::store_load:
        return "store_load";
    case FrameKind::store_save:
        return "store_save";
    case FrameKind::store_refused:
        return "store_refused";
    }
    return nullptr;
}

std::size_t max_payload(FrameKind kind) noexcept {
    switch (kind) {
    case FrameKind::hello:
    case FrameKind::ready:
        return std::size_t{1} << 10;
    case FrameKind::load:
        return std::size_t{1} << 20;
    case FrameKind::create:
    case FrameKind::initialize:
    case FrameKind::invoke:
    case FrameKind::finalize:
    case FrameKind::destroy:
    case FrameKind::diagnostics:
        // Documents from the runtime itself, whatever their size.
        return kMaxFrameLength;
    case FrameKind::cancel:
    case FrameKind::exit:
        return 0;
    case FrameKind::loaded:
    case FrameKind::load_failed:
        return std::size_t{4} << 20;
    case FrameKind::sink:
        return 1 + kMaxSinkChunkBytes;
    case FrameKind::log:
        return 4 + kMaxLogMessageBytes;
    case FrameKind::lookup:
        return 5 + kMaxServiceNameBytes;
    case FrameKind::result:
        return 5;
    case FrameKind::provider_log:
        return 4 + 4 + 4 + kMaxLogAttributesBytes + kMaxLogMessageBytes;
    case FrameKind::store_load:
        return 4 + kMaxCompiledModelKeyBytes;
    case FrameKind::store_save:
        return 4 + 4 + kMaxCompiledModelKeyBytes + kMaxCompiledModelBytes;
    case FrameKind::store_refused:
        return 4 + 8 + 8 + kMaxCompiledModelKeyBytes;
    case FrameKind::store_reply:
        return 1 + kMaxCompiledModelBytes;
    }
    return 0;
}

std::array<unsigned char, kFrameHeaderSize> encode_header(FrameKind kind, std::uint64_t id,
                                                          std::size_t payload_size) noexcept {
    std::array<unsigned char, kFrameHeaderSize> header{};
    put_u32(header.data(), kFrameMagic);
    put_u32(header.data() + 4, static_cast<std::uint32_t>(kind));
    put_u64(header.data() + 8, id);
    put_u32(header.data() + 16, static_cast<std::uint32_t>(payload_size));
    return header;
}

std::string encode_frame(const Frame& frame) {
    if (frame.payload.size() > max_payload(frame.kind)) {
        throw Error(ErrorCode::INVALID_ARGUMENT,
                    std::string("a ") + frame_kind_name(static_cast<std::uint32_t>(frame.kind)) +
                        " frame may carry at most " + std::to_string(max_payload(frame.kind)) +
                        " bytes, not " + std::to_string(frame.payload.size()));
    }
    const auto header = encode_header(frame.kind, frame.id, frame.payload.size());
    std::string bytes(reinterpret_cast<const char*>(header.data()), header.size());
    bytes += frame.payload;
    return bytes;
}

std::optional<FrameHeader> decode_header(const unsigned char* bytes, std::string& problem) {
    const std::uint32_t magic = get_u32(bytes);
    if (magic != kFrameMagic) {
        problem = "a frame header has the magic " + std::to_string(magic) + ", not " +
                  std::to_string(kFrameMagic);
        return std::nullopt;
    }
    const std::uint32_t kind = get_u32(bytes + 4);
    const char* name = frame_kind_name(kind);
    if (name == nullptr) {
        problem = "a frame header names the unknown kind " + std::to_string(kind);
        return std::nullopt;
    }
    FrameHeader header;
    header.kind = static_cast<FrameKind>(kind);
    header.id = get_u64(bytes + 8);
    header.payload_size = get_u32(bytes + 16);
    if (header.payload_size > max_payload(header.kind)) {
        problem = std::string("a ") + name + " frame announces " +
                  std::to_string(header.payload_size) + " bytes, over its limit of " +
                  std::to_string(max_payload(header.kind));
        return std::nullopt;
    }
    return header;
}

PayloadWriter& PayloadWriter::u8(std::uint8_t value) {
    text_.push_back(static_cast<char>(value));
    return *this;
}

PayloadWriter& PayloadWriter::u32(std::uint32_t value) {
    unsigned char bytes[4];
    put_u32(bytes, value);
    text_.append(reinterpret_cast<const char*>(bytes), sizeof bytes);
    return *this;
}

PayloadWriter& PayloadWriter::u64(std::uint64_t value) {
    unsigned char bytes[8];
    put_u64(bytes, value);
    text_.append(reinterpret_cast<const char*>(bytes), sizeof bytes);
    return *this;
}

PayloadWriter& PayloadWriter::i32(std::int32_t value) {
    return u32(static_cast<std::uint32_t>(value));
}

PayloadWriter& PayloadWriter::bytes(std::string_view value) {
    text_.append(value.data(), value.size());
    return *this;
}

std::string PayloadWriter::take() noexcept { return std::move(text_); }

bool PayloadReader::take(std::size_t size, std::string_view& field) noexcept {
    if (rest_.size() < size) {
        return false;
    }
    field = rest_.substr(0, size);
    rest_.remove_prefix(size);
    return true;
}

bool PayloadReader::u8(std::uint8_t& value) noexcept {
    std::string_view field;
    if (!take(1, field)) {
        return false;
    }
    value = static_cast<std::uint8_t>(field[0]);
    return true;
}

bool PayloadReader::u32(std::uint32_t& value) noexcept {
    std::string_view field;
    if (!take(4, field)) {
        return false;
    }
    value = get_u32(reinterpret_cast<const unsigned char*>(field.data()));
    return true;
}

bool PayloadReader::u64(std::uint64_t& value) noexcept {
    std::string_view field;
    if (!take(8, field)) {
        return false;
    }
    value = get_u64(reinterpret_cast<const unsigned char*>(field.data()));
    return true;
}

bool PayloadReader::i32(std::int32_t& value) noexcept {
    std::uint32_t raw = 0;
    if (!u32(raw)) {
        return false;
    }
    value = static_cast<std::int32_t>(raw);
    return true;
}

bool PayloadReader::bytes(std::size_t size, std::string_view& value) noexcept {
    return take(size, value);
}

std::string_view PayloadReader::rest() noexcept { return std::exchange(rest_, std::string_view()); }

void Channel::refuse_kinds(std::initializer_list<FrameKind> kinds, std::string why) {
    for (const FrameKind kind : kinds) {
        refused_kinds_ |= std::uint64_t{1} << (static_cast<std::uint32_t>(kind) & 63u);
    }
    refused_why_ = std::move(why);
}

#ifndef _WIN32

std::pair<int, int> open_channel_pair() {
    int ends[2] = {-1, -1};
#if defined(SOCK_CLOEXEC)
    const int type = SOCK_STREAM | SOCK_CLOEXEC;
#else
    const int type = SOCK_STREAM;
#endif
    if (::socketpair(AF_UNIX, type, 0, ends) != 0) {
        const int error = errno;
        throw Error(ErrorCode::MODULE_HOST_UNAVAILABLE,
                    "cannot make a channel for a module host: " + system_text(error), {},
                    nlohmann::json{{"reason", "spawn"}, {"system", system_text(error)}}.dump());
    }
#if !defined(SOCK_CLOEXEC)
    // macOS has no SOCK_CLOEXEC. A process spawned by another thread meanwhile could inherit the
    // descriptors; the runtime's own spawns close every descriptor they do not pass on.
    for (const int end : ends) {
        static_cast<void>(::fcntl(end, F_SETFD, FD_CLOEXEC));
    }
#endif
    protect_from_sigpipe(ends[0]);
    protect_from_sigpipe(ends[1]);
    return {ends[0], ends[1]};
}

void protect_from_sigpipe(int descriptor) noexcept {
#if defined(SO_NOSIGPIPE)
    int on = 1;
    static_cast<void>(::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on));
#else
    static_cast<void>(descriptor);
#endif
}

Channel::Channel(int descriptor) noexcept : descriptor_(descriptor) {}

Channel::~Channel() {
    if (descriptor_ >= 0) {
        static_cast<void>(::close(descriptor_));
    }
}

int Channel::descriptor() const noexcept { return descriptor_; }

void Channel::shut_down() noexcept { static_cast<void>(::shutdown(descriptor_, SHUT_RDWR)); }

bool Channel::wait_readable(std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        pollfd waiting{descriptor_, POLLIN, 0};
        const int ready =
            ::poll(&waiting, 1,
                   static_cast<int>(std::clamp<std::int64_t>(left.count() + 1, 0, 60 * 60 * 1000)));
        if (ready > 0 || (ready < 0 && errno != EINTR)) {
            return true; // the read reports what there is, an error included
        }
        if (ready == 0 && std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
    }
}

Channel::ReadResult Channel::read(std::optional<std::chrono::steady_clock::time_point> deadline) {
    ReadResult result;
    // Reads exactly `size` bytes into `out`; `got` counts those read so far.
    enum class Fill { whole, end, failed, timed_out };
    const auto fill = [this, &deadline, &result](char* out, std::size_t size,
                                                 std::size_t& got) -> Fill {
        while (got < size) {
            if (deadline) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    *deadline - std::chrono::steady_clock::now());
                pollfd waiting{descriptor_, POLLIN, 0};
                const int ready = ::poll(&waiting, 1,
                                         static_cast<int>(std::clamp<std::int64_t>(
                                             left.count() + 1, 0, 60 * 60 * 1000)));
                if (ready < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    result.problem = "cannot wait for the channel: " + system_text(errno);
                    return Fill::failed;
                }
                if (ready == 0) {
                    if (std::chrono::steady_clock::now() >= *deadline) {
                        return Fill::timed_out;
                    }
                    continue;
                }
            }
            const ssize_t n = ::recv(descriptor_, out + got, size - got, 0);
            if (n > 0) {
                got += static_cast<std::size_t>(n);
            } else if (n == 0) {
                return Fill::end;
            } else if (errno != EINTR) {
                result.problem = "cannot read the channel: " + system_text(errno);
                return Fill::failed;
            }
        }
        return Fill::whole;
    };

    unsigned char header_bytes[kFrameHeaderSize];
    std::size_t got = 0;
    switch (fill(reinterpret_cast<char*>(header_bytes), kFrameHeaderSize, got)) {
    case Fill::whole:
        break;
    case Fill::end:
        if (got == 0) {
            result.status = ReadStatus::end;
            return result;
        }
        result.status = ReadStatus::broken;
        result.problem = "the channel ended within a frame header (" + std::to_string(got) +
                         " of " + std::to_string(kFrameHeaderSize) + " bytes)";
        return result;
    case Fill::failed:
        result.status = ReadStatus::broken;
        return result;
    case Fill::timed_out:
        result.status = ReadStatus::timed_out;
        return result;
    }
    std::string problem;
    const auto header = decode_header(header_bytes, problem);
    if (!header) {
        result.status = ReadStatus::broken;
        result.problem = std::move(problem);
        return result;
    }
    if (((refused_kinds_ >> (static_cast<std::uint32_t>(header->kind) & 63u)) & 1u) != 0) {
        // Refused at its header: none of its payload is read.
        result.status = ReadStatus::broken;
        result.problem = std::string("a ") +
                         frame_kind_name(static_cast<std::uint32_t>(header->kind)) + " frame " +
                         refused_why_;
        return result;
    }
    result.frame.kind = header->kind;
    result.frame.id = header->id;
    std::string& payload = result.frame.payload;
    const std::size_t size = header->payload_size;
    std::size_t done = 0;
    while (done < size) {
        // Grow only by what is about to be read, and never beyond the announced length: the
        // buffer at most doubles at a time, up to that length.
        const std::size_t step = std::min(kReadStep, size - done);
        if (done + step > payload.capacity()) {
            payload.reserve(std::min(size, std::max(done + step, 2 * payload.capacity())));
        }
        payload.resize(done + step);
        std::size_t got_step = 0;
        const Fill filled = fill(&payload[done], step, got_step);
        done += got_step;
        if (filled == Fill::whole) {
            continue;
        }
        payload.resize(done);
        if (filled == Fill::timed_out) {
            result.status = ReadStatus::timed_out;
            return result;
        }
        result.status = ReadStatus::broken;
        if (filled == Fill::end) {
            result.problem = std::string("the channel ended within the payload of a ") +
                             frame_kind_name(static_cast<std::uint32_t>(header->kind)) +
                             " frame (" + std::to_string(done) + " of " + std::to_string(size) +
                             " bytes)";
        }
        return result;
    }
    result.status = ReadStatus::frame;
    return result;
}

bool Channel::write(const Frame& frame, std::string* problem) {
    return write_parts(frame.kind, frame.id, {frame.payload}, problem);
}

bool Channel::write_parts(FrameKind kind, std::uint64_t id,
                          std::initializer_list<std::string_view> payload, std::string* problem) {
    return send(kind, id, payload, std::nullopt, nullptr, problem) == WriteStatus::written;
}

Channel::WriteStatus Channel::write_parts_until(FrameKind kind, std::uint64_t id,
                                                std::initializer_list<std::string_view> payload,
                                                std::chrono::steady_clock::time_point deadline,
                                                std::string* problem) {
    return send(kind, id, payload, deadline, nullptr, problem);
}

Channel::WriteStatus Channel::write_parts_while(
    FrameKind kind, std::uint64_t id, std::initializer_list<std::string_view> payload,
    const std::function<bool()>& go_on,
    std::optional<std::chrono::steady_clock::time_point> deadline, std::string* problem) {
    return send(kind, id, payload, deadline, &go_on, problem);
}

bool Channel::broken() const noexcept { return broken_.load(); }

bool Channel::set_send_timeout(std::chrono::nanoseconds wait, std::string* problem) {
    if (wait == send_timeout_) {
        return true;
    }
    timeval value{};
    if (wait > std::chrono::nanoseconds::zero()) {
        const auto micros = std::max<std::int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(wait).count(), 1);
        value.tv_sec = static_cast<decltype(value.tv_sec)>(micros / 1000000);
        value.tv_usec = static_cast<decltype(value.tv_usec)>(micros % 1000000);
    }
    if (::setsockopt(descriptor_, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof value) != 0) {
        if (problem != nullptr) {
            *problem = "cannot bound a write to the channel: " + system_text(errno);
        }
        return false;
    }
    send_timeout_ = wait;
    return true;
}

Channel::WriteStatus Channel::send(FrameKind kind, std::uint64_t id,
                                   std::initializer_list<std::string_view> payload,
                                   std::optional<std::chrono::steady_clock::time_point> deadline,
                                   const std::function<bool()>* go_on, std::string* problem) {
    using std::chrono::steady_clock;
    constexpr std::size_t kMaxParts = 8;
    const auto say = [problem](const std::string& text) {
        if (problem != nullptr) {
            *problem = text;
        }
    };
    std::size_t size = 0;
    for (const std::string_view part : payload) {
        size += part.size();
    }
    if (size > max_payload(kind) || payload.size() > kMaxParts) {
        say(std::string("a ") + frame_kind_name(static_cast<std::uint32_t>(kind)) +
            " frame may carry at most " + std::to_string(max_payload(kind)) +
            " bytes, in at most " + std::to_string(kMaxParts) + " parts");
        return WriteStatus::refused;
    }
    auto header = encode_header(kind, id, size);
    iovec parts[1 + kMaxParts];
    std::size_t count = 0;
    parts[count].iov_base = header.data();
    parts[count++].iov_len = header.size();
    for (const std::string_view part : payload) {
        // sendmsg() only reads the payload; iovec has no const member.
        parts[count].iov_base = const_cast<char*>(part.data());
        parts[count++].iov_len = part.size();
    }
    const bool bounded = deadline.has_value() || go_on != nullptr;
    const auto told_to_stop = [go_on] { return go_on != nullptr && !(*go_on)(); };

    // The writer's turn: waited for in slices when the write is bounded.
    std::unique_lock<std::timed_mutex> lock(write_mutex_, std::defer_lock);
    if (!bounded) {
        lock.lock();
    } else {
        for (;;) {
            const auto now = steady_clock::now();
            if (deadline && now >= *deadline) {
                say("another write held the channel until the deadline");
                return WriteStatus::timed_out;
            }
            auto until = go_on != nullptr ? deadline_after(now, kWriteSlice) : *deadline;
            if (deadline && *deadline < until) {
                until = *deadline;
            }
            if (lock.try_lock_until(until)) {
                break;
            }
            if (told_to_stop()) {
                say("the write was told to stop while another write held the channel");
                return WriteStatus::stopped;
            }
        }
    }
    if (broken_.load()) {
        say("the channel is broken: an earlier write was cut short within its frame");
        return WriteStatus::failed;
    }

    bool wrote = false; // part of the frame is in the stream
    const auto give_up = [this, &wrote, &say](WriteStatus status, const std::string& why) {
        if (wrote) {
            broken_.store(true);
        }
        say(why);
        return status;
    };
    // A bounded write keeps its bounds itself: the system's send timeout bounds each wait for room
    // in the stream, not a whole sendmsg() (macOS's, and Linux's, which restarts it for each buffer
    // of the stream it waits for), so a peer that keeps taking a few bytes could stretch one send
    // far beyond them. It waits for room with poll(), until the end of the slice or the deadline,
    // and then sends without waiting: on macOS no more than the stream has room for
    // (send_chunk()), elsewhere with MSG_DONTWAIT (kBoundedSendFlags), which takes what fits. A
    // send timeout of one slice stays as a second bound.
    const std::size_t chunk = bounded ? send_chunk() : 0;
    const int flags = bounded ? kBoundedSendFlags : kSendFlags;
    std::string why;
    if (!set_send_timeout(bounded ? std::chrono::nanoseconds(kWriteSlice)
                                  : std::chrono::nanoseconds::zero(),
                          &why)) {
        return give_up(WriteStatus::failed, why);
    }
    auto slice_end = deadline_after(steady_clock::now(), kWriteSlice);
    std::size_t first = 0;
    while (first < count) {
        if (parts[first].iov_len == 0) {
            ++first;
            continue;
        }
        // A bounded write waits for room until the end of the slice or the deadline, whichever
        // comes first.
        if (bounded) {
            auto now = steady_clock::now();
            if (deadline && now >= *deadline) {
                return give_up(WriteStatus::timed_out,
                               "the channel took no more bytes before the deadline");
            }
            if (go_on != nullptr && now >= slice_end) {
                // A slice passed with the frame unfinished: ask whether to go on.
                if (told_to_stop()) {
                    return give_up(WriteStatus::stopped, "the write was told to stop");
                }
                now = steady_clock::now();
                slice_end = deadline_after(now, kWriteSlice);
            }
            auto until = go_on != nullptr ? slice_end : *deadline;
            if (deadline && *deadline < until) {
                until = *deadline;
            }
            const auto millis = std::chrono::ceil<std::chrono::milliseconds>(until - now).count();
            pollfd room{descriptor_, POLLOUT, 0};
            const int ready =
                ::poll(&room, 1, static_cast<int>(std::clamp<std::int64_t>(millis, 1, 1000)));
            if (ready < 0 && errno != EINTR) {
                return give_up(WriteStatus::failed,
                               "cannot wait for room in the channel: " + system_text(errno));
            }
            if (ready <= 0) {
                continue; // no room yet: the deadline and the slice are looked at again
            }
            // Room, or an end or an error, which the send reports.
        }
        // The parts left, a bounded write's cut to `chunk` bytes.
        iovec cut[1 + kMaxParts];
        std::size_t cut_count = 0;
        std::size_t left = 0;
        for (std::size_t i = first; i < count && (chunk == 0 || left < chunk); ++i) {
            cut[cut_count] = parts[i];
            if (chunk != 0) {
                cut[cut_count].iov_len = std::min(cut[cut_count].iov_len, chunk - left);
            }
            left += cut[cut_count].iov_len;
            ++cut_count;
        }
        msghdr message{};
        message.msg_iov = cut;
        message.msg_iovlen = static_cast<decltype(message.msg_iovlen)>(cut_count);
        const ssize_t n = ::sendmsg(descriptor_, &message, flags);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (bounded && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // No room after all, or the send timeout passed: on to the deadline, or the
                // slice's end.
                continue;
            }
            return give_up(WriteStatus::failed,
                           "cannot write to the channel: " + system_text(errno));
        }
        auto sent = static_cast<std::size_t>(n);
        if (sent > 0) {
            wrote = true;
        }
        while (sent > 0 && first < count) {
            const std::size_t used = std::min(sent, parts[first].iov_len);
            parts[first].iov_base = static_cast<char*>(parts[first].iov_base) + used;
            parts[first].iov_len -= used;
            sent -= used;
            if (parts[first].iov_len == 0) {
                ++first;
            }
        }
    }
    return WriteStatus::written;
}

std::size_t Channel::send_chunk() noexcept {
#if defined(__APPLE__)
    // poll() reports room once the stream can take the send low-water mark (2048 bytes unless
    // changed): a send of at most that many bytes then never waits.
    if (send_chunk_ == 0) {
        int low_water = 0;
        socklen_t size = sizeof low_water;
        send_chunk_ = ::getsockopt(descriptor_, SOL_SOCKET, SO_SNDLOWAT, &low_water, &size) == 0 &&
                              low_water > 0
                          ? static_cast<std::size_t>(low_water)
                          : 1;
    }
    return send_chunk_;
#else
    return 0; // no cut: MSG_DONTWAIT keeps the send from waiting (kBoundedSendFlags)
#endif
}

#else // _WIN32: process isolation is not available.

std::pair<int, int> open_channel_pair() {
    throw Error(ErrorCode::MODULE_HOST_UNAVAILABLE,
                "process isolation is not available on Windows in this version", {},
                nlohmann::json{{"reason", "spawn"}}.dump());
}

void protect_from_sigpipe(int) noexcept {}

Channel::Channel(int descriptor) noexcept : descriptor_(descriptor) {}

Channel::~Channel() = default;

int Channel::descriptor() const noexcept { return descriptor_; }

void Channel::shut_down() noexcept {}

bool Channel::wait_readable(std::chrono::steady_clock::time_point) { return true; }

Channel::ReadResult Channel::read(std::optional<std::chrono::steady_clock::time_point>) {
    ReadResult result;
    result.status = ReadStatus::broken;
    result.problem = "process isolation is not available on Windows in this version";
    return result;
}

bool Channel::write(const Frame&, std::string* problem) {
    if (problem != nullptr) {
        *problem = "process isolation is not available on Windows in this version";
    }
    return false;
}

bool Channel::write_parts(FrameKind, std::uint64_t, std::initializer_list<std::string_view>,
                          std::string* problem) {
    return write(Frame{}, problem);
}

Channel::WriteStatus Channel::write_parts_until(FrameKind, std::uint64_t,
                                                std::initializer_list<std::string_view>,
                                                std::chrono::steady_clock::time_point,
                                                std::string* problem) {
    static_cast<void>(write(Frame{}, problem));
    return WriteStatus::failed;
}

Channel::WriteStatus Channel::write_parts_while(
    FrameKind, std::uint64_t, std::initializer_list<std::string_view>, const std::function<bool()>&,
    std::optional<std::chrono::steady_clock::time_point>, std::string* problem) {
    static_cast<void>(write(Frame{}, problem));
    return WriteStatus::failed;
}

bool Channel::broken() const noexcept { return false; }

#endif

} // namespace praenora::detail
