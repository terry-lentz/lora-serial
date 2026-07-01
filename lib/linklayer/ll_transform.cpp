/**
 * @file ll_transform.cpp
 * @brief Implementation of the payload transforms declared in ll_transform.h.
 *
 * Wraps the heatshrink LZSS encoder/decoder behind the Compress/Decompress
 * helpers. Per-function contracts live with the declarations in
 * ll_transform.h.
 */
#include "ll_transform.h"

#include <string.h>

extern "C" {
#include "heatshrink_decoder.h"
#include "heatshrink_encoder.h"
}

namespace link_layer {

// One shared encoder/decoder instance each. Single-threaded use (the link layer
// transforms one frame at a time), reset before every call.
static heatshrink_encoder s_enc;  ///< Shared LZSS encoder (reset per call).
static heatshrink_decoder s_dec;  ///< Shared LZSS decoder (reset per call).

int Compress(const uint8_t* src, size_t len, uint8_t* dst, size_t cap) {
    heatshrink_encoder_reset(&s_enc);
    size_t sunk = 0, out = 0;
    while (sunk < len) {
        size_t g = 0;
        heatshrink_encoder_sink(&s_enc, (uint8_t*)src + sunk, len - sunk, &g);
        sunk += g;
        HSE_poll_res pr;
        do {
            if (out >= cap) return -1;
            size_t o = 0;
            pr = heatshrink_encoder_poll(&s_enc, dst + out, cap - out, &o);
            out += o;
        } while (pr == HSER_POLL_MORE);
    }
    HSE_finish_res fr;
    do {
        fr = heatshrink_encoder_finish(&s_enc);
        if (fr == HSER_FINISH_MORE) {
            if (out >= cap) return -1;
            size_t o = 0;
            heatshrink_encoder_poll(&s_enc, dst + out, cap - out, &o);
            out += o;
        }
    } while (fr == HSER_FINISH_MORE);
    if (out >= len) return -1;   // no gain
    return (int)out;
}

int Decompress(const uint8_t* src, size_t len, uint8_t* dst, size_t cap) {
    heatshrink_decoder_reset(&s_dec);
    size_t sunk = 0, out = 0;
    while (sunk < len) {
        size_t g = 0;
        heatshrink_decoder_sink(&s_dec, (uint8_t*)src + sunk, len - sunk, &g);
        sunk += g;
        HSD_poll_res pr;
        do {
            if (out >= cap) return -1;
            size_t o = 0;
            pr = heatshrink_decoder_poll(&s_dec, dst + out, cap - out, &o);
            out += o;
        } while (pr == HSDR_POLL_MORE);
    }
    HSD_finish_res fr;
    do {
        fr = heatshrink_decoder_finish(&s_dec);
        if (fr == HSDR_FINISH_MORE) {
            if (out >= cap) return -1;
            size_t o = 0;
            heatshrink_decoder_poll(&s_dec, dst + out, cap - out, &o);
            out += o;
        }
    } while (fr == HSDR_FINISH_MORE);
    return (int)out;
}

} // namespace link_layer
