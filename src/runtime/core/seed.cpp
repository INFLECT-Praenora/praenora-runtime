#include "core/seed.hpp"

#include "core/sha256.hpp"

namespace praenora::detail {

std::uint64_t derive_step_seed(std::uint64_t run_seed, std::string_view step_id) {
    static constexpr std::string_view kDomain = "praenora-seed-v1";
    std::uint8_t seed_bytes[8];
    for (int i = 0; i < 8; ++i) {
        seed_bytes[i] = static_cast<std::uint8_t>(run_seed >> (56 - 8 * i));
    }
    Sha256 hash;
    hash.update(kDomain.data(), kDomain.size());
    hash.update(seed_bytes, sizeof seed_bytes);
    hash.update(step_id.data(), step_id.size());
    const auto digest = hash.finish_digest();
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value = (value << 8) | digest[i];
    }
    return value & kMaxJsonSafeInteger;
}

} // namespace praenora::detail
