/**
 * @file identity.h
 * @brief Decide each board's role + link address from its hardware ID.
 *
 * Goal: run the SAME firmware on both boards (no node_a / node_b build split)
 * and let them self-organize. Every ESP32 has a unique factory MAC (6 bytes);
 * we compare the two MACs and the numerically LOWER one becomes the initiator.
 * The role maps to a FIXED address (initiator -> 1, responder -> 2) rather
 * than hashing the MAC into the address because the 1-byte link address goes
 * into the AEAD NONCE (src||epoch||counter, see linklayer.h BuildNonce). Two
 * boards sharing an address would reuse keystream — a real cryptographic
 * break. Tying the address to the role guarantees the two ends always hold
 * DISTINCT addresses (1 and 2), so the nonce stays unique per direction. The
 * MAC is only the tie-breaker that decides WHO is the initiator; it never
 * itself appears in a nonce. Pure logic, no platform deps, so the election is
 * unit-tested in the native sim.
 */
#ifndef LINK_LAYER_IDENTITY_H_
#define LINK_LAYER_IDENTITY_H_
#include <stdint.h>
#include <string.h>

namespace link_layer {

// Link addresses assigned by role. Distinct + non-zero (0 is reserved for the
// "accept any peer" sentinel and ADDR_BCAST handling in linklayer.h).
static constexpr uint8_t kAddrInitiator = 1;  ///< Initiator's link address.
static constexpr uint8_t kAddrResponder = 2;  ///< Responder's link address.

/**
 * @brief Is THIS board the initiator?
 *
 * True iff our MAC sorts before the peer's (lexicographic over the 6 bytes,
 * most-significant first — a total order, so for two distinct MACs exactly one
 * side is the initiator). Equal MACs can only happen if a board hears its own
 * echo; we treat that as "not initiator" so it can never make a board talk to
 * itself.
 *
 * @param[in] my_mac   this board's 6-byte factory MAC.
 * @param[in] peer_mac the peer's 6-byte factory MAC.
 * @return true if this board should be the initiator.
 */
inline bool ElectInitiator(const uint8_t my_mac[6], const uint8_t peer_mac[6]) {
    return memcmp(my_mac, peer_mac, 6) < 0;
}

/**
 * @brief Fill (addr, peer) for this board given the elected role.
 *
 * The initiator is address 1 talking to 2; the responder is 2 talking to 1.
 *
 * @param[in]  initiator true if this board is the initiator.
 * @param[out] addr      receives this board's link address.
 * @param[out] peer      receives the peer's link address.
 */
inline void AssignAddrs(bool initiator, uint8_t* addr, uint8_t* peer) {
    *addr = initiator ? kAddrInitiator : kAddrResponder;
    *peer = initiator ? kAddrResponder : kAddrInitiator;
}

}  // namespace link_layer

#endif  // LINK_LAYER_IDENTITY_H_
