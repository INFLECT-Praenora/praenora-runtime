// Per-step seeds derived from the run seed.
#ifndef PRAENORA_RUNTIME_CORE_SEED_HPP
#define PRAENORA_RUNTIME_CORE_SEED_HPP

#include <cstdint>
#include <string_view>

namespace praenora::detail {

// 2^53 - 1: the largest integer that every JSON implementation represents exactly. Seeds lie in
// [0, kMaxJsonSafeInteger].
inline constexpr std::uint64_t kMaxJsonSafeInteger = 9007199254740991ULL;

// The first 8 bytes (big-endian) of
// SHA-256("praenora-seed-v1" || u64be(run_seed) || utf8(step_id)), masked to 2^53 - 1.
std::uint64_t derive_step_seed(std::uint64_t run_seed, std::string_view step_id);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_CORE_SEED_HPP
