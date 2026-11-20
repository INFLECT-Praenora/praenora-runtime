// The store of shared compiled models (src/runtime/isolation/compiled_model_store.hpp): entries
// found again under their provider's namespace and key, kept least recently used first out within
// the total, and refused over the key and entry limits. Small limits stand for the real ones,
// which the first test checks; the process-wide store is never written here.
#include "isolation/compiled_model_store.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using praenora::detail::CompiledModelNamespace;
using praenora::detail::CompiledModelStore;

namespace {

const CompiledModelNamespace kSpace{"test-simulation", std::string(64, 'a')};

// A store whose keys take at most 4 bytes, entries 10 and all of them together 30, keys included.
CompiledModelStore small_store() {
    CompiledModelStore::Limits limits;
    limits.key = 4;
    limits.entry = 10;
    limits.total = 30;
    return CompiledModelStore(limits);
}

// The entry under `key`, or "(none)".
std::string found(CompiledModelStore& store, const std::string& key,
                  const CompiledModelNamespace& space = kSpace) {
    const std::shared_ptr<const std::string> entry = store.find(space, key);
    return entry != nullptr ? *entry : std::string("(none)");
}

} // namespace

TEST(CompiledModelStore, HasTheLimitsOfTheSpecification) {
    EXPECT_EQ(praenora::detail::kMaxCompiledModelKeyBytes, 256u);
    EXPECT_EQ(praenora::detail::kMaxCompiledModelBytes, 134217728u);
    EXPECT_EQ(praenora::detail::kMaxCompiledModelsTotalBytes, 536870912u);
    const CompiledModelStore::Limits& limits = praenora::detail::compiled_model_store().limits();
    EXPECT_EQ(limits.key, 256u);
    EXPECT_EQ(limits.entry, 134217728u);
    EXPECT_EQ(limits.total, 536870912u);
}

TEST(CompiledModelStore, FindsWhatWasSavedAndNothingElse) {
    CompiledModelStore store = small_store();
    EXPECT_EQ(found(store, "k"), "(none)");
    const CompiledModelStore::SaveResult saved = store.save(kSpace, "k", "model");
    EXPECT_EQ(saved.status, CompiledModelStore::Saved::saved);
    EXPECT_EQ(saved.evicted, 0u);
    EXPECT_EQ(found(store, "k"), "model");
    EXPECT_EQ(found(store, "j"), "(none)");
    EXPECT_EQ(store.size(), 1u);
    EXPECT_EQ(store.total_bytes(), 1u + 5u); // the key and the entry
    // An empty entry is an entry.
    EXPECT_EQ(store.save(kSpace, "e", "").status, CompiledModelStore::Saved::saved);
    EXPECT_EQ(found(store, "e"), "");
}

TEST(CompiledModelStore, ASaveUnderAKeyReplacesItsEntry) {
    CompiledModelStore store = small_store();
    store.save(kSpace, "k", "first");
    store.save(kSpace, "k", "second");
    EXPECT_EQ(found(store, "k"), "second");
    EXPECT_EQ(store.size(), 1u);
    EXPECT_EQ(store.total_bytes(), 1u + 6u);
}

TEST(CompiledModelStore, AnEntryFoundStaysValidAfterItIsReplacedOrEvicted) {
    CompiledModelStore store = small_store();
    store.save(kSpace, "k", "first");
    const std::shared_ptr<const std::string> entry = store.find(kSpace, "k");
    store.save(kSpace, "k", "second");
    store.clear();
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(*entry, "first");
    EXPECT_EQ(store.size(), 0u);
    EXPECT_EQ(store.total_bytes(), 0u);
}

// Within the total of 30 bytes: three entries of 1 + 9 bytes fit, a fourth evicts the least
// recently used, and a hit counts as a use.
TEST(CompiledModelStore, EvictsTheLeastRecentlyUsedFirst) {
    CompiledModelStore store = small_store();
    store.save(kSpace, "a", "aaaaaaaaa");
    store.save(kSpace, "b", "bbbbbbbbb");
    store.save(kSpace, "c", "ccccccccc");
    EXPECT_EQ(store.total_bytes(), 30u);
    EXPECT_EQ(found(store, "a"), "aaaaaaaaa"); // a is now used after b and c
    const CompiledModelStore::SaveResult saved = store.save(kSpace, "d", "ddddddddd");
    EXPECT_EQ(saved.status, CompiledModelStore::Saved::saved);
    EXPECT_EQ(saved.evicted, 1u);
    EXPECT_EQ(found(store, "b"), "(none)");
    EXPECT_EQ(found(store, "c"), "ccccccccc");
    EXPECT_EQ(found(store, "a"), "aaaaaaaaa");
    EXPECT_EQ(found(store, "d"), "ddddddddd");
    // A replaced entry counts as used too: c, now the least recently used, goes next.
    store.save(kSpace, "a", "AAAAAAAAA");
    store.save(kSpace, "e", "eeeeeeeee");
    EXPECT_EQ(found(store, "c"), "(none)");
    EXPECT_EQ(found(store, "d"), "ddddddddd");
    EXPECT_EQ(found(store, "a"), "AAAAAAAAA");
    EXPECT_EQ(found(store, "e"), "eeeeeeeee");
    EXPECT_LE(store.total_bytes(), 30u);
}

// An entry that fits alone but not beside the others evicts them, oldest first, until it fits.
TEST(CompiledModelStore, AnEntryThatFitsAloneEvictsUntilItFits) {
    CompiledModelStore::Limits limits;
    limits.key = 4;
    limits.entry = 25;
    limits.total = 30;
    CompiledModelStore store(limits);
    store.save(kSpace, "a", "aaaaaaaaa");
    store.save(kSpace, "b", "bbbbbbbbb");
    store.save(kSpace, "c", "c");
    // 3 + 20 bytes: a and b go (22 + 23 > 30, then 12 + 23 > 30), c stays (2 + 23).
    const CompiledModelStore::SaveResult saved = store.save(kSpace, "big", std::string(20, 'x'));
    EXPECT_EQ(saved.status, CompiledModelStore::Saved::saved);
    EXPECT_EQ(saved.evicted, 2u);
    EXPECT_EQ(found(store, "a"), "(none)");
    EXPECT_EQ(found(store, "b"), "(none)");
    EXPECT_EQ(found(store, "c"), "c");
    EXPECT_EQ(found(store, "big"), std::string(20, 'x'));
    EXPECT_EQ(store.total_bytes(), 2u + 23u);
}

TEST(CompiledModelStore, RefusesAnEntryOverTheEntryLimit) {
    CompiledModelStore store = small_store();
    store.save(kSpace, "a", "aaaaaaaaa");
    EXPECT_EQ(store.save(kSpace, "k", std::string(10, 'x')).status,
              CompiledModelStore::Saved::saved);
    const CompiledModelStore::SaveResult refused = store.save(kSpace, "j", std::string(11, 'x'));
    EXPECT_EQ(refused.status, CompiledModelStore::Saved::too_large);
    EXPECT_EQ(refused.evicted, 0u);
    EXPECT_EQ(found(store, "j"), "(none)");
    // A refused entry does not replace the one under its key, and evicts nothing.
    EXPECT_EQ(store.save(kSpace, "k", std::string(11, 'y')).status,
              CompiledModelStore::Saved::too_large);
    EXPECT_EQ(found(store, "k"), std::string(10, 'x'));
    EXPECT_EQ(found(store, "a"), "aaaaaaaaa");
}

// With limits where the entry limit is above the total (not the real ones), an entry that cannot
// fit even alone is refused as too large, and nothing is evicted.
TEST(CompiledModelStore, RefusesAnEntryThatCannotFitTheTotal) {
    CompiledModelStore::Limits limits;
    limits.key = 4;
    limits.entry = 100;
    limits.total = 30;
    CompiledModelStore store(limits);
    store.save(kSpace, "a", "aaaaaaaaa");
    EXPECT_EQ(store.save(kSpace, "k", std::string(30, 'x')).status,
              CompiledModelStore::Saved::too_large);
    EXPECT_EQ(found(store, "a"), "aaaaaaaaa");
    EXPECT_EQ(store.save(kSpace, "k", std::string(29, 'x')).status,
              CompiledModelStore::Saved::saved);
}

TEST(CompiledModelStore, RefusesAKeyOverTheKeyLimit) {
    CompiledModelStore store = small_store();
    EXPECT_EQ(store.save(kSpace, "four", "x").status, CompiledModelStore::Saved::saved);
    EXPECT_EQ(store.save(kSpace, "fives", "x").status, CompiledModelStore::Saved::key_too_long);
    EXPECT_EQ(found(store, "four"), "x");
    EXPECT_EQ(found(store, "fives"), "(none)");
    EXPECT_EQ(store.size(), 1u);
}

// A provider's entries are its own: the same key under another provider, or under another library
// of the same provider, is another entry.
TEST(CompiledModelStore, KeepsTheNamespacesOfProvidersApart) {
    CompiledModelStore store = small_store();
    const CompiledModelNamespace other_library{"test-simulation", std::string(64, 'b')};
    const CompiledModelNamespace other_provider{"roadrunner", std::string(64, 'a')};
    store.save(kSpace, "k", "mine");
    EXPECT_EQ(found(store, "k", other_library), "(none)");
    EXPECT_EQ(found(store, "k", other_provider), "(none)");
    store.save(other_library, "k", "library");
    store.save(other_provider, "k", "provider");
    EXPECT_EQ(found(store, "k"), "mine");
    EXPECT_EQ(found(store, "k", other_library), "library");
    EXPECT_EQ(found(store, "k", other_provider), "provider");
    EXPECT_EQ(store.size(), 3u);
}

// Module hosts' readers use the store at once from their threads: saves, finds and evictions keep
// the total within its limit.
TEST(CompiledModelStore, IsSafeToUseFromManyThreads) {
    CompiledModelStore::Limits limits;
    limits.key = 8;
    limits.entry = 64;
    limits.total = 1000;
    CompiledModelStore store(limits);
    std::atomic<bool> over{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&store, &over, t] {
            for (int i = 0; i < 500; ++i) {
                const std::string key = std::to_string(t) + "-" + std::to_string(i % 37);
                if (i % 3 == 0) {
                    store.save(kSpace, key, std::string(static_cast<std::size_t>(i % 60), 'x'));
                } else {
                    static_cast<void>(store.find(kSpace, key));
                }
                if (store.total_bytes() > 1000) {
                    over = true;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_FALSE(over.load());
    EXPECT_LE(store.total_bytes(), 1000u);
}
