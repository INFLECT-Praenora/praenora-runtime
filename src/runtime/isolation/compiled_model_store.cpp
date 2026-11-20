#include "isolation/compiled_model_store.hpp"

#include <iterator>
#include <utility>

namespace praenora::detail {

std::shared_ptr<const std::string> CompiledModelStore::find(const CompiledModelNamespace& space,
                                                            std::string_view key) {
    if (key.size() > limits_.key) {
        return nullptr;
    }
    const Key wanted(space.provider, space.library_sha256, std::string(key));
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = index_.find(wanted);
    if (found == index_.end()) {
        return nullptr;
    }
    entries_.splice(entries_.begin(), entries_, found->second); // now the most recently used
    return found->second->data;
}

CompiledModelStore::SaveResult CompiledModelStore::save(const CompiledModelNamespace& space,
                                                        std::string_view key, std::string entry) {
    SaveResult result;
    if (key.size() > limits_.key) {
        result.status = Saved::key_too_long;
        return result;
    }
    const std::size_t bytes = key.size() + entry.size();
    if (entry.size() > limits_.entry || bytes > limits_.total) {
        result.status = Saved::too_large;
        return result;
    }
    // Everything that allocates comes first: when it throws (no memory), nothing has changed.
    Key kept(space.provider, space.library_sha256, std::string(key));
    std::list<Entry> fresh;
    fresh.push_back(Entry{kept, std::make_shared<const std::string>(std::move(entry)), bytes});
    // What is evicted or replaced is freed outside the lock: a large entry takes a while to free.
    std::list<Entry> released;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        auto indexed = index_.find(kept);
        if (indexed == index_.end()) {
            // The index's new node is the last allocation; if it throws, nothing has changed.
            indexed = index_.emplace(std::move(kept), fresh.begin()).first;
        } else {
            total_ -= indexed->second->bytes;
            released.splice(released.end(), entries_, indexed->second);
            indexed->second = fresh.begin();
        }
        // Nothing throws from here on: the list's nodes move, the index loses nodes.
        while (total_ + bytes > limits_.total && !entries_.empty()) {
            const auto oldest = std::prev(entries_.end());
            total_ -= oldest->bytes;
            index_.erase(oldest->key);
            released.splice(released.end(), entries_, oldest);
            ++result.evicted;
        }
        entries_.splice(entries_.begin(), fresh); // `indexed` still names the moved node
        total_ += bytes;
    }
    return result;
}

std::size_t CompiledModelStore::size() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

std::size_t CompiledModelStore::total_bytes() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return total_;
}

void CompiledModelStore::clear() {
    std::list<Entry> released;
    const std::lock_guard<std::mutex> lock(mutex_);
    released.swap(entries_);
    index_.clear();
    total_ = 0;
}

CompiledModelStore& compiled_model_store() {
    static auto* const store = new CompiledModelStore(); // never destroyed: used until exit
    return *store;
}

} // namespace praenora::detail
