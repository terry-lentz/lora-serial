/**
 * @file session.h
 * @brief Per-session key handshake for forward secrecy.
 *
 * Pairing (AT+TRAIN) gives the two boards a shared LONG-TERM key. On its own
 * that key encrypts every session forever, so a leak of it exposes all past
 * traffic. This adds a lightweight handshake at link bring-up that mixes in a
 * fresh EPHEMERAL X25519 Diffie-Hellman exchange, producing a different key
 * for every session:
 *   session_key = KDF( static_key || ephemeral_DH || init_pub || resp_pub )
 * The ephemeral_DH (a throwaway keypair per session) gives FORWARD SECRECY —
 * the ephemeral private keys are never stored, so a later static-key leak
 * can't rederive a past session key. The static_key binds the handshake to
 * the paired peer, giving MUTUAL AUTH: a man-in-the-middle who doesn't hold
 * the paired key derives a DIFFERENT session key, so the very first AEAD frame
 * fails its tag check and no data ever flows (authentication is implicit — we
 * never send an auth tag in the handshake itself). Both ephemeral publics are
 * hashed in a FIXED order (initiator's first), so both ends compute the same
 * key and a swapped/tampered public changes it. This is deliberately NOT the
 * literal Noise Protocol Framework — it's a minimal construction from the
 * primitives we already ship (X25519 + the Ascon KDF), targeting the same
 * goals with far less code on the MCU. Pure logic, no radio/Arduino deps, so
 * the whole exchange is run against a simulated channel in the native unit
 * tests (see test/test_link).
 */
#ifndef LINK_LAYER_SESSION_H_
#define LINK_LAYER_SESSION_H_
#include <stdint.h>

namespace link_layer {

/**
 * @brief Derive the per-session key from the static key + ephemeral DH.
 *
 * @param[in]  static_key   the paired long-term key (16 bytes).
 * @param[in]  my_eph_priv  our ephemeral X25519 private scalar (32 bytes).
 * @param[in]  peer_eph_pub the peer's ephemeral public key (32 bytes).
 * @param[in]  init_pub     initiator's ephemeral public (transcript order).
 * @param[in]  resp_pub     responder's ephemeral public (transcript order).
 * @param[out] out_key      the derived 16-byte session key.
 */
void DeriveSessionKey(const uint8_t static_key[16],
                      const uint8_t my_eph_priv[32],
                      const uint8_t peer_eph_pub[32],
                      const uint8_t init_pub[32],
                      const uint8_t resp_pub[32],
                      uint8_t out_key[16]);

/**
 * @brief Drives one end of the two-message session-key handshake.
 *
 * Each side sends MyPublic() and, on receiving the peer's public, derives the
 * shared session key. Loss is the caller's problem: just keep sending
 * MyPublic() until OnPeerPublic() succeeds (the derivation is idempotent).
 * Pure logic — the caller supplies the ephemeral private key (real randomness
 * on hardware, a fixed vector in tests).
 */
class SessionHandshake {
public:
    /**
     * @brief Start a handshake.
     *
     * @param[in] static_key the paired long-term key (16 bytes).
     * @param[in] initiator  true if we are the initiator (transcript order).
     * @param[in] eph_priv   our 32-byte ephemeral private scalar (caller's
     *                       randomness).
     */
    void Begin(const uint8_t static_key[16], bool initiator,
               const uint8_t eph_priv[32]);

    /**
     * @brief Our ephemeral public key, to transmit to the peer.
     * @return pointer to the 32-byte ephemeral public key (owned by this
     *         handshake; valid for its lifetime).
     */
    const uint8_t* MyPublic() const { return my_pub_; }

    /**
     * @brief Feed the peer's ephemeral public key; derive the session key.
     *
     * Safe to call again (idempotent) if the peer retransmits.
     *
     * @param[in] peer_pub the peer's 32-byte ephemeral public key.
     * @return true once the session key has been derived.
     */
    bool OnPeerPublic(const uint8_t peer_pub[32]);

    /**
     * @brief The derived per-session key.
     * @return pointer to the 16-byte session key (owned by this handshake);
     *         valid only after OnPeerPublic() has returned true.
     */
    const uint8_t* SessionKey() const { return session_key_; }

private:
    uint8_t static_key_[16];    ///< Paired long-term key (authenticates).
    uint8_t eph_priv_[32];      ///< Our ephemeral X25519 private scalar.
    uint8_t my_pub_[32];        ///< Our ephemeral X25519 public key.
    uint8_t session_key_[16];   ///< Derived per-session key.
    bool    initiator_ = false;  ///< Are we the initiator? (transcript order).
};

} // namespace link_layer

#endif  // LINK_LAYER_SESSION_H_
