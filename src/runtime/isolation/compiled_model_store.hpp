// The store of compiled models that the module hosts of steps sharing them hand to each other
// (docs/specs/process-isolation.md#shared-compiled-models): in the runtime's process, in memory
// only, never on disk. A provider in a module host saves a model it compiled under a key of its
// own, and a provider of the same library in another module host loads it from there instead of
// compiling it again. The runtime's side of each channel answers its module host's requests
// (isolation/child_module.hpp); nothing else reads or writes the store.
//
// Entries are kept by namespace (the provider's name and its library's SHA-256, which the runtime
// takes from its own load request for the module host, never from what the module host says) and
// key (at most kMaxCompiledModelKeyBytes). An entry takes at most kMaxCompiledModelBytes; the
// keys and entries kept take at most kMaxCompiledModelsTotalBytes together, and a save that would
// go beyond it first evicts the least recently used entries (a find or a save is a use).
#ifndef PRAENORA_RUNTIME_ISOLATION_COMPILED_MODEL_STORE_HPP
#define PRAENORA_RUNTIME_ISOLATION_COMPILED_MODEL_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>

namespace praenora::detail {

inline constexpr std::size_t kMaxCompiledModelKeyBytes = 256;
inline constexpr std::size_t kMaxCompiledModelBytes = std::size_t{128} << 20;
inline constexpr std::size_t kMaxCompiledModelsTotalBytes = std::size_t{512} << 20;

// Whose entries: one provider library's.
struct CompiledModelNamespace {
    std::string provider;       // the provider's name
    std::string library_sha256; // its library's SHA-256, as validation computed it
};

class CompiledModelStore {
public:
    struct Limits {
        std::size_t key = kMaxCompiledModelKeyBytes;
        std::size_t entry = kMaxCompiledModelBytes;
        std::size_t total = kMaxCompiledModelsTotalBytes; // keys and entries together
    };

    // saved: kept (an entry under the key before is replaced); key_too_long: the key is over the
    // key limit; too_large: the entry is over the entry limit, or its key and it are over the
    // total. A refused entry changes nothing.
    enum class Saved { saved, key_too_long, too_large };
    struct SaveResult {
        Saved status = Saved::saved;
        std::size_t evicted = 0; // entries evicted to make room
    };

    CompiledModelStore() : CompiledModelStore(Limits{}) {}
    explicit CompiledModelStore(Limits limits) : limits_(limits) {}
    CompiledModelStore(const CompiledModelStore&) = delete;
    CompiledModelStore& operator=(const CompiledModelStore&) = delete;

    const Limits& limits() const noexcept { return limits_; }

    // The entry under `key` in `space`, or nullptr; a hit makes it the most recently used. The
    // entry stays valid for its holder when the store replaces or evicts it. Thread-safe.
    std::shared_ptr<const std::string> find(const CompiledModelNamespace& space,
                                            std::string_view key);

    // Keeps `entry` under `key` in `space`, as the most recently used, evicting the least recently
    // used entries first until it fits within the total. Thread-safe. Strongly exception-safe: when
    // it throws (std::bad_alloc), the store is as it was.
    SaveResult save(const CompiledModelNamespace& space, std::string_view key, std::string entry);

    // The number of entries, and the bytes they take with their keys. Thread-safe.
    std::size_t size() const;
    std::size_t total_bytes() const;

    // Forgets every entry. Thread-safe.
    void clear();

private:
    using Key = std::tuple<std::string, std::string, std::string>; // provider, library, key

    struct Entry {
        Key key;
        std::shared_ptr<const std::string> data;
        std::size_t bytes = 0; // the key's and the data's
    };

    Limits limits_;
    mutable std::mutex mutex_;
    std::list<Entry> entries_; // the most recently used first
    std::map<Key, std::list<Entry>::iterator> index_;
    std::size_t total_ = 0;
};

// The runtime's process-wide store, with the limits above. Never destroyed: module hosts' readers
// may use it until the process ends.
CompiledModelStore& compiled_model_store();

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_ISOLATION_COMPILED_MODEL_STORE_HPP
