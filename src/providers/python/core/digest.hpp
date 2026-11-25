// The directory digest of Python sources (docs/specs/python-service.md#the-directory-digest), as
// the core computes it from the files it finds; cmake/PraenoraPythonDigest.cmake computes the same
// value when a module or provider is built. No Python: the core's unit tests compile this file.
#ifndef PRAENORA_PROVIDERS_PYTHON_CORE_DIGEST_HPP
#define PRAENORA_PROVIDERS_PYTHON_CORE_DIGEST_HPP

#include <praenora/sdk/status.h>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace praenora::detail::python {

// A directory whose digest cannot be computed. code() is IO_ERROR when the directory does not
// exist or a file or directory below it cannot be read, and DATA_INVALID when it holds what the
// digest does not cover: a symbolic link (outside a __pycache__ directory, as the CMake function
// refuses it) or a file that is not a regular one. what() names the path.
class DigestError : public std::runtime_error {
public:
    DigestError(praenora_status code, const std::string& message)
        : std::runtime_error(message), code_(code) {}
    praenora_status code() const noexcept { return code_; }

private:
    praenora_status code_;
};

// 64 lowercase hexadecimal digits: the SHA-256 of the lines "<SHA-256 of the file>  <path>\n" of
// every regular file below `root` but those below a directory named __pycache__, each path
// relative to `root` with / separators, in UTF-8, the lines sorted by the bytes of the path.
// Throws DigestError.
std::string directory_digest(const std::filesystem::path& root);

} // namespace praenora::detail::python

#endif // PRAENORA_PROVIDERS_PYTHON_CORE_DIGEST_HPP
