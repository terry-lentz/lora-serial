/**
 * @file ll_aead.h
 * @brief Portable Ascon-128 v1.2 AEAD — authenticated encryption.
 *
 * No Arduino deps -> compiles on host (native sim) and ESP32, like
 * ll_transform.h. Ascon is the NIST lightweight-crypto standard family
 * (SP 800-232); this is the well-specified v1.2 "Ascon-128" variant (128-bit
 * key/nonce/tag, 64-bit rate, pa=12 / pb=6 rounds), validated against the
 * official LWC known-answer vectors (see test/test_link — test_aead_kat).
 * Unlike AES-CTR it AUTHENTICATES: any bit flip in ciphertext, tag, or the
 * associated-data header is detected and the frame is rejected. This header
 * is the public API only — the permutation/sponge internals (which loop) live
 * in ll_aead.cpp, per the Google C++ style guide.
 */
#ifndef LINK_LAYER_LL_AEAD_H_
#define LINK_LAYER_LL_AEAD_H_
#include <stddef.h>
#include <stdint.h>

namespace link_layer {

/**
 * @brief Encrypt + authenticate: pt[0..ptlen) -> ct[0..ptlen) plus a tag.
 *
 * Produces a 16-byte tag (the caller may transmit a prefix of it). The nonce
 * MUST be unique per key — we use a monotonic 64-bit counter.
 *
 * @param[in]  key   16-byte Ascon-128 key.
 * @param[in]  npub  16-byte public nonce (unique per key).
 * @param[in]  ad    associated data: authenticated but not encrypted.
 * @param[in]  adlen length of `ad` in bytes.
 * @param[in]  pt    plaintext to encrypt.
 * @param[in]  ptlen length of `pt` in bytes.
 * @param[out] ct    ciphertext output (ptlen bytes).
 * @param[out] tag   16-byte authentication tag.
 */
void Ascon128Encrypt(const uint8_t key[16], const uint8_t npub[16],
                     const uint8_t* ad, size_t adlen,
                     const uint8_t* pt, size_t ptlen,
                     uint8_t* ct, uint8_t tag[16]);

/**
 * @brief Decrypt + verify: recover plaintext only if the tag matches.
 *
 * Uses a constant-time tag compare. On any tamper the data is rejected and
 * `pt` holds untrusted garbage.
 *
 * @param[in]  key    16-byte Ascon-128 key.
 * @param[in]  npub   16-byte public nonce (must match the encrypt nonce).
 * @param[in]  ad     associated data that was authenticated.
 * @param[in]  adlen  length of `ad` in bytes.
 * @param[in]  ct     ciphertext to decrypt.
 * @param[in]  ctlen  length of `ct` in bytes.
 * @param[in]  tag    received authentication tag.
 * @param[in]  taglen number of tag bytes to verify.
 * @param[out] pt     plaintext output (ctlen bytes; valid only on true).
 * @return true if the tag verified; false on any tamper.
 */
bool Ascon128Decrypt(const uint8_t key[16], const uint8_t npub[16],
                     const uint8_t* ad, size_t adlen,
                     const uint8_t* ct, size_t ctlen,
                     const uint8_t* tag, size_t taglen,
                     uint8_t* pt);

/**
 * @brief Hash `in` to a 16-byte key via an Ascon sponge (rate 64, p12).
 *
 * The secure-pairing KDF: link key = AsconKdf16(X25519 shared secret). Not
 * standardized Ascon-Hash, but a sound sponge construction for deriving a
 * symmetric key from an ECDH secret.
 *
 * @param[in]  in  the input bytes to hash (e.g. the ECDH shared secret).
 * @param[in]  len length of `in` in bytes.
 * @param[out] out the derived 16-byte key.
 */
void AsconKdf16(const uint8_t* in, size_t len, uint8_t out[16]);

} // namespace link_layer

#endif  // LINK_LAYER_LL_AEAD_H_
