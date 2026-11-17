// A module library that is loaded and checked: its v0 function table, its describe() identity,
// its digest and its gate. Shared (std::shared_ptr) by every instance of the module; the library
// is unloaded when the last owner lets go, unless it is pinned.
#ifndef PRAENORA_RUNTIME_LOADER_LOADED_MODULE_HPP
#define PRAENORA_RUNTIME_LOADER_LOADED_MODULE_HPP

#include "discovery/manifest.hpp"
#include "loader/library_gate.hpp"
#include "loader/shared_library.hpp"

#include <praenora/sdk/abi.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace praenora::detail {

class LoadedModule {
public:
    // The checks of load(), in the order it makes them.
    enum class Check {
        library_file,       // 1: the file exists, and its SHA-256 matches a pinned one
        library_open,       // 2: the library loads
        entry_point,        // 3: it exports praenora_module_get_api
        table,              // 4-6: the table and its header
        required_functions, // 7: no required function is NULL
        describe,           // 8: describe() writes a valid pmp-describe document
        identity,           // 9: describe() agrees with the manifest
    };

    // Loads the manifest's library and checks it, in this order; each failure throws
    // praenora::Error located in the manifest:
    //   1. check_library_file() (below): the library file exists (LIBRARY_NOT_FOUND); its
    //      SHA-256 is computed and, when the manifest pins library.sha256, must match before any
    //      of its code runs (LIBRARY_DIGEST_MISMATCH);
    //   2. the library loads (LIBRARY_LOAD_FAILED, with the operating system's text), and no
    //      abandoned call that its calls would have to wait for may still be running in it
    //      (MODULE_UNRESPONSIVE at /library/path, naming that call; details
    //      {"abandoned_call": to_json(call)}): its gate (library_gate.hpp) is asked for
    //      call_access() before any of the module's code runs;
    //   3. it exports praenora_module_get_api (ENTRY_POINT_MISSING);
    //   4. praenora_module_get_api(0) returns a table (API_TABLE_INVALID);
    //   5. the table's header reports ABI major 0 (ABI_INCOMPATIBLE);
    //   6. header.reserved is 0 and header.struct_size covers the required functions
    //      (API_TABLE_INVALID);
    //   7. no required function is NULL (API_TABLE_INVALID, naming the functions);
    //   8. describe() succeeds, writes JSON and satisfies the pmp-describe schema, and its
    //      versions are usable semantic versions; and
    //   9. its name, version, ABI major and capabilities agree with the manifest
    //      (MODULE_IDENTITY_MISMATCH for 8 and 9, naming the fields that differ).
    // describe() runs on the calling thread. With PRAENORA_DEBUG_KEEP_LIBRARIES=1 in the
    // environment the library is pinned at once, so it is never unloaded.
    static std::shared_ptr<LoadedModule> load(const ModuleManifest& manifest);
    // load(), telling the caller how far it got: `*reached` is set to each check as load() starts
    // it, so when load() throws, it names the check that failed (for the contract checks).
    static std::shared_ptr<LoadedModule> load(const ModuleManifest& manifest, Check* reached);

    LoadedModule(const LoadedModule&) = delete;
    LoadedModule& operator=(const LoadedModule&) = delete;

    // A copy of the module's table, as large as this runtime's praenora_module_api_v0
    // (copy_module_table()): functions beyond the module's header.struct_size, or covered by it
    // only in part, are NULL, so optional functions can be tested for NULL without looking at
    // struct_size.
    const praenora_module_api_v0& api() const noexcept;
    const nlohmann::json& describe() const noexcept;
    const std::string& library_sha256() const noexcept; // always computed
    const ModuleManifest& manifest() const noexcept;

    // The gate of the library's image, shared by every LoadedModule of it in the process.
    LibraryGate& gate() const noexcept;
    // The lease a call into one of the module's instances takes: exclusive unless the manifest
    // declares independent instances (features.concurrency "instance"). self_test() always takes
    // an exclusive one.
    LibraryGate::Access call_access() const noexcept;

    // Keeps the library loaded for the rest of the process (an instance of it is unresponsive and
    // its code may still be running). Thread-safe.
    void pin() noexcept;
    bool pinned() const noexcept;

private:
    LoadedModule(ModuleManifest manifest, SharedLibrary library, std::shared_ptr<LibraryGate> gate,
                 const praenora_module_api_v0& api, nlohmann::json describe,
                 std::string library_sha256);

    ModuleManifest manifest_;
    SharedLibrary library_;
    // After library_, so it goes first: a gate is never left behind for an image that is unloaded
    // and may come back at the same address.
    std::shared_ptr<LibraryGate> gate_;
    praenora_module_api_v0 api_{};
    nlohmann::json describe_;
    std::string library_sha256_;
};

// Checks the manifest's library file without loading it, exactly as LoadedModule::load() starts:
// the file must exist (LIBRARY_NOT_FOUND) and be readable (IO_ERROR), both located at
// /library/path, and, when the manifest pins library.sha256, have that SHA-256
// (LIBRARY_DIGEST_MISMATCH, located at /library/sha256). Returns the file's SHA-256 (64 lowercase
// hex digits). Every error is a praenora::Error located in the manifest, with the messages of
// LoadedModule::load().
std::string check_library_file(const ModuleManifest& manifest);

// The copy of a module's function table that LoadedModule::api() returns: as much of the table as
// its header.struct_size covers, up to the size of this runtime's praenora_module_api_v0, in whole
// function pointers only, so a function that struct_size covers only in part reads as NULL, like
// one it does not cover at all. `header` starts a table whose struct_size is at least
// PRAENORA_MODULE_API_V0_BASE_SIZE (load() checks that first).
praenora_module_api_v0 copy_module_table(const praenora_api_header& header) noexcept;

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_LOADER_LOADED_MODULE_HPP
