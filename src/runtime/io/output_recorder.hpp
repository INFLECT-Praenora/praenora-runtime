// The recorded workflow outputs of a run: one file per recorded output, outputs/<output>.jsonl
// below the run's output directory, holding one envelope per line.
#ifndef PRAENORA_RUNTIME_IO_OUTPUT_RECORDER_HPP
#define PRAENORA_RUNTIME_IO_OUTPUT_RECORDER_HPP

#include "core/sha256.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace praenora::detail {

// Each line is canonical_json(envelope) followed by "\n", written in binary mode, so the files of
// two runs with the same fingerprint (on the same platform, with the same binaries) are
// byte-identical. Not thread-safe: one thread records.
class OutputRecorder {
public:
    // What one file holds once recording is over.
    struct FileInfo {
        std::string port;   // the workflow output
        std::string path;   // relative to the output directory, '/'-separated: outputs/<port>.jsonl
        std::string sha256; // of the file's bytes
        std::uint64_t messages = 0;
    };

    // Creates <dir>/outputs/ and, for each of `ports` (workflow output names), an empty
    // <port>.jsonl, replacing any earlier file. Throws praenora::Error(IO_ERROR) when a file cannot
    // be created, and INVALID_ARGUMENT for a port named twice or a name that is not a port name.
    OutputRecorder(std::filesystem::path dir, std::vector<std::string> ports);
    ~OutputRecorder();
    OutputRecorder(const OutputRecorder&) = delete;
    OutputRecorder& operator=(const OutputRecorder&) = delete;

    // Appends `envelope` to the file of `port`. Throws praenora::Error: INVALID_ARGUMENT for a port
    // not given to the constructor, INVALID_STATE after finish(), IO_ERROR when writing fails, and
    // DATA_INVALID when a string of the envelope is not valid UTF-8.
    void record(const std::string& port, const nlohmann::json& envelope);

    // Hands what was recorded to the operating system. Throws praenora::Error(IO_ERROR).
    void flush();

    // Closes the files and returns what each holds, sorted by port. Calling it again returns the
    // same. Throws praenora::Error(IO_ERROR) when a file cannot be completed.
    std::vector<FileInfo> finish();

private:
    struct File {
        std::filesystem::path path;
        std::ofstream stream;
        Sha256 digest;
        FileInfo info;
    };

    std::map<std::string, std::unique_ptr<File>> files_; // by port
    std::vector<FileInfo> finished_;
    bool done_ = false;
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_IO_OUTPUT_RECORDER_HPP
