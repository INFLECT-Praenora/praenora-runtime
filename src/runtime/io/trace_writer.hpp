// The message trace of a run (trace/messages.jsonl below the output directory, written when
// outputs.trace_messages is on): every envelope that goes into or comes out of a step, one per
// line.
#ifndef PRAENORA_RUNTIME_IO_TRACE_WRITER_HPP
#define PRAENORA_RUNTIME_IO_TRACE_WRITER_HPP

#include "core/sha256.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace praenora::detail {

// Each line is canonical_json({"tick", "step", "direction", "port", "envelope"[, "diagnostics"]})
// followed by "\n", written in binary mode: byte-deterministic like the recorded outputs. A
// result without outputs but with diagnostics gets a record of its own, with port and envelope
// null. Not thread-safe: one thread writes.
class TraceWriter {
public:
    // What the file holds once the trace is finished.
    struct Summary {
        std::string sha256;         // of the file's bytes
        std::uint64_t messages = 0; // the envelopes (records with diagnostics only not counted)
    };

    // Creates `file`, and its parent directories, replacing any earlier file. Throws
    // praenora::Error(IO_ERROR).
    explicit TraceWriter(std::filesystem::path file);
    TraceWriter(const TraceWriter&) = delete;
    TraceWriter& operator=(const TraceWriter&) = delete;

    // One line: `envelope` going "in" to step `step` through its input `port`, or coming "out" of
    // it through its output `port`, in tick `tick`. `diagnostics`, when given, is the diagnostics
    // object of the step's result. Throws praenora::Error: INVALID_STATE after finish(), IO_ERROR
    // when writing fails, DATA_INVALID for a string that is not valid UTF-8.
    void write(std::uint64_t tick, const std::string& step, const char* direction,
               const std::string& port, const nlohmann::json& envelope,
               const nlohmann::json* diagnostics);

    // The diagnostics object of a result of step `step` in tick `tick` that has no outputs:
    // {"tick", "step", "direction": "out", "port": null, "envelope": null, "diagnostics"}. Throws
    // like write().
    void write_diagnostics(std::uint64_t tick, const std::string& step,
                           const nlohmann::json& diagnostics);

    // Hands what was written to the operating system. Throws praenora::Error(IO_ERROR).
    void flush();

    // Closes the file and returns what it holds; calling it again returns the same. Throws
    // praenora::Error(IO_ERROR) when the file cannot be completed.
    Summary finish();

private:
    void write_line(const nlohmann::json& record);

    std::filesystem::path file_;
    std::ofstream stream_;
    Sha256 digest_;
    Summary summary_;
    bool done_ = false;
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_IO_TRACE_WRITER_HPP
