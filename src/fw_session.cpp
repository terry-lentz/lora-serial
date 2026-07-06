/**
 * @file fw_session.cpp
 * @brief Per-session key handshake glue (see fw_session.h).
 *
 * Drives the portable lib/linklayer/session.h choreography over the radio: it
 * exchanges ephemeral X25519 public keys with the peer, derives the per-session
 * AEAD key, and installs it into g_link_key. The file-local statics below hold
 * the in-flight handshake state for this translation unit.
 */
#include "fw_session.h"

#include <string.h>

#include "fw_config.h"   // cfg, g_link_key, g_static_key, FEAT_ENC
#include "fw_device.h"   // g_device.initiator() — drives which side we are
#include "fw_radio.h"    // g_radio.Tx, g_radio.Rx
#include "platform/platform.h"  // platform::Random32 (session-key entropy)
#include "session.h"     // link_layer::SessionHandshake

/**
 * @brief Handshake frame magic. The wire frame is
 *        [magic(4)][type(1)][ephemeral public key(32)] = 37 B, sent as a raw
 *        PHY payload (the pubkeys are public, so they go in the clear;
 *        authentication is implicit — only a holder of the static key derives
 *        the matching session key). A distinct magic keeps these apart from
 *        AT+TRAIN pairing frames and from normal link frames.
 */
static const uint8_t kHsMagic[4] = {0xA5, 0x53, 0x4B, 0x31};  // "·SK1"
static const uint8_t kHsInit = 1;   ///< type byte: initiator -> responder
static const uint8_t kHsResp = 2;   ///< type byte: responder -> initiator
static const size_t  kHsPubLen = 32;                  ///< ephemeral pubkey len
static const size_t  kHsFrameLen = 4 + 1 + kHsPubLen; ///< handshake frame: 37

// Minimum gap between INIT attempts while unestablished; a floor on the reply
// listen window (the real window is mode-sized, see below); how many attempts
// before we FALL BACK to the static key so data can flow; and how long to wait
// before trying the handshake again after falling back.
static const uint32_t kHsAttemptMs  = 150;   ///< min gap between INIT attempts
static const uint32_t kHsListenMinMs = 400;  ///< floor on the reply-listen win
static const int      kHsMaxAttempts = 6;    ///< tries before static fallback
static const uint32_t kHsRetryMs   = 20000;  ///< re-try gap after a fallback

static link_layer::SessionHandshake g_hs;  ///< in-flight handshake state
static bool g_session_active = false;      ///< true once a session key is up
static bool g_hs_started = false;   ///< have we generated our ephemeral yet?

/**
 * @brief Fill a buffer with hardware randomness.
 *
 * platform::Random32() yields 32 bits per call; fills four bytes at a time.
 *
 * @param[out] p  destination buffer. Must not be null.
 * @param[in]  n  number of random bytes to write.
 */
static void FillRandom(uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i += 4) {
        uint32_t r = platform::Random32();
        for (size_t j = 0; j < 4 && i + j < n; j++)
            p[i + j] = (uint8_t)(r >> (8 * j));
    }
}

/**
 * @brief Lazily start a handshake by generating our ephemeral keypair.
 *
 * Generates the keypair once per session reset and keeps it stable across
 * retries, so a re-derive is idempotent. A no-op if already started.
 */
static void EnsureStarted() {
    if (g_hs_started) return;
    uint8_t eph_priv[32];
    FillRandom(eph_priv, sizeof(eph_priv));
    g_hs.Begin(g_static_key, g_device.initiator(), eph_priv);
    g_hs_started = true;
}

/**
 * @brief Transmit a handshake frame carrying our ephemeral public key.
 *
 * @param[in] type  the handshake type byte (kHsInit or kHsResp).
 */
static void SendHs(uint8_t type) {
    uint8_t fr[kHsFrameLen];
    memcpy(fr, kHsMagic, 4);
    fr[4] = type;
    memcpy(fr + 5, g_hs.MyPublic(), kHsPubLen);
    g_radio.Tx(fr, sizeof(fr));
}

/**
 * @brief Install the derived per-session key as the live AEAD key.
 */
static void InstallSessionKey() {
    memcpy(g_link_key, g_hs.SessionKey(), 16);
    g_session_active = true;
}

void SessionReset() {
    memcpy(g_link_key, g_static_key, 16);   // back to the static key
    g_session_active = false;
    g_hs_started = false;                    // fresh ephemeral next handshake
}

bool SessionActive() { return g_session_active; }

bool SessionDriveInitiator() {
    if (!(cfg.feat & FEAT_FS)) return false;    // FS opt-in; off -> static key
    if (!(cfg.feat & FEAT_ENC)) return false;   // no key, nothing to negotiate
    if (g_session_active) return false;
    static uint32_t last_attempt = 0, backoff_until = 0;
    static int attempts = 0;
    uint32_t now = millis();

    // If the peer never answers (e.g. it's on older firmware), don't wedge data
    // behind a handshake that won't complete: after a few tries, fall back to
    // the static key (return false -> normal data turns) and retry later.
    if (now < backoff_until) return false;
    if (now - last_attempt < kHsAttemptMs) return false;
    last_attempt = now;

    EnsureStarted();
    SendHs(kHsInit);

    // Listen for the reply. Window is mode-sized (turn_rx_ms already accounts
    // for a frame's time-on-air at the current SF/BW) so the round-trip fits
    // even on the slow/far modes.
    uint32_t turn_rx = g_radio.turn_rx_ms();
    uint32_t window = turn_rx > kHsListenMinMs ? turn_rx : kHsListenMinMs;
    uint8_t rx[link_layer::MAXFRAME]; size_t rl;
    uint32_t t0 = millis();
    while (millis() - t0 < window) {
        if (g_radio.Rx(rx, sizeof(rx), rl, window) == RADIOLIB_ERR_NONE) {
            SessionHandleRx(rx, rl);
            if (g_session_active) break;
        } else {
            break;
        }
    }
    if (g_session_active) { attempts = 0; return true; }
    if (++attempts >= kHsMaxAttempts) {   // give up for now, let data flow
        attempts = 0; backoff_until = now + kHsRetryMs;
        return false;
    }
    return true;   // keep this turn for the handshake
}

bool SessionHandleRx(const uint8_t* rx, size_t len) {
    if (!(cfg.feat & FEAT_FS)) return false;   // FS off -> not our frame
    if (len != kHsFrameLen || memcmp(rx, kHsMagic, 4) != 0) return false;
    if (!(cfg.feat & FEAT_ENC)) return true;   // ignore HS frames if not enc
    uint8_t type = rx[4];
    const uint8_t* peer_pub = rx + 5;

    if (type == kHsInit && !g_device.initiator()) {
        // Responder: derive against our (stable) ephemeral, install, and reply.
        // Idempotent for a repeated INIT; re-keys if the initiator restarted.
        EnsureStarted();
        g_hs.OnPeerPublic(peer_pub);
        InstallSessionKey();
        SendHs(kHsResp);
        return true;
    }
    if (type == kHsResp && g_device.initiator()) {
        // Initiator: derive + install. Ignore once we're already established.
        if (g_hs_started && !g_session_active) {
            g_hs.OnPeerPublic(peer_pub);
            InstallSessionKey();
        }
        return true;
    }
    return true;   // a handshake frame, just not one we act on -> still consume
}
