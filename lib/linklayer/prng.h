/**
 * @file prng.h
 * @brief Tiny linear congruential generator (LCG) for *reproducible*,
 *        non-cryptographic pseudo-random bytes.
 *
 * The tests and the on-device speed test need a deterministic stream of
 * "random-looking" bytes — incompressible payloads and repeatable loss
 * patterns — without pulling in <random> (heavy, heap) or any per-platform
 * RNG. An LCG is the classic minimal answer: each step is
 * state = state * MULTIPLIER + INCREMENT (mod 2^32, via uint32 wrap). The two
 * parameter sets below are the well-known textbook ones, named here so the
 * "magic numbers" live at a single, explained source of truth: kLcgAnsiC (the
 * C standard's example rand(), a.k.a. glibc TYPE_0; used for pseudo-random
 * DATA bytes) and kLcgNumRecipes (Numerical Recipes' "ranqd1"; used for
 * deterministic loss / percentage decisions in the simulator). NOT FOR
 * CRYPTOGRAPHY — an LCG is trivially predictable; the link's security uses
 * Ascon-128 AEAD (see ll_aead.h / SECURITY.md). The only goal here is
 * determinism. NextByte() returns the HIGH bits because an LCG's low bits
 * cycle with a short period — the high bits are the usable ones.
 */
#ifndef LINK_LAYER_PRNG_H_
#define LINK_LAYER_PRNG_H_
#include <cstdint>

namespace link_layer {

/** @brief Multiplier (a) and increment (c): next = state * a + c. */
struct LcgParams {
    uint32_t mult;  ///< Multiplier (a).
    uint32_t inc;   ///< Increment (c).
};

/// C standard / glibc rand() example params — for pseudo-random data bytes.
static constexpr LcgParams kLcgAnsiC      = { 1103515245u, 12345u };
/// Numerical Recipes "ranqd1" params — for loss / percentage decisions.
static constexpr LcgParams kLcgNumRecipes = { 1664525u, 1013904223u };

/**
 * @brief A seeded LCG with deterministic, reproducible output.
 *
 * Same seed + same params => identical sequence on every run and platform
 * (the whole point: reproducible tests).
 */
class Lcg {
    uint32_t  state_;   ///< Current LCG state (the last output word).
    LcgParams params_;  ///< Multiplier/increment in use.
 public:
    /**
     * @brief Construct a seeded LCG.
     * @param[in] seed   initial state.
     * @param[in] params the LCG multiplier/increment to use.
     */
    explicit Lcg(uint32_t seed, LcgParams params = kLcgAnsiC)
        : state_(seed), params_(params) {}
    /**
     * @brief Restart the sequence from a new seed.
     * @param[in] s the new initial state.
     */
    void Seed(uint32_t s) { state_ = s; }

    /**
     * @brief Advance one step and return the raw 32-bit state word.
     * @return the new state (state * multiplier + increment, mod 2^32).
     */
    uint32_t Next() {
        state_ = state_ * params_.mult + params_.inc;
        return state_;
    }

    /**
     * @brief Advance one step and return a byte from the usable high bits.
     *
     * An LCG's low bits cycle with a short period, so we take the high bits.
     *
     * @return one pseudo-random byte.
     */
    uint8_t NextByte() { return (uint8_t)(Next() >> 16); }
};

}  // namespace link_layer

#endif  // LINK_LAYER_PRNG_H_
