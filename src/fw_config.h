/**
 * @file fw_config.h
 * @brief Shared firmware contract: build-time config, constants, the
 *        settings/mode types, and the `extern` globals every module shares.
 *
 * Every firmware translation unit (main.cpp, fw_radio.cpp, fw_host.cpp, ...)
 * includes this so they agree on one definition of the shared state. The
 * globals are DEFINED once in main.cpp; here they are only declared extern.
 */
#ifndef LORA_SERIAL_FW_CONFIG_H_
#define LORA_SERIAL_FW_CONFIG_H_

#include <Preferences.h>
#include <RadioLib.h>
#include <esp_system.h>     // esp_reset_reason(), esp_random()
// portable windowed-ARQ link layer (compress + AEAD)
#include "adr.h"            // link_layer::AdrController — 'auto' mode decisions
#include "linklayer.h"
#include "modeswitch.h"     // coordinated runtime PHY mode-switch handshake
#include "x25519.h"         // ECDH for AT+TRAIN secure pairing

// One firmware, flashed to BOTH boards: the initiator/responder role and the
// 1-byte addresses are auto-elected from the chip MACs (+ proximity pairing),
// so there is no per-board build. Build/flash with the single `node_raw` env.

// Firmware version — the single source of truth for "which release is this?".
// tools/version.py injects -D FW_VERSION="<git describe>" at build time (the
// exact tag on a release build, e.g. v0.1.2, or v0.1.2-3-gabc1234 on a dev
// build past the tag). This default covers a build with no git metadata. It is
// reported by ATI (fw=...), AT+VER, and the USB boot banner.
#ifndef FW_VERSION
#define FW_VERSION "dev"   ///< overridden by tools/version.py at build time
#endif

/**
 * @brief Map a compact 1-byte bandwidth code to its bandwidth in kHz.
 *
 * The 1-byte form is what gets stored in configs/modes; this expands it for the
 * radio API. Any code other than 0 (125 kHz) or 1 (250 kHz) means 500 kHz.
 *
 * @param[in] code  bandwidth code: 0 = 125, 1 = 250, else 500 kHz.
 * @return the bandwidth in kHz.
 */
inline float BwFromCode(uint8_t code) {
    return code == 0 ? 125.0f : code == 1 ? 250.0f : 500.0f;
}

// ---- Feature toggles (stored in cfg.feat). Bit 0x02 is reserved (was UART
// flow control). ----
#define FEAT_COMP 0x01   ///< per-frame compression
#define FEAT_ENC  0x04   ///< Ascon-128 AEAD (authenticated encryption + replay)
#define FEAT_ADR  0x08   ///< 'auto' mode: adaptive data rate (initiator-driven)
#define FEAT_GFSK 0x10   ///< let 'auto' step up into GFSK ('ludicrous'); opt-in
/**
 * @brief Forward secrecy: per-session ephemeral-key handshake (opt-in; off by
 *        default). The static key gives authenticated+replay-safe encryption —
 *        FS adds a fresh per-session key on top, see fw_session.
 */
#define FEAT_FS   0x20
/**
 * @brief Auto TX-power control (OPT-IN; OFF by default). The loop is
 *        PEER-SNR feedback: each side holds its own TX power so the SNR the
 *        peer reports for it (via the link's authenticated aux byte) stays a
 *        margin above the mode's demod floor. This fixes the asymmetric-link
 *        starvation of the former own-RSSI loop, which floored one side's power
 *        and deafened the peer on a path with unequal loss each way (sim:
 *        test_autopower_own_rssi_starves_asym vs test_autopower_peer_snr_*).
 *        OFF by default (enable with AT+APWR=1) — field testing surfaced a
 *        mode-switch interaction that still needs validation (see the JOURNEY).
 */
#define FEAT_APWR 0x40

/**
 * @brief Built-in fallback key (used only until the pair is "trained" via
 *        AT+TRAIN, which derives a unique key over X25519 ECDH and stores it in
 *        NVS). It's in the firmware, so it's NOT secret — AT+TRAIN is how you
 *        get a real per-pair secret. See SECURITY.md.
 */
static const uint8_t kLinkKey[16] = {
    0x4c,0x6f,0x52,0x61,0x53,0x68,0x65,0x6c,0x6c,0x4b,0x65,0x79,0x21,0x32,0x33,
    0x34};

/**
 * @brief AEAD on-wire tag length (8 = lean, 16 = max margin). -D LINK_TAGLEN=16
 *        to change.
 */
#ifndef LINK_TAGLEN
#define LINK_TAGLEN 8
#endif
/**
 * @brief On reboot, resume the frame counter at (saved + STRIDE) so counters
 *        that were used but not yet persisted are skipped -> a nonce is NEVER
 *        reused (LoRaWAN 1.1 fix for the counter-reset keystream-reuse bug).
 *        Larger = less NVS wear, more skip.
 */
#define LINK_CTR_STRIDE 256

// Build-time defaults (overridable per env). Runtime values live in NVS and can
// be changed via the config console (and, later, a WiFi UI) without reflashing.

// Role selection. MAC_ROLE (default ON): identical firmware on both boards; at
// boot each beacons its factory MAC and the numerically LOWER MAC becomes the
// initiator (address 1), the other the responder (address 2) — which is why
// there is ONE node_raw env, not per-board NODE_A/NODE_B builds. The discovery
// code lives in fw_device.cpp under #if MAC_ROLE. Build -DMAC_ROLE=0 for the
// legacy scheme where the role comes from the static NODE_ADDR/PEER_ADDR below.
#ifndef MAC_ROLE
#define MAC_ROLE 1  ///< 1 = auto-elect role from MAC; 0 = static addrs
#endif
// PROX_PAIR (default ON, requires MAC_ROLE): first-boot PROXIMITY pairing. An
// unpaired board (no per-pair key in NVS) pairs ONCE at low power with an
// ADJACENT board — electing roles, deriving a unique per-pair key over X25519,
// and persisting both — then skips discovery on every later boot. Build
// -DPROX_PAIR=0 to re-elect from the MAC every boot on the shared built-in key.
#ifndef PROX_PAIR
#define PROX_PAIR 1  ///< 1 = first-boot proximity pairing; 0 = re-elect
#endif
#ifndef NODE_ADDR
#define NODE_ADDR 1          ///< default 1-byte link address (NVS overrides)
#endif
#ifndef PEER_ADDR
#define PEER_ADDR 0          ///< default peer address (0 = accept any peer)
#endif
#ifndef NODE_NAME
#define NODE_NAME "node"     ///< default board name (NVS overrides)
#endif
// Beta defaults: a fresh / factory-reset board boots with COMPRESSION and
// ENCRYPTION on, at a FIXED 'medium' LoRa mode (SF7/BW250) and full TX power.
// (Encryption uses the shared built-in key until AT+TRAIN / proximity pairing
// derives a unique per-pair key.) The two ADAPTIVE features are OFF by default
// and OPT-IN — field testing showed both still need validation:
//   - ADR / 'auto' speed (AT+MODE=auto) adapts the LoRa mode to link SNR, but
//     can wedge in the slowest mode on a marginal link (see the JOURNEY).
//   - Peer-SNR auto-power (AT+APWR=1) trims TX power to the peer's headroom,
//     but a mode switch can then leave the faster mode below its demod floor
//     (the switch keeps the floored power) and revert (see the JOURNEY).
// Fixed medium + full power is the reliable out-of-box path; enable the two for
// experimentation. The GFSK 'ludicrous' rung is also opt-in (AT+ADRGFSK=1).
#ifndef FEAT_DEFAULT
#define FEAT_DEFAULT (FEAT_COMP | FEAT_ENC)
#endif

// Fixed-mode config (radio defaults). Override at build time, e.g.
//   PLATFORMIO_BUILD_FLAGS="-D CFG_SF=9 -D CFG_BWCODE=0 -D CFG_CR=5"
#ifndef CFG_SF
#define CFG_SF 12            ///< default LoRa spreading factor (far/SF12)
#endif
#ifndef CFG_BWCODE
#define CFG_BWCODE 0         ///< default bandwidth code (0 = 125 kHz)
#endif
#ifndef CFG_CR
#define CFG_CR 8             ///< default coding-rate denominator (4/8)
#endif
// Default ARQ window, per mode. Fast modes (low SF / wide BW) want a big window
// to fill the bandwidth-delay product; slow modes (SF12) want a small one (huge
// frame airtime).
#ifndef CFG_WINDOW
#define CFG_WINDOW 8         ///< default ARQ burst window (frames)
#endif

// ---- Radio constants ----
/**
 * @brief Carrier frequency (MHz). REGULATORY (Taiwan NCC LP0002): 920-925 MHz;
 *        923.2 MHz keeps a BW125 (and even BW500) channel inside the band.
 *        VERIFY legal TX power / duty cycle before field use.
 */
static const float    kFreqMhz     = 923.2f;
static const uint8_t  kSyncWord    = 0x12;   ///< private link (not LoRaWAN)
static const uint16_t kPreamble     = 8;     ///< min-ish; shaves airtime/frame
static const float    kTcxoV       = 1.8f;   ///< Wio-SX1262 TCXO on SX1262 DIO3
/// configured MAX TX power (dBm); legal in TW (920-925: 27 dBm EIRP outdoor)
static const int8_t   kTxPowerDbm = 22;
/// No valid RX for this long (ms) => the link is silent. Auto-power then ramps
/// our own TX power UP (fw_host) and reports "no signal" to the peer so it
/// boosts too (fw_device) -- the two halves that re-establish a starved link
/// after a reboot. Above the idle cadence (kIdleGapMax 1 s), below the 5 s
/// radio-stuck floor, so normal chatter never trips it.
static const uint32_t kApSilentMs = 3000;
/**
 * @brief Proximity-pairing TX power (dBm): a deliberately LOW power used only
 *        during first-boot pairing so the link reaches only an ADJACENT board
 *        (proximity = human intent, like NFC/BLE pairing) and can't catch a
 *        distant un-paired peer. ~0 dBm (1 mW) is short-range but forgiving of
 *        antenna/orientation; restored to kTxPowerDbm once paired. See
 *        ProximityPair() and docs (PROX_PAIR).
 */
static const int8_t   kProxPairDbm = 0;
/**
 * @brief Control-config spreading factor: the most robust mode. All
 *        negotiation happens here so a switch message survives a marginal link.
 */
static const uint8_t  kCtrlSf      = 12;
static const uint8_t  kCtrlBwCode  = 0;      ///< control-config bandwidth: 125
static const uint8_t  kCtrlCr      = 8;      ///< control-config coding rate 4/8

// ---- GFSK 'ludicrous' mode params (the SX1262's non-LoRa modulation) ----
// No spreading gain (so short range), but a far higher raw bitrate than LoRa.
// Conservative first values; tune upward once bench-validated.
static const float    kFskBitrate  = 200.0f;  ///< kbps (LoRa tops out ~62.5)
static const float    kFskFreqDev  = 100.0f;  ///< kHz frequency deviation
static const float    kFskRxBw     = 467.0f;  ///< kHz RX bandwidth (>= Carson)
static const uint16_t kFskPreamble = 16;      ///< preamble bits
/**
 * @brief GFSK-only inter-frame pacing (ms): a tiny gap after each GFSK TX, the
 *        idea being to let the peer re-arm its receiver before the next
 *        back-to-back frame. OFF by default: bench testing showed it didn't
 *        reduce the re-arm loss and actually destabilized the close-range link,
 *        while fast-retransmit already recovers that loss cheaply (see
 *        docs/FUTURE_MODES.md). Kept as a tunable for future field experiments.
 *        LoRa is NEVER paced (phy_fsk_ guard). 0 = off.
 */
static const uint32_t kFskTxPaceMs = 0;

// ---- Carrier frequency range ----
// The SX1262 tunes roughly 150-960 MHz; AT+FREQ rejects values outside this, so
// a typo can't put the PLL where the radio can't lock. Which sub-GHz ISM band
// is actually LEGAL is the operator's responsibility (e.g. Taiwan NCC: 920-925
// MHz) — see the Regulatory section of the README.
static const float kFreqMinMhz = 150.0f;   ///< SX1262 lower sub-GHz edge (MHz)
static const float kFreqMaxMhz = 960.0f;   ///< SX1262 upper sub-GHz edge (MHz)

// ---- Shared types ----

/**
 * @brief The runtime modem settings: identity, features, radio mode, carrier.
 *
 * Loaded from / saved to NVS by the Host layer. addr/peer are the 1-byte link
 * addresses; feat is the FEAT_* bitmask; sf/bw_code/cr is the LoRa mode;
 * freq_mhz/sync are the carrier and private-link sync word.
 */
struct ModemSettings {
    uint8_t addr;     ///< our 1-byte link address
    uint8_t peer;     ///< peer's 1-byte address (0 = accept any)
    uint8_t feat;     ///< FEAT_* feature bitmask
    char name[16];    ///< human-readable board name
    uint8_t sf;       ///< LoRa spreading factor (radio mode; runtime, NVS)
    uint8_t bw_code;  ///< bandwidth code 0=125/1=250/2=500 (runtime, NVS)
    uint8_t cr;       ///< LoRa coding-rate denominator (runtime, NVS)
    float freq_mhz;   ///< carrier frequency in MHz (NVS)
    uint8_t sync;     ///< private-link sync word (NVS)
};

/**
 * @brief A named range/speed mode (a preset over SF/BW/CR or GFSK).
 *
 * For LoRa presets, sf/bw_code/cr apply (bw_code: 0=125, 1=250, 2=500). For the
 * GFSK 'ludicrous' mode, fsk=1 and the SF/BW/CR fields are unused (the kFsk*
 * params drive the PHY instead).
 */
struct RfMode {
    const char* name;  ///< preset name (e.g. "medium"); static literal
    uint8_t sf;        ///< LoRa spreading factor (unused when fsk=1)
    uint8_t bw_code;   ///< bandwidth code 0=125/1=250/2=500 (unused when fsk=1)
    uint8_t cr;        ///< LoRa coding-rate denominator (unused when fsk=1)
    uint8_t fsk;       ///< 1 = GFSK 'ludicrous' PHY, 0 = LoRa
};

// ---- Shared globals (DEFINED in main.cpp) ----
extern ModemSettings        cfg;          ///< live runtime modem settings (NVS)
extern Preferences          prefs;        ///< NVS "loramodem" namespace handle
extern link_layer::LinkLayer<4096>  g_link;   ///< windowed-ARQ link engine
extern link_layer::ModeSwitch g_modesw;   ///< coordinated PHY mode-switch state
extern link_layer::AdrController g_adr;   ///< 'auto' mode decision engine
extern Module               radio_module; ///< RadioLib Module backing `radio`
extern SX1262               radio;        ///< the SX1262 radio driver object

/// Radio-op breadcrumb (RTC_NOINIT, survives a watchdog reboot): the last radio
/// op the LOOP entered, stamped by RMARK() in fw_radio.cpp and reported by
/// AT+DIAG (wedgeop), so a wedge-reboot reveals where the loop hung. The RX
/// task uses g_dbg_rx, kept separate so the two tasks don't overwrite each
/// other. Defined in fw_diag.cpp.
extern volatile uint8_t g_dbg_stage;
/// RX-task breadcrumb (RTC_NOINIT): 'd' = inside readData, '.' = idle. Reported
/// by AT+DIAG as rxop; see g_dbg_stage. Defined in fw_diag.cpp.
extern volatile uint8_t g_dbg_rx;

/**
 * @brief Hook called repeatedly while blocked waiting for a radio packet, so
 *        host I/O (USB/UART) keeps flowing instead of starving during
 *        multi-second radio ops. Wired to Host::IdleHook in setup().
 */
extern void (*g_rx_idle_hook)();

// Turn-taking timing (derived per-mode from time-on-air) now lives in the Radio
// class — read it via g_radio.toa_ms(), g_radio.interframe_ms(),
// g_radio.turn_rx_ms(), g_radio.retransmit_ms(), g_radio.listen_ms(),
// g_radio.window() (see fw_radio.h).

// Experimental inter-frame TX pacing, the runtime role, the proximity-pairing
// flag, and ProximityPair() now live in the Device class — read/set them via
// g_device.tx_gap()/g_device.SetTxGap(), g_device.initiator(),
// g_device.pairing(), and g_device.ProximityPair() (see fw_device.h).

// Byte-flow diagnostics (reported by AT+LINK?) — host->link bytes accepted and
// link->host bytes delivered — now live in the Host class. Read them via
// g_host.host_in() / g_host.host_out() (see fw_host.h).

/**
 * @brief The ACTIVE AEAD key the link encrypts with. Normally a copy of
 *        g_static_key, but after a successful session handshake it holds the
 *        fresh per-session key (see fw_session.h). The link layer points at
 *        this buffer, so overwriting it swaps the key live.
 */
extern uint8_t g_link_key[16];

/**
 * @brief The long-term/static key: the AT+TRAIN-paired key from NVS if present,
 *        else the built-in fallback (kLinkKey). The session handshake mixes
 *        this with an ephemeral DH to derive g_link_key; it is also the
 *        fallback when no session is established (e.g. peer on older firmware).
 */
extern uint8_t g_static_key[16];

// TX power, the smoothed RSSI, and the PHY modulation (LoRa/GFSK) now live in
// the Radio class — read them via g_radio.tx_power(), g_radio.rssi(),
// g_radio.have_rssi(), g_radio.phy_fsk(), and set power via
// g_radio.SetTxPower() (see fw_radio.h).

// Recovery counters (this boot), surfaced via AT+LINK? so the soft recoveries
// are observable: radio re-inits (MaybeReinitRadio) and rendezvous fallbacks
// (MaybeRendezvous) now live in the Device class — read them via
// g_device.reinit_count() / g_device.rendezvous_count() (see fw_device.h). Full
// reboots show up in AT+DIAG (boots + lastreset).

#endif  // LORA_SERIAL_FW_CONFIG_H_
