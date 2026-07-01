/**
 * @file session.cpp
 * @brief Implementation of the per-session key handshake from session.h.
 *
 * Derives the per-session key (static key + ephemeral X25519 DH + both
 * publics) and drives one end of the two-message exchange. Per-function
 * contracts live with the declarations in session.h.
 */
#include "session.h"

#include <string.h>

#include "ll_aead.h"     // AsconKdf16
#include "x25519.h"      // ephemeral ECDH

namespace link_layer {

void DeriveSessionKey(const uint8_t static_key[16],
                      const uint8_t my_eph_priv[32],
                      const uint8_t peer_eph_pub[32],
                      const uint8_t init_pub[32],
                      const uint8_t resp_pub[32],
                      uint8_t out_key[16]) {
    uint8_t dh[32];
    x25519::scalarmult(dh, my_eph_priv, peer_eph_pub);   // ephemeral shared sec

    // Transcript: static_key || DH || init_pub || resp_pub. Binding the static
    // key authenticates; the DH gives forward secrecy; both publics (fixed
    // order) tie the key to this exact exchange.
    uint8_t buf[16 + 32 + 32 + 32];
    memcpy(buf,            static_key, 16);
    memcpy(buf + 16,       dh,         32);
    memcpy(buf + 48,       init_pub,   32);
    memcpy(buf + 80,       resp_pub,   32);
    AsconKdf16(buf, sizeof(buf), out_key);
}

void SessionHandshake::Begin(const uint8_t static_key[16], bool initiator,
                             const uint8_t eph_priv[32]) {
    memcpy(static_key_, static_key, 16);
    memcpy(eph_priv_, eph_priv, 32);
    initiator_ = initiator;
    x25519::scalarmult_base(my_pub_, eph_priv_);   // our ephemeral public
}

bool SessionHandshake::OnPeerPublic(const uint8_t peer_pub[32]) {
    // Transcript order is always initiator's public first, then responder's.
    const uint8_t* init_pub = initiator_ ? my_pub_ : peer_pub;
    const uint8_t* resp_pub = initiator_ ? peer_pub : my_pub_;
    DeriveSessionKey(static_key_, eph_priv_, peer_pub, init_pub, resp_pub,
                     session_key_);
    return true;
}

} // namespace link_layer
