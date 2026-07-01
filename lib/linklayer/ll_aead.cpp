/**
 * @file ll_aead.cpp
 * @brief Implementation of the Ascon-128 v1.2 AEAD declared in ll_aead.h.
 *
 * The Ascon sponge permutation, AD absorption, and the encrypt / decrypt /
 * KDF drivers. See ll_aead.h for what Ascon is and why we use it; this file
 * is the math. Per-function contracts for the public API live with their
 * declarations in ll_aead.h.
 */
#include "ll_aead.h"

#include <string.h>

namespace link_layer {
namespace {

/**
 * @brief Rotate a 64-bit word right by `n` bits.
 * @param[in] x the value to rotate.
 * @param[in] n the rotation amount (0..63).
 * @return `x` rotated right by `n`.
 */
uint64_t AsconRotr(uint64_t x, int n) {
    return (x >> n) | (x << (64 - n));
}

/**
 * @brief Load `n` bytes big-endian into the TOP of a 64-bit word (rest 0).
 * @param[in] b source bytes.
 * @param[in] n number of bytes to load (0..8).
 * @return the assembled big-endian word.
 */
uint64_t AsconLoadBe(const uint8_t* b, size_t n) {  // top n bytes, rest 0
    uint8_t t[8] = {0};
    memcpy(t, b, n);
    uint64_t r = 0;
    for (int i = 0; i < 8; i++) r = (r << 8) | t[i];
    return r;
}

/**
 * @brief Store the TOP `n` bytes of a 64-bit word big-endian.
 * @param[out] b destination bytes.
 * @param[in]  n number of bytes to write (0..8).
 * @param[in]  x the word whose top `n` bytes are written.
 */
void AsconStoreBe(uint8_t* b, size_t n, uint64_t x) {  // write top n bytes
    uint8_t t[8];
    for (int i = 7; i >= 0; i--) { t[i] = (uint8_t)(x & 0xff); x >>= 8; }
    memcpy(b, t, n);
}

/** @brief The 320-bit Ascon sponge state, as five 64-bit words. */
struct AsconState {
    uint64_t x0;  ///< Rate word: data is absorbed/squeezed here (64-bit rate).
    uint64_t x1;  ///< Capacity word 1.
    uint64_t x2;  ///< Capacity word 2.
    uint64_t x3;  ///< Capacity word 3.
    uint64_t x4;  ///< Capacity word 4.
};

/**
 * @brief Ascon round constants.
 *
 * The 12 values added (one per round) in the S-box's constant-addition step,
 * for domain separation between rounds.
 */
const uint64_t ASCON_RC[12] = {
    0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87, 0x78, 0x69, 0x5a, 0x4b
};

/**
 * @brief One Ascon permutation round (constant add, S-box, linear diffusion).
 * @param[in,out] s the sponge state to transform in place.
 * @param[in]     C the round constant for this round.
 */
void AsconRound(AsconState& s, uint64_t C) {
    uint64_t x0 = s.x0, x1 = s.x1, x2 = s.x2, x3 = s.x3, x4 = s.x4;
    x2 ^= C;                                           // add round constant
    // substitution layer (bitsliced 5-bit S-box)
    x0 ^= x4; x4 ^= x3; x2 ^= x1;
    uint64_t t0 = x0, t1 = x1, t2 = x2, t3 = x3, t4 = x4;
    t0 = ~t0; t1 = ~t1; t2 = ~t2; t3 = ~t3; t4 = ~t4;
    t0 &= x1; t1 &= x2; t2 &= x3; t3 &= x4; t4 &= x0;
    x0 ^= t1; x1 ^= t2; x2 ^= t3; x3 ^= t4; x4 ^= t0;
    x1 ^= x0; x0 ^= x4; x3 ^= x2; x2 = ~x2;
    // linear diffusion layer
    x0 ^= AsconRotr(x0, 19) ^ AsconRotr(x0, 28);
    x1 ^= AsconRotr(x1, 61) ^ AsconRotr(x1, 39);
    x2 ^= AsconRotr(x2, 1)  ^ AsconRotr(x2, 6);
    x3 ^= AsconRotr(x3, 10) ^ AsconRotr(x3, 17);
    x4 ^= AsconRotr(x4, 7)  ^ AsconRotr(x4, 41);
    s.x0 = x0; s.x1 = x1; s.x2 = x2; s.x3 = x3; s.x4 = x4;
}
/**
 * @brief The 12-round Ascon permutation p^a (initialization / finalization).
 * @param[in,out] s the sponge state to permute in place.
 */
void AsconP12(AsconState& s) {
    for (int i = 0;  i < 12; i++) AsconRound(s, ASCON_RC[i]);
}

/**
 * @brief The 6-round Ascon permutation p^b (per data block).
 * @param[in,out] s the sponge state to permute in place.
 */
void AsconP6(AsconState& s) {
    for (int i = 6;  i < 12; i++) AsconRound(s, ASCON_RC[i]);
}

/**
 * @brief Initialize the sponge state from the key + nonce (Ascon-128 v1.2).
 *
 * Loads IV‖K‖N, runs the 12-round permutation, then XORs the key back into
 * the capacity.
 *
 * @param[out] s   the sponge state to initialize.
 * @param[in]  key the 16-byte key.
 * @param[in]  npub the 16-byte public nonce.
 * @param[out] K0  receives the first key word (reused at finalization).
 * @param[out] K1  receives the second key word (reused at finalization).
 */
void AsconInit(AsconState& s, const uint8_t key[16], const uint8_t npub[16],
               uint64_t& K0, uint64_t& K1) {
    K0 = AsconLoadBe(key, 8); K1 = AsconLoadBe(key + 8, 8);
    uint64_t N0 = AsconLoadBe(npub, 8), N1 = AsconLoadBe(npub + 8, 8);
    // x0 = the Ascon-128 IV: a domain-separation constant for the parameters
    // k|rate|a|b = 128|64|12|6, i.e. bytes 0x80 40 0c 06 then zeros (128-bit
    // key, 64-bit rate, 12 init rounds, 6 data rounds).
    s.x0 = 0x80400c0600000000ULL; s.x1 = K0; s.x2 = K1; s.x3 = N0; s.x4 = N1;
    AsconP12(s);
    s.x3 ^= K0; s.x4 ^= K1;
}
/**
 * @brief Absorb the associated data, then domain-separate AD from the message.
 *
 * Absorbs AD in 8-byte (rate) blocks with p6 between them, pads the final
 * block, then flips the domain-separation bit so AD and message can't be
 * confused.
 *
 * @param[in,out] s     the sponge state.
 * @param[in]     ad    associated-data bytes (authenticated, not encrypted).
 * @param[in]     adlen length of `ad` in bytes.
 */
void AsconAbsorbAd(AsconState& s, const uint8_t* ad, size_t adlen) {
    if (adlen > 0) {
        while (adlen >= 8) {
            s.x0 ^= AsconLoadBe(ad, 8); AsconP6(s); ad += 8; adlen -= 8;
        }
        uint8_t blk[8] = {0}; memcpy(blk, ad, adlen); blk[adlen] = 0x80;
        s.x0 ^= AsconLoadBe(blk, 8); AsconP6(s);
    }
    s.x4 ^= 1;   // domain separation between AD and message
}
/**
 * @brief Finalize the sponge and squeeze out the 16-byte authentication tag.
 *
 * XORs the key into the capacity, runs the 12-round permutation, then XORs the
 * key back and emits the tag from the last two state words.
 *
 * @param[in,out] s   the sponge state (after all data is processed).
 * @param[in]     K0  the first key word from AsconInit().
 * @param[in]     K1  the second key word from AsconInit().
 * @param[out]    tag the 16-byte authentication tag.
 */
void AsconFinal(AsconState& s, uint64_t K0, uint64_t K1, uint8_t tag[16]) {
    s.x1 ^= K0; s.x2 ^= K1;
    AsconP12(s);
    s.x3 ^= K0; s.x4 ^= K1;
    AsconStoreBe(tag, 8, s.x3); AsconStoreBe(tag + 8, 8, s.x4);
}

}  // namespace

void Ascon128Encrypt(const uint8_t key[16], const uint8_t npub[16],
                     const uint8_t* ad, size_t adlen,
                     const uint8_t* pt, size_t ptlen,
                     uint8_t* ct, uint8_t tag[16]) {
    AsconState s; uint64_t K0, K1;
    AsconInit(s, key, npub, K0, K1);
    AsconAbsorbAd(s, ad, adlen);
    while (ptlen >= 8) {
        s.x0 ^= AsconLoadBe(pt, 8);
        AsconStoreBe(ct, 8, s.x0);
        AsconP6(s);
        pt += 8; ct += 8; ptlen -= 8;
    }
    uint8_t blk[8] = {0}; memcpy(blk, pt, ptlen); blk[ptlen] = 0x80;
    s.x0 ^= AsconLoadBe(blk, 8);
    AsconStoreBe(ct, ptlen, s.x0);   // emit only the ptlen ciphertext bytes
    AsconFinal(s, K0, K1, tag);
}

bool Ascon128Decrypt(const uint8_t key[16], const uint8_t npub[16],
                     const uint8_t* ad, size_t adlen,
                     const uint8_t* ct, size_t ctlen,
                     const uint8_t* tag, size_t taglen,
                     uint8_t* pt) {
    AsconState s; uint64_t K0, K1;
    AsconInit(s, key, npub, K0, K1);
    AsconAbsorbAd(s, ad, adlen);
    while (ctlen >= 8) {
        uint64_t c = AsconLoadBe(ct, 8);
        AsconStoreBe(pt, 8, s.x0 ^ c);
        s.x0 = c;
        AsconP6(s);
        ct += 8; pt += 8; ctlen -= 8;
    }
    uint64_t cword = AsconLoadBe(ct, ctlen);  // top ctlen bytes = ct, rest 0
    AsconStoreBe(pt, ctlen, s.x0 ^ cword);       // recover plaintext bytes
    uint64_t mask = ctlen ? (~0ULL << (8 * (8 - ctlen)))
                          : 0;                   // top ctlen bytes
    s.x0 = (s.x0 & ~mask) | (cword & mask);    // state top bytes <- ciphertext
    s.x0 ^= (0x80ULL << (8 * (7 - ctlen)));        // re-insert the pad bit
    uint8_t computed[16];
    AsconFinal(s, K0, K1, computed);
    uint8_t acc = 0;
    for (size_t i = 0; i < taglen; i++) acc |= computed[i] ^ tag[i];
    return acc == 0;
}

void AsconKdf16(const uint8_t* in, size_t len, uint8_t out[16]) {
    AsconState s;
    s.x0 = 0x4b44463136000000ULL;   // domain-separation IV: "KDF16"
    s.x1 = s.x2 = s.x3 = s.x4 = 0;
    AsconP12(s);
    while (len >= 8) {
        s.x0 ^= AsconLoadBe(in, 8); AsconP12(s); in += 8; len -= 8;
    }
    uint8_t blk[8] = {0}; memcpy(blk, in, len);
    blk[len] = 0x80;   // pad last block
    s.x0 ^= AsconLoadBe(blk, 8); AsconP12(s);
    AsconStoreBe(out, 8, s.x0); AsconP12(s);
    AsconStoreBe(out + 8, 8, s.x0);   // squeeze 16
}

}  // namespace link_layer
