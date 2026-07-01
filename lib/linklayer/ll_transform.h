/**
 * @file ll_transform.h
 * @brief Portable payload compression for the link layer (heatshrink LZSS).
 *
 * No Arduino deps -> compiles on host and ESP32. (Frame encryption is the
 * Ascon-128 AEAD in ll_aead.h; this file is compression only.) Declarations
 * only — the definitions (which loop, so they don't belong inline in a header
 * per the Google C++ style guide) live in ll_transform.cpp.
 */
#ifndef LINK_LAYER_LL_TRANSFORM_H_
#define LINK_LAYER_LL_TRANSFORM_H_
#include <stddef.h>
#include <stdint.h>

namespace link_layer {

/**
 * @brief Compress src -> dst (heatshrink LZSS).
 *
 * @param[in]  src bytes to compress.
 * @param[in]  len length of `src` in bytes.
 * @param[out] dst destination for the compressed bytes.
 * @param[in]  cap capacity of `dst`.
 * @return compressed length, or -1 if it doesn't fit or doesn't shrink.
 */
int Compress(const uint8_t* src, size_t len, uint8_t* dst, size_t cap);

/**
 * @brief Decompress src -> dst (heatshrink LZSS).
 *
 * @param[in]  src compressed bytes.
 * @param[in]  len length of `src` in bytes.
 * @param[out] dst destination for the decompressed bytes.
 * @param[in]  cap capacity of `dst`.
 * @return decompressed length, or -1 on failure.
 */
int Decompress(const uint8_t* src, size_t len, uint8_t* dst, size_t cap);

} // namespace link_layer

#endif  // LINK_LAYER_LL_TRANSFORM_H_
