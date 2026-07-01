/**
 * @file fw_session.h
 * @brief Firmware glue for the per-session key handshake (forward secrecy).
 *
 * The portable crypto + choreography live in lib/linklayer/session.h (and are
 * unit-tested there); this runs that handshake over the radio at link bring-up
 * and installs the resulting key into g_link_key (which the AEAD reads live, so
 * the swap is instant).
 *
 * It's automatic and self-healing: the initiator keeps sending a handshake INIT
 * until it has a session; the responder answers any INIT idempotently; and a
 * peer reboot (observed via the link's epoch change) triggers a fresh handshake
 * on both ends. If encryption is off, or the peer never answers (e.g. older
 * firmware), the link simply stays on the static key. No AT command, no config.
 * Implementation in fw_session.cpp.
 */
#ifndef LORA_SERIAL_FW_SESSION_H_
#define LORA_SERIAL_FW_SESSION_H_
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Revert to the static key and force a fresh handshake.
 *
 * Call at boot, after (re)pairing, on a local NewSession, and on a peer reset.
 */
void SessionReset();

/**
 * @brief Whether a forward-secret per-session key is installed.
 *
 * @return true if a per-session key is in use; false if on the static key.
 */
bool SessionActive();

/**
 * @brief Initiator only: drive the handshake if there's no session yet.
 *
 * If unestablished, sends a handshake INIT and listens briefly for the reply.
 * Call once at the top of the initiator turn.
 *
 * @return true if it used this turn for the handshake (the caller should skip
 *         its normal data turn); false to proceed with normal data.
 */
bool SessionDriveInitiator();

/**
 * @brief Either role: examine a just-received raw radio frame for a handshake.
 *
 * If it's a handshake message, handle it (responder: derive the key + reply;
 * initiator: derive the key) and return true so the caller does NOT feed it to
 * the link layer.
 *
 * @param[in] rx   the received frame bytes. Must not be null.
 * @param[in] len  the received length in bytes.
 * @return true if the frame was a handshake message (consumed here); false for
 *         ordinary link frames.
 */
bool SessionHandleRx(const uint8_t* rx, size_t len);

#endif  // LORA_SERIAL_FW_SESSION_H_
