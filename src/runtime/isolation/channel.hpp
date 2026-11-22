// The channel between the runtime and one module host (docs/specs/process-isolation.md): frames
// over a connected stream socket, whose module-host end is that process's descriptor 3. It is an
// internal protocol of one runtime build, never a public format: the handshake refuses a module
// host of another runtime version or channel protocol.
//
// A frame is a header of kFrameHeaderSize bytes, little-endian (the magic kFrameMagic, the kind,
// the id of the request or call it belongs to, and the payload's length), then the payload. A
// reader checks the magic, the kind and the length against the kind's limit (max_payload())
// before it reads any of the payload, and grows the payload only as its bytes arrive and never
// beyond the announced length, so a header that announces a huge length never makes it allocate
// that much. What the module host sends is untrusted input to the runtime: every frame kind it may
// send has a limit, small but for the store's compiled models.
//
// On Windows the frame codec compiles and works, and the descriptor functions refuse: process
// isolation is not available there in this version.
#ifndef PRAENORA_RUNTIME_ISOLATION_CHANNEL_HPP
#define PRAENORA_RUNTIME_ISOLATION_CHANNEL_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace praenora::detail {

// The number of the channel protocol, compiled into the runtime and the module host alike.
inline constexpr std::uint32_t kChannelProtocol = 1;
// The first four bytes of every frame: "PRMH".
inline constexpr std::uint32_t kFrameMagic = 0x484D5250u;
inline constexpr std::size_t kFrameHeaderSize = 20;

// The kinds of frame. Payloads are made of little-endian fields (PayloadWriter) or are JSON text.
enum class FrameKind : std::uint32_t {
    // From the runtime to the module host.
    hello = 1,        // u32 protocol, then the runtime's version
    load = 2,         // JSON {"manifest", "manifest_sha256", "library_sha256" (or null),
                      //  "providers": [{"manifest", "manifest_sha256", "library",
                      //  "library_sha256", "services": [{"name", "major", "minor"}],
                      //  "log_level"}]}
    create = 3,       // a call: u64 output limit, u64 error limit, i32 log threshold, u8 cancelled,
    initialize = 4,   // then the call's document; the id numbers the call
    invoke = 5,       //
    finalize = 6,     //
    destroy = 7,      //
    diagnostics = 8,  //
    cancel = 9,       // empty; the id names the call to cancel
    exit = 10,        // empty
    store_reply = 11, // the answer to the store_load of the id: u8 found, then the entry
    // From the module host to the runtime.
    ready = 16,        // u32 protocol, then the module host's version
    loaded = 17,       // JSON {"describe", "library_sha256", "diagnostics", "self_test"}
    load_failed = 18,  // JSON {"code", "message", "pointer", "line", "column", "details",
                       //  "provider" (the index of the provider whose manifest it is located in,
                       //  or null for the module's)}
    sink = 19,         // u8 sink (0: output, 1: error), then bytes the module wrote
    log = 20,          // i32 level (PRAENORA_LOG_*), then the message
    lookup = 21,       // u32 major, u8 provided, then the service's name
    result = 22,       // i32 status, u8 whether create made an instance
    provider_log = 23, // u32 provider (its index in the load), i32 level (PRAENORA_LOG_*), u32
                       // size, then that many bytes of the record's attributes (JSON
                       // [[key, value], ...]), then the message
    // The shared compiled models (isolation/compiled_model_store.hpp), only from the module host
    // of a step that shares them; the id numbers the module host's store requests.
    store_load = 24,    // u32 provider (its index in the load), then the key
    store_save = 25,    // u32 provider, u32 key size, that many bytes of key, then the entry
    store_refused = 26, // u32 provider, u64 key size, u64 entry size, then the key's first bytes
                        // (at most kMaxCompiledModelKeyBytes): a save the module host refused
};

// The kind's name ("hello", "invoke", "load_failed", ...), or nullptr for a value that names no
// kind.
const char* frame_kind_name(std::uint32_t kind) noexcept;

// The largest payload a frame of `kind` may carry.
std::size_t max_payload(FrameKind kind) noexcept;

// Limits of what the module host sends, below the frame limits: the longest chunk of a sink
// write, log message and service name it passes on. A longer sink write travels in several
// chunks; a longer log message or name is cut to this length.
inline constexpr std::size_t kMaxSinkChunkBytes = std::size_t{1} << 20;
inline constexpr std::size_t kMaxLogMessageBytes = std::size_t{1} << 20;
inline constexpr std::size_t kMaxServiceNameBytes = std::size_t{4} << 10;
// The longest attributes of a provider's log record that the module host passes on; a record with
// longer ones goes without them.
inline constexpr std::size_t kMaxLogAttributesBytes = std::size_t{64} << 10;

struct Frame {
    FrameKind kind = FrameKind::hello;
    std::uint64_t id = 0;
    std::string payload;
};

struct FrameHeader {
    FrameKind kind = FrameKind::hello;
    std::uint64_t id = 0;
    std::uint32_t payload_size = 0;
};

// The header of a frame as it travels. `payload_size` must fit in 32 bits.
std::array<unsigned char, kFrameHeaderSize> encode_header(FrameKind kind, std::uint64_t id,
                                                          std::size_t payload_size) noexcept;

// The frame as it travels, its header and its payload. Throws praenora::Error(INVALID_ARGUMENT)
// when the payload is larger than max_payload(frame.kind).
std::string encode_frame(const Frame& frame);

// Decodes and checks the kFrameHeaderSize bytes at `bytes`: nullopt, with the reason in `problem`,
// when the magic is wrong, the kind is unknown or the length exceeds the kind's limit.
std::optional<FrameHeader> decode_header(const unsigned char* bytes, std::string& problem);

// Builds a payload of little-endian fields.
class PayloadWriter {
public:
    PayloadWriter& u8(std::uint8_t value);
    PayloadWriter& u32(std::uint32_t value);
    PayloadWriter& u64(std::uint64_t value);
    PayloadWriter& i32(std::int32_t value);
    PayloadWriter& bytes(std::string_view value);
    std::string take() noexcept;

private:
    std::string text_;
};

// Reads the fields of a payload in order; a field that runs past the end is refused (false) and
// leaves its output as it was.
class PayloadReader {
public:
    explicit PayloadReader(std::string_view payload) noexcept : rest_(payload) {}
    bool u8(std::uint8_t& value) noexcept;
    bool u32(std::uint32_t& value) noexcept;
    bool u64(std::uint64_t& value) noexcept;
    bool i32(std::int32_t& value) noexcept;
    // The next `size` bytes.
    bool bytes(std::size_t size, std::string_view& value) noexcept;
    // What is left, all of it.
    std::string_view rest() noexcept;

private:
    bool take(std::size_t size, std::string_view& field) noexcept;
    std::string_view rest_;
};

// A connected pair of channel ends (a POSIX socket pair, AF_UNIX and SOCK_STREAM): each
// close-on-exec, each set never to raise SIGPIPE where the system has SO_NOSIGPIPE. The caller owns
// both descriptors (Channel). Throws praenora::Error(MODULE_HOST_UNAVAILABLE, details.reason
// "spawn") with the system's text when the pair cannot be made, and always on Windows.
std::pair<int, int> open_channel_pair();

// Sets `descriptor`, a socket, never to raise SIGPIPE on writes, where the system has
// SO_NOSIGPIPE (macOS); elsewhere every write passes MSG_NOSIGNAL. Best effort.
void protect_from_sigpipe(int descriptor) noexcept;

// One end of a channel; owns its descriptor and closes it when destroyed. One thread reads at a
// time; writes are serialized, so frames never interleave. Never raises SIGPIPE, and never changes
// a process-wide signal disposition.
class Channel {
public:
    // Takes `descriptor` (a connected stream socket) over.
    explicit Channel(int descriptor) noexcept;
    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    int descriptor() const noexcept;

    // frame: a whole frame was read. end: the stream ended between frames, or this end was shut
    // down. broken: the stream ended within a frame, a read failed, or a header was refused
    // (`problem` says why; nothing more is read after it). timed_out: the deadline passed first.
    enum class ReadStatus { frame, end, broken, timed_out };
    struct ReadResult {
        ReadStatus status = ReadStatus::end;
        Frame frame;
        std::string problem;
    };

    // Reads one frame, blocking until it is whole or, with a deadline, until the deadline. After
    // a timeout within a frame the stream cannot be read further.
    ReadResult read(std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);

    // Waits until something can be read (a byte, the end of the stream or an error, which the
    // next read() reports) or until `deadline`; false when the deadline came first. Reads nothing,
    // so the stream stays whole.
    bool wait_readable(std::chrono::steady_clock::time_point deadline);

    // How a write ended. written: the whole frame. refused: over the kind's limit, nothing
    // written. failed: the peer is gone, the write failed, or the channel is broken (below).
    // timed_out: its deadline came first. stopped: it was told to stop (write_parts_while()). A
    // write that times out or stops after it wrote part of its frame leaves the channel broken:
    // the stream can no longer be read as frames, and every later write fails at once. One that
    // wrote nothing leaves the channel as it was, and the same frame may be written again.
    enum class WriteStatus { written, refused, failed, timed_out, stopped };

    // How long a write in slices (write_parts_while()) waits before it asks whether to go on.
    static constexpr std::chrono::milliseconds kWriteSlice{5};

    // Writes `frame` whole, waiting as long as that takes. False, with the reason in `problem`,
    // when the payload exceeds the kind's limit, the peer is gone, the write fails or the channel
    // is broken. Thread-safe.
    bool write(const Frame& frame, std::string* problem = nullptr);

    // Writes a frame of `kind` and `id` whose payload is `parts` (at most 8), one after the
    // other, without copying them into one string first; as write() otherwise.
    bool write_parts(FrameKind kind, std::uint64_t id,
                     std::initializer_list<std::string_view> parts, std::string* problem = nullptr);

    // As write_parts(), but waits, for the writer (another thread's write) and for room in the
    // stream, no later than `deadline`: a hard bound, also against a peer that keeps taking a few
    // bytes at a time (it returns within a few milliseconds of the deadline; a deadline at the
    // clock's last instant, as core/deadline.hpp makes for a wait without limit, is none).
    // Thread-safe; any thread may call it while another reads.
    WriteStatus write_parts_until(FrameKind kind, std::uint64_t id,
                                  std::initializer_list<std::string_view> parts,
                                  std::chrono::steady_clock::time_point deadline,
                                  std::string* problem = nullptr);

    // As write_parts(), but waits in slices of kWriteSlice: after each slice in which the frame was
    // not finished, however many bytes the peer took in it, `go_on` says whether to go on (false:
    // stopped); and, with a `deadline`, no later than it. Thread-safe; any thread may call it
    // while another reads.
    WriteStatus write_parts_while(FrameKind kind, std::uint64_t id,
                                  std::initializer_list<std::string_view> parts,
                                  const std::function<bool()>& go_on,
                                  std::optional<std::chrono::steady_clock::time_point> deadline,
                                  std::string* problem = nullptr);

    // Whether a write ended within its frame (see WriteStatus): nothing more can be written.
    bool broken() const noexcept;

    // From now on read() refuses a frame of any of `kinds` at its header, before any of its
    // payload is read, as it refuses a header over its kind's limit (broken), with the problem
    // "a <kind> frame " followed by `why`: the runtime's end of the channel of a module host whose
    // step does not share compiled models refuses the store's frames so. Call it before the first
    // read(), on the thread that starts the reader.
    void refuse_kinds(std::initializer_list<FrameKind> kinds, std::string why);

    // Ends both directions: a read blocked on this end returns end, and the peer reads the end of
    // the stream once it has read what was sent. The descriptor stays open until destruction.
    // Thread-safe.
    void shut_down() noexcept;

private:
    int descriptor_;
    // Every write: without a deadline and `go_on` it waits as long as it takes. With either, it
    // keeps its bounds itself: it waits for room with poll() until the end of its slice or its
    // deadline, then sends without waiting: on macOS at most send_chunk() bytes, which the stream
    // has room for; elsewhere with MSG_DONTWAIT, so the send takes what fits and returns. The
    // socket's send timeout (SO_SNDTIMEO, a send option only), set under the writer's lock to one
    // slice (and back to none for an unbounded write), is a second bound. The descriptor stays
    // blocking, and a read on it is not disturbed.
    WriteStatus send(FrameKind kind, std::uint64_t id,
                     std::initializer_list<std::string_view> parts,
                     std::optional<std::chrono::steady_clock::time_point> deadline,
                     const std::function<bool()>* go_on, std::string* problem);
    // Sets the send timeout to `wait` (0: none), unless it is that already (writer's lock held).
    bool set_send_timeout(std::chrono::nanoseconds wait, std::string* problem);
    // The most a bounded write sends at once once poll() reports room (writer's lock held): on
    // macOS the socket's send low-water mark (SO_SNDLOWAT), which poll() promises; 0 (no limit)
    // elsewhere, where MSG_DONTWAIT keeps a send from waiting.
    std::size_t send_chunk() noexcept;

    std::timed_mutex write_mutex_;
    std::chrono::nanoseconds send_timeout_{0}; // guarded by write_mutex_
#if defined(__APPLE__)
    std::size_t send_chunk_ = 0; // guarded by write_mutex_; 0 until known
#endif
    std::atomic<bool> broken_{false};
    // The kinds read() refuses at their header (bit `kind`), and why; set before the first read.
    std::uint64_t refused_kinds_ = 0;
    std::string refused_why_;
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_ISOLATION_CHANNEL_HPP
