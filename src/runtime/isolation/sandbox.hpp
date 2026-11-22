// The sandbox profiles of a module host (docs/specs/process-isolation.md#the-restricted-profile):
// none, or restricted, a macOS sandbox profile that the module host applies to itself, for good,
// before it loads any provider or module library, so that their static initializers already run
// under it. The profile, written here, allows everything by default and denies network access
// (network*), new processes (process-fork, process-exec) and file writes (file-write*) other than
// below the module host's temporary directory and to /dev/null. Reads, dlopen(), MAP_JIT memory
// and memory made executable with mprotect() (a JIT's code), and the descriptors the module host
// was given (its channel, standard output and error) keep working.
//
// The module host's temporary directory is a private one (PrivateTemporaryDirectory): the runtime
// makes it, for that one module host, below the real path of the system's temporary directory,
// and removes it once the module host has ended (a runtime that is killed leaves it); the module
// host's TMPDIR names it, and with it std::filesystem::temp_directory_path().
//
// sandbox_init() is deprecated: it is called in one function, with the deprecation warning
// silenced there. A process that runs in a sandbox already cannot apply another one: macOS refuses
// it (EPERM).
#ifndef PRAENORA_RUNTIME_ISOLATION_SANDBOX_HPP
#define PRAENORA_RUNTIME_ISOLATION_SANDBOX_HPP

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace praenora::detail {

enum class SandboxProfile { none, restricted };

// "none" or "restricted", as the configuration and the run record name them.
std::string_view sandbox_profile_name(SandboxProfile profile) noexcept;
// The profile `name` names, exactly as written; nullopt for anything else.
std::optional<SandboxProfile> sandbox_profile_from_name(std::string_view name) noexcept;

// Whether this platform has the restricted profile: macOS; not Linux or Windows in this version.
bool restricted_sandbox_available() noexcept;

// The restricted profile, in the system's sandbox profile language: writes are allowed below
// `temporary_directory` (UTF-8, a real path with symlinks resolved) and to /dev/null only.
std::string restricted_profile_text(std::string_view temporary_directory);

// Applies the restricted profile to this process, for good. nullopt when it is applied; otherwise
// the system's text ("Operation not permitted" in a process that runs in a sandbox already), and
// off macOS that the profile is not available.
std::optional<std::string> apply_restricted_profile(std::string_view temporary_directory);

// A private temporary directory for one module host: praenora-module-host-XXXXXX, made by
// mkdtemp() (readable and writable by its owner only) below the real path of the system's
// temporary directory (std::filesystem::temp_directory_path(), symlinks resolved). Removed, with
// everything in it, by remove() or when destroyed; what cannot be removed (a file a module made
// immutable, say) is left, and remove() says so. A process that is killed removes nothing: its
// directory stays until the system cleans its temporary directory.
class PrivateTemporaryDirectory {
public:
    // Makes one; nullopt with the system's text in `problem` when it cannot be made (and always on
    // Windows).
    static std::optional<PrivateTemporaryDirectory> make(std::string& problem);

    PrivateTemporaryDirectory(PrivateTemporaryDirectory&& other) noexcept;
    PrivateTemporaryDirectory& operator=(PrivateTemporaryDirectory&& other) noexcept;
    PrivateTemporaryDirectory(const PrivateTemporaryDirectory&) = delete;
    PrivateTemporaryDirectory& operator=(const PrivateTemporaryDirectory&) = delete;
    ~PrivateTemporaryDirectory();

    // Its real path, UTF-8; empty once moved from or removed.
    const std::string& path() const noexcept;

    // Removes it with everything in it, now. First it gives its owner read, write and search
    // rights again on every directory below it, without following a symbolic link, so that modes
    // a module set in it cannot keep it. nullopt when it is gone (or was moved from or removed
    // before); otherwise what is left and why ("<path>: <the system's text>"), and it stays. The
    // destructor does nothing more once it has been called.
    std::optional<std::string> remove() noexcept;

private:
    explicit PrivateTemporaryDirectory(std::string path) noexcept;
    // Adds read, write and search rights for the owner to `directory` and to every directory below
    // it, top down, unless it is a symbolic link; never follows one. Problems are ignored: the
    // removal that follows says what is left.
    static void give_owner_rights(const std::filesystem::path& directory);

    std::string path_; // empty once moved from
};

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_ISOLATION_SANDBOX_HPP
