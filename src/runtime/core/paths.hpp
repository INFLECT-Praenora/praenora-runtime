// Paths travel through the runtime as UTF-8 strings. These helpers convert them to and from
// std::filesystem::path (wide on Windows) and read and write files in binary mode.
#ifndef PRAENORA_RUNTIME_CORE_PATHS_HPP
#define PRAENORA_RUNTIME_CORE_PATHS_HPP

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace praenora::detail {

std::filesystem::path path_from_utf8(std::string_view utf8);
std::string path_to_utf8(const std::filesystem::path& path);

// `utf8` as is when absolute, otherwise relative to `base_dir`; lexically normalized either way.
std::filesystem::path resolve_against(const std::filesystem::path& base_dir, std::string_view utf8);

// Splits a search-path list such as PRAENORA_MODULE_PATH: ';'-separated on Windows,
// ':'-separated elsewhere. Empty entries are dropped.
std::vector<std::string> split_path_list(std::string_view list);

// The file's bytes, unchanged. Throws praenora::Error(IO_ERROR) if it cannot be read.
std::string read_file_bytes(const std::filesystem::path& file);

// Writes `data` to `<file>.tmp-<pid>-<n>`, then renames it over `file`, so readers see either the
// old or the new content, never a partial file. Creates missing parent directories. Throws
// praenora::Error(IO_ERROR) on failure and leaves no temporary file behind. The data is not
// flushed to the storage device (no fsync): atomic for readers, not durable across a crash of the
// machine.
void write_file_atomically(const std::filesystem::path& file, std::string_view data);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_CORE_PATHS_HPP
