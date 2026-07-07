/**
 * @file fw_device.cpp
 * @brief Device class implementation (see fw_device.h).
 *
 * The device layer is the `Device` class, instantiated ONCE as the static
 * singleton `g_device` (no heap — CLAUDE.md rule 5). It owns the runtime role,
 * the pairing flag, the recovery clocks/counters, and the turn loops'
 * carry-over state as private members; Arduino's setup()/loop() are one-liners
 * in main.cpp that forward to Setup()/Loop(). The file-static tuning constants
 * below are used only by this translation unit.
 */
#include "fw_device.h"

#include "adr.h"         // link_layer::AdrController — portable ADR decision
#include "autopower.h"   // link_layer::SnrToAux — report our SNR to the peer
#include "fw_diag.h"     // g_diag: watchdog pet, no-progress reboot breadcrumb
#include "fw_host.h"     // g_host: host I/O, settings, pairing, telemetry
#include "fw_radio.h"    // g_radio: Tx/Rx, ApplyMode, mode helpers, LED
#include "fw_session.h"  // per-session key handshake (forward secrecy)
#include "identity.h"    // link_layer::ElectInitiator — MAC-based election
#include "platform/platform.h"  // platform::DeviceId, platform::Random32
#ifdef HAS_DISPLAY
#include "fw_display.h"  // g_display: OLED status bar + teletype
#endif

// Single-image safety guard. Both boards flash the SAME node_raw image and
// MUST auto-elect distinct roles/addresses from their MACs at runtime
// (MAC_ROLE, defined in fw_config.h). If MAC_ROLE is ever compiled out (a lost
// #define, a dropped include), RecomputeRole() falls back to the static
// NODE_ADDR default (1) on BOTH boards, so both elect themselves initiator and
// the link never forms. That regression shipped once, silently — the discovery
// code survived a refactor but its enabling #define did not (journey entry 34).
// This turns that failure into a BUILD error, not a dead link found in the
// field. A deliberate legacy static-address build is still allowed: set
// distinct per-board NODE_ADDR/PEER_ADDR (e.g. 1/2 and 2/1) and this passes.
#if !MAC_ROLE && (NODE_ADDR == 1) && (PEER_ADDR == 0)
#error "node_raw needs MAC role election: MAC_ROLE is off yet both boards use \
the default address 1 -> both become initiator, no link. Enable MAC_ROLE, or \
build each board with distinct NODE_ADDR/PEER_ADDR."
#endif

// The single device-orchestration instance (static singleton; no heap —
// rule 5).
Device g_device;

/**
 * @brief Bring-up debug: temporary serial markers to localize a startup/loop
 *        hang on a board with no JTAG. Default OFF; build -DDEBUG_BRINGUP=1 to
 *        enable. Prints to the (raw) USB stream, so only use while debugging,
 *        never in a data run.
 */
#ifndef DEBUG_BRINGUP
#define DEBUG_BRINGUP 0
#endif
/**
 * @brief Bring-up trace macro: printf-to-USB-then-flush when DEBUG_BRINGUP is
 *        on, and a no-op (compiled out) otherwise. Takes the same arguments as
 *        printf.
 */
#if DEBUG_BRINGUP
#define BG(...) do { Serial.printf(__VA_ARGS__); Serial.flush(); } while (0)
#else
#define BG(...) do {} while (0)
#endif

// ---- Radio bring-up recovery -----------------------------------------------
// A WEDGED SX1262 (left in a bad sleep/AGC state by a prior run, or after many
// soft resets without a power cycle) can make begin() fail. RadioLib's brief
// reset pulse may not clear it, but holding NRST LOW far longer often does —
// and even a transient begin() failure clears on a retry. So rather than trap
// on the first failure (a silent permanent hang needing a power cycle — no
// LED, no AT, no coredump, just like an MCU lock-up), bring-up
// retries forever with escalating resets so the board self-recovers. RST pin =
// 42 (per the Module ctor in main.cpp).
static const int      kRadioRstPin  = BOARD_LORA_RST;  ///< SX1262 NRST
static const int      kRadioNssPin  = BOARD_LORA_NSS;  ///< SX1262 NSS
static const uint32_t kNrstHoldMs   = 100;   ///< NRST low to unstick a wedge
static const uint32_t kNrstSettleMs = 250;   ///< post-reset settle (XOSC/TCXO)
static const uint32_t kRadioRetryMs = 1200;  ///< gap between bring-up retries

// --------------------------------------------------------------------------
// Host-port / startup tuning (file-local: only Setup() and the loops use it).
// --------------------------------------------------------------------------
/**
 * @brief USB-CDC RX FIFO size (bytes). Big enough to absorb host bursts while
 *        the radio is mid-transmit; we still drain it into PSRAM
 *        (g_host.IngestInit) for safety.
 */
static constexpr size_t   kUsbRxBufBytes  = 16384;
/**
 * @brief USB-CDC line rate. Cosmetic for native USB (real throughput is ~1
 *        MB/s), but some hosts still expect a baud; 115200 is the conventional
 *        terminal default.
 */
static constexpr uint32_t kSerialBaud     = 115200;
/**
 * @brief Max time (ms) to wait for a host terminal to open the port at boot
 *        before proceeding anyway, so a headless board still comes up without
 *        USB attached.
 */
static constexpr uint32_t kSerialWaitMs   = 3000;
/**
 * @brief After ANY link activity, stay in fast-poll for this long (ms) so
 *        interactive echoes/typing stay snappy; only then back off toward
 *        kIdleGapMax.
 */
static constexpr uint32_t kActivityHoldMs = 1500;
/// fast-poll turn gap (ms) while active/interactive (vs. kIdleGapMax idle)
static constexpr uint32_t kFastGapMs      = 50;
/**
 * @brief Responder: how recently (ms) the host produced bytes for us to treat
 *        the link as interactive/bidirectional (vs. a one-way bulk download).
 */
static constexpr uint32_t kInteractiveMs  = 2000;
/**
 * @brief Responder: when interactive, briefly wait this long (ms) after
 *        delivering host data for the host's echo/response so it piggybacks on
 *        THIS reply (halves round-trip latency). Skipped on one-way bulk, where
 *        it would be dead time.
 */
static constexpr uint32_t kPiggybackMs    = 30;

// --------------------------------------------------------------------------
// ADR ('auto' mode) tuning — copied into the portable AdrController's config in
// Setup(). The decision logic that uses these lives in lib/linklayer/adr.h (so
// it's unit-tested in the native sim); these stay here so the firmware remains
// the one source of truth for the actual numbers.
// --------------------------------------------------------------------------
// Cadence + hysteresis: how often the initiator re-evaluates the link, and how
// many consecutive evaluations a faster mode must look safe before stepping UP
// (we step DOWN to a more robust mode immediately).
static const uint32_t kAdrPeriodMs = 3000;   ///< re-evaluation cadence (ms)
static const int      kAdrUpStable = 3;      ///< stable evals before step-up
// Loss-aware ADR: SNR alone doesn't see interference/multipath/re-arm misses,
// so also watch the link's retransmit rate (frames resent vs sent, since the
// current mode was applied). Need this many frames before trusting the ratio;
// step DOWN (more robust) if loss is at/above the high mark; only step UP if
// it's below the low mark (so we never climb into, or stay stuck on, a mode
// that's actually dropping frames even when its SNR looks fine).
static const uint32_t kAdrMinFrames   = 24;  ///< frames before retx trusted
static const int      kAdrDownRetxPct  = 25; ///< step-down retx high mark (%)
static const int      kAdrUpMaxRetxPct = 8;  ///< step-up retx low mark (%)
/**
 * @brief RSSI (dBm) required before 'auto' will try the GFSK 'ludicrous' rung
 *        (FEAT_GFSK). RSSI, not SNR: LoRa SNR saturates around +11 dB, so even
 *        a 5 cm link reads ~11 and can't clear an SNR gate — RSSI still
 *        discriminates. GFSK has no spreading gain, so demand a strong
 *        (short-range) signal.
 */
static const float    kGfskUpRssi = -70.0f;
// If a requested switch hasn't completed in this long (the handshake's ACK kept
// getting lost on a lossy link), abandon it and don't retry for the cooldown —
// so ADR can't dead-flap the link when the current mode is too lossy to even
// carry the switch handshake.
static const uint32_t kAdrSwitchTimeoutMs = 6000;  ///< abandon a stuck switch
static const uint32_t kAdrCooldownMs      = 30000; ///< no-retry cooldown (ms)
/// After a rendezvous fallback, ADR avoids the failed mode this long (ms) so a
/// fast rung that keeps dying can't make the link flap medium<->fast.
static const uint32_t kAdrFallbackPenaltyMs = 60000;

/**
 * @brief Calm idle keepalive: poll ~1/s when truly idle (stays at 50 ms for
 *        1.5 s after any activity, so interactive typing is still responsive;
 *        only the FIRST byte after a long idle waits up to ~1 s).
 */
static const uint32_t kIdleGapMax = 1000;

// Only kAdrMinFrames is used directly by the firmware (to decide when the retx
// ratio is trustworthy); the rest of the ADR tuning above is copied into the
// portable AdrController's config in Setup().

// --------------------------------------------------------------------------
// Recovery timeouts SCALE with the mode's airtime. At slow modes (far/SF12) a
// single turn or the session handshake legitimately takes many seconds, so a
// fixed timeout would fire mid-exchange and "recover" a link that is actually
// fine — reinit the radio, switch mode, or reboot — and far never links. We
// express each as max(floor, mult * retransmit_ms): the ARQ retransmit
// timeout already scales with time-on-air (see DeriveTiming), so the watchdogs
// stay eager on fast modes (the floors) and patient on slow ones, while always
// preserving the escalation order radio-stuck < rendezvous < reboot. The floors
// are the proven fast-mode values.
// --------------------------------------------------------------------------
static const uint32_t kRadioStuckFloorMs = 5000;   ///< 1st line: reinit radio
static const uint32_t kRendezvousFloorMs = 9000;   ///< 2nd: fall back to a mode
static const uint32_t kRebootFloorMs     = 45000;  ///< last: reboot (>WDT 25s)
static const uint32_t kRadioStuckMult = 3;   ///< ~3 retransmits of silence
static const uint32_t kRendezvousMult = 5;   ///< ~5 retransmits of silence
static const uint32_t kRebootMult     = 12;  ///< ~12 retransmits of silence

// --------------------------------------------------------------------------
// MAC-based role discovery (build with -DMAC_ROLE=1). When ON, BOTH boards run
// identical firmware and elect their roles from their factory MACs at boot:
// each beacons its MAC, and the numerically lower MAC becomes the initiator
// (address 1), the other the responder (address 2). Tying the address to the
// elected role keeps the two addresses distinct, which is REQUIRED because the
// address is part of the AEAD nonce (see identity.h / linklayer.h BuildNonce).
// --------------------------------------------------------------------------
#if MAC_ROLE
/**
 * @brief Cleartext PHY discovery beacon magic: [magic "LRD1"][6-byte MAC].
 *        Distinct from link/session frames. The MAC is public and the AEAD key
 *        still gates all data, so a cleartext beacon leaks nothing — it only
 *        decides which board drives turns.
 */
static const uint8_t kDiscMagic[4] = {0x4C, 0x52, 0x44, 0x31};   // "LRD1"
static const size_t  kDiscFrameLen = 4 + 6;  ///< beacon length: magic + MAC
// Beacon cadence: randomized so two boards booting together desync and stop
// colliding (one transmits while the other listens). After electing we send a
// few more beacons so a peer that elected slightly later still hears us. If the
// peer isn't powered yet, we back off after kDiscFastBeacons so a lone board
// isn't beaconing continuously forever (it still finds the peer within one slow
// interval once it appears).
static const uint32_t kDiscBaseMs   = 150;   ///< fast-phase min gap (ms)
static const uint32_t kDiscJitterMs = 600;   ///< + up to this (random, ms)
static const int      kDiscConfirm  = 6;     ///< beacons sent after electing
static const int      kDiscFastBeacons = 24; ///< fast phase, then back off
static const uint32_t kDiscSlowMs   = 3000;  ///< backed-off min gap (lone)
static const uint32_t kDiscSlowJitterMs = 3000;  ///< backed-off jitter (ms)
/**
 * @brief During proximity pairing, print a "PAIRING" status line to the host
 *        every this many beacons (so a watcher sees the board waiting for its
 *        partner).
 */
static const int      kDiscStatusBeacons = 8;
#endif  // MAC_ROLE

// --------------------------------------------------------------------------
// Setup
// --------------------------------------------------------------------------
void Device::Setup() {
    // Big USB-CDC RX buffer to absorb host bursts. The core still drops if this
    // fills (no NAK — arduino-esp32 #10836), so we additionally drain it into a
    // multi-MB PSRAM ingest ring (g_host.IngestInit) for true large-transfer
    // reliability.
    li_idle_gap_ = kFastGapMs;   // seed the initiator's idle backoff (was the
                                 // function-static init of the old idle_gap)
    platform::HostSetRxBufferSize(kUsbRxBufBytes);
    g_host.IngestInit();   // claim the PSRAM host->link ingest ring before I/O
    Serial.begin(kSerialBaud);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);   // active-high: start off
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < kSerialWaitMs) { }
    BG("\r\n[BG] S0 serial t=%lu\r\n", (unsigned long)millis());

    // Start at the control config.
    // begin(freq, bw, sf, cr, sync, pwr, preamble, tcxo, ldo)
    int16_t state = radio.begin(kFreqMhz, BwFromCode(kCtrlBwCode), kCtrlSf,
                                kCtrlCr, kSyncWord, kTxPowerDbm, kPreamble,
                                kTcxoV, false);
    // Recover a wedged radio instead of hanging silently (see RadioHardNrst).
    // Retry forever with an aggressive NRST between tries; reboot now and then
    // for a fully fresh attempt. The board thus self-heals when the chip
    // un-wedges (or is finally power-cycled) rather than staying dark.
    for (int tries = 1; state != RADIOLIB_ERR_NONE; tries++) {
        RadioHardNrst();
        delay(kRadioRetryMs);
        BG("[BG] radio begin retry %d (err %d)\r\n", tries, (int)state);
        state = radio.begin(kFreqMhz, BwFromCode(kCtrlBwCode), kCtrlSf,
                            kCtrlCr, kSyncWord, kTxPowerDbm, kPreamble,
                            kTcxoV, false);
        if (state != RADIOLIB_ERR_NONE && (tries % 20) == 0) platform::Reboot();
    }
    BG("[BG] S1 radio.begin ok\r\n");
    // Shared post-begin() setup (RF switch, CRC, DIO1 IRQ) AND the bounded SPI
    // BUSY-line timeout — routed through RadioCommonSetup so the timeout is
    // active from boot, not only after the first Reinit/GFSK switch (a stale
    // 1000 ms timeout here is what let the initiator hang in readData).
    g_radio.RadioCommonSetup(true);

    g_radio.ApplyMode(CFG_SF, CFG_BWCODE, CFG_CR);   // initial config
                                                     // (re-applied from NVS
                                                     // below)

    g_host.LoadSettings();
    g_radio.ApplyMode(cfg.sf, cfg.bw_code, cfg.cr);   // apply the saved radio
                                              // mode (overrides build default)
    g_radio.SetFrequency(cfg.freq_mhz);        // apply saved carrier + sync
                                              // (override the begin() defaults)
    g_radio.SetSyncWord(cfg.sync);
    g_host.ApplyLinkConfig();
    BG("[BG] S2 cfg applied mode=%s\r\n", g_radio.CurrentModeName());
    // Role from address: the lower address initiates turns. Same binary on both
    // boards -> they're interchangeable; only the (NVS-settable) address
    // differs.
    RecomputeRole();
    g_modesw.Begin(g_radio.CurrentModeIndex());   // seed the mode-switch engine
                                          // at the loaded mode (idle until a
                                          // switch is requested by ADR/auto)
    // Seed the ADR controller: build its mode ladder from the radio table and
    // copy the firmware's tuning constants into its config (one source of
    // truth — the controller's defaults are only used by the native tests).
    g_radio.BuildAdrLadder(&g_adr.ladder);
    g_adr.cfg.period_ms         = kAdrPeriodMs;
    g_adr.cfg.up_stable         = kAdrUpStable;
    g_adr.cfg.down_retx_pct     = kAdrDownRetxPct;
    g_adr.cfg.up_max_retx_pct   = kAdrUpMaxRetxPct;
    g_adr.cfg.gfsk_up_rssi      = kGfskUpRssi;
    g_adr.cfg.switch_timeout_ms = kAdrSwitchTimeoutMs;
    g_adr.cfg.cooldown_ms       = kAdrCooldownMs;
    g_adr.cfg.fallback_penalty_ms = kAdrFallbackPenaltyMs;
    g_adr.Begin();
    // Start on the static key; the initiator will negotiate a forward-secret
    // per-session key once the link is up (no-op if encryption is off).
    SessionReset();
    BG("[BG] S3 session reset, role=%d\r\n", (int)initiator_);
    g_diag.Init();  // capture last reset reason / boot count; start watchdog
    g_rx_idle_hook = Host::IdleHook;   // service host I/O during radio waits
                               // (no starvation)
    BG("[BG] S4 pre RadioStartTask\r\n");
    g_radio.StartTask();   // start the interrupt RX task + arm continuous RX
                        // (no-op otherwise). After all radio config above.
#if MAC_ROLE
    // Identical firmware on both boards: establish the role now that the radio
    // is armed, then re-apply the link with the elected address and reset the
    // session on the new identity.
#if PROX_PAIR
    // First-boot PROXIMITY pairing: a PAIRED board (per-pair key in NVS) loaded
    // its role from NVS already (g_host.LoadSettings/RecomputeRole above) and
    // skips discovery entirely; an UNPAIRED board pairs ONCE at low power with
    // an adjacent peer (blocks until it appears) and persists role+key.
    if (g_host.IsPaired()) {
        BG("[BG] paired (NVS): skip discovery, role=%d\r\n", (int)initiator_);
    } else {
        ProximityPair();
    }
#else
    // No persistence: elect the role from the MACs every boot (proven path).
    DiscoverRole();
#endif  // PROX_PAIR
    g_host.ApplyLinkConfig();
    g_modesw.Begin(g_radio.CurrentModeIndex());
    SessionReset();
#endif  // MAC_ROLE
    BG("[BG] S5 setup DONE role=%d mode=%s\r\n",
       (int)initiator_, g_radio.CurrentModeName());
    // Boot banner: announce the firmware version to the host once at startup,
    // before any transparent data flows. Human-readable and one line (like the
    // pairing banners), so a host connected at boot sees which build it's on.
    g_host.Emit("[LoRa-Serial] " FW_VERSION "\r\n");
#ifdef HAS_DISPLAY
    g_display.Init();   // OLED status bar + teletype (display-equipped boards)
#endif
}

// --------------------------------------------------------------------------
// Loop — single entry point: run whichever role this board is configured for.
// --------------------------------------------------------------------------
void Device::Loop() {
    g_diag.Pet();  // mark loop alive (also petted from g_host.Poll on waits)
#ifdef HAS_DISPLAY
    g_display.Tick();   // refresh the OLED status bar / teletype (rate-limited)
#endif
#if DEBUG_BRINGUP
    static uint32_t s_hb_at = 0, s_hb_n = 0;
    s_hb_n++;
    if (millis() - s_hb_at >= 1000) {
        s_hb_at = millis();
        Serial.printf("[BRINGUP] loop hb n=%lu t=%lu\r\n",
                      (unsigned long)s_hb_n, (unsigned long)millis());
    }
#endif
    if (initiator_) LoopInitiator(); else LoopResponder();
}

// First-boot PROXIMITY pairing (see PROX_PAIR). Drop to low power so only an
// ADJACENT board is heard, elect the role from the MAC, X25519-train a unique
// per-pair key, persist role+key, then restore full power. Run on BOTH boards
// placed close together. Blocks while pairing; the host-I/O idle hook keeps USB
// serviced (DiscoverRole/RunPairing call it during their listen windows), so
// ATI reports state=pairing throughout. Returns true on success. Also invoked
// by AT+PAIR for an on-demand re-pair.
#if MAC_ROLE
bool Device::ProximityPair() {
    pairing_ = true;
    g_host.Emit("\r\n[LoRa-Serial] PAIRING - bring the other board close, "
                "pair it too...\r\n");
    int8_t saved_pwr = g_radio.tx_power();
    g_radio.SetTxPower(kProxPairDbm);          // low power -> proximity only
    DiscoverRole();                        // elect role (adjacent peer only)
    char fp[5];
    bool ok = g_host.RunPairing(fp);       // X25519 -> unique key, persisted
    if (ok) g_host.PersistRole();          // persist the elected addr/peer
    g_radio.SetTxPower(saved_pwr);         // back to full/normal power
    pairing_ = false;
    digitalWrite(LED_PIN, LOW);            // leave the pairing-blink LED off
    g_host.Emit(ok ? "[LoRa-Serial] PAIRED\r\n"
                   : "[LoRa-Serial] pairing failed (no adjacent peer)\r\n");
    return ok;
}
#else
bool Device::ProximityPair() { return false; }  // no MAC role election here
#endif  // MAC_ROLE

// Recompute the role from the config: the lower address initiates turns.
void Device::RecomputeRole() {
    initiator_ = (cfg.peer == 0) ? (cfg.addr == 1) : (cfg.addr < cfg.peer);
}

// ===========================================================================
// Reliable byte pipe between the USB host and the peer radio, on the portable
// windowed-ARQ link layer (lib/linklayer, unit-tested natively). Per-frame
// compression + Ascon-128 AEAD. Half-duplex turn-taking: the lower address is
// the initiator (drives turns); the other is the responder.
// ===========================================================================

// turn_rx_ms/interframe_ms are derived per-mode from ToA in ApplyMode().
// turn_rx_ms is a PER-FRAME deadline (reset on every received frame), so a
// burst of ANY length is received as long as frames keep arriving — the burst
// ends on INTERFRAME silence or the F_MORE-clear flag, never a fixed
// whole-burst timer.

// --- Turn loops. ONE binary contains both; Loop() picks the role at runtime
// from the address (lower address initiates), so the two boards are
// interchangeable. ---

// Initiator: each turn, send our burst (or a poll), then listen for the
// responder's burst. Idle backoff keeps airtime down; fast while data flows
// either way.
void Device::LoopInitiator() {
    g_dbg_stage = 'P';   // wedge breadcrumb: host Poll / USB-ingest section
    g_host.Poll();
    g_dbg_stage = 'M';   // wedge breadcrumb: modesw + recovery + ADR + session
    g_modesw.Poll(millis()); ServiceModeSwitch();   // probation revert, if any
    MaybeReinitRadio();                      // first: un-wedge a deaf radio
    MaybeRendezvous();                       // then: a PHY/mode mismatch
    MaybeReboot();                            // last: reboot a hard wedge
    DriveAdr();                                  // 'auto': maybe pick a mode
    // Establish a forward-secret session key before normal data (opt-in). While
    // there's no session, this consumes the turn with a handshake exchange.
    if (SessionDriveInitiator()) return;
    uint32_t now = millis();
    bool work = g_link.HasWork();
    if (work) li_last_activity_ = now;
    uint32_t gap = work ? 0 : li_idle_gap_;
    if (now - li_last_turn_ < gap) return;
    li_last_turn_ = now;

    uint8_t fr[link_layer::MAXFRAME]; size_t fl;
    g_link.SetCtrlTx(g_modesw.TxCtrl());   // stamp any mode-switch REQ/ACK
    g_link.BeginTurn();
    bool sent = false;
    while (g_link.NextTx(fr, sizeof(fr), fl, now)) {
        g_radio.Tx(fr, fl); sent = true;   // Tx blinks per TX frame
        if (tx_gap_ms_) delay(tx_gap_ms_);   // re-arm pacing (experimental)
    }
    if (!sent) {   // grant responder a turn
        fl = g_link.MakePoll(fr, sizeof(fr)); g_radio.Tx(fr, fl);
    }

    bool my_turn = false, got_frame = false; uint32_t t0 = millis();
    while (!my_turn && millis() - t0 < g_radio.turn_rx_ms()) {
        uint8_t rx[link_layer::MAXFRAME]; size_t rl;
        if (g_radio.Rx(rx, sizeof(rx), rl, g_radio.interframe_ms())
                == RADIOLIB_ERR_NONE) {
            if (DiscHandleRx(rx, rl)) { t0 = millis(); continue; }
            if (SessionHandleRx(rx, rl)) {
                last_rx_ms_ = millis(); t0 = millis(); continue;
            }
            my_turn = g_link.OnRx(rx, rl, millis());
            if (g_link.TakeValidRx()) last_rx_ms_ = millis();  // real only
            if (g_link.TakePeerReset()) {     // peer rebooted / reset its
                peer_reset_count_++;          // session (epoch changed) — a
                SessionReset();               // climbing count flags a flappy
            }                                 // peer host (see AT+LINK? preset)
            got_frame = true;
            g_radio.LedBlink();     // per-RX-frame activity
            t0 = millis();          // got a frame -> reset deadline (receive
                                    // any-length burst)
        } else break;
    }
    // Feed the peer's control nibble to the switch engine; a received frame
    // also proves the PHY is alive (clears probation). Then apply if the ACK
    // committed us to a new mode.
    g_dbg_stage = 'H';   // wedge breadcrumb: post-turn host-pump section
    if (got_frame) g_modesw.AfterRecv(g_link.CtrlRx(), true, millis());
    // Report how well we heard the peer (peer-SNR auto-power). If we've heard
    // nothing valid for a while, report "no signal" instead of a stale reading
    // — carried even on a poll — so a deaf side tells the peer to get louder
    // and the link re-establishes after a reboot (entry 32).
    bool i_silent = (millis() - last_rx_ms_) > kApSilentMs;
    g_link.SetAuxTx(link_layer::SnrToAux(
        i_silent ? link_layer::kApNoSignalSnr : g_radio.snr()));
    ServiceModeSwitch();
    size_t rcvd = g_host.PumpLinkOut();
    if (rcvd > 0 || g_link.HasWork()) li_last_activity_ = millis();

    // Stay in fast-poll for ~1.5s after ANY activity so echoes + typing bursts
    // come back snappily; only back off after real silence.
    if (millis() - li_last_activity_ < kActivityHoldMs)
        li_idle_gap_ = kFastGapMs;
    else
        li_idle_gap_ = li_idle_gap_ < kIdleGapMax ? li_idle_gap_ * 2
                                                  : kIdleGapMax;
    g_host.MaybeStatus();
}

// Responder: wait for the initiator's burst, then send our burst (data or a
// poll/ack).
void Device::LoopResponder() {
    g_host.Poll();
    g_modesw.Poll(millis()); ServiceModeSwitch();   // probation revert, if any
    MaybeReinitRadio();                      // first: un-wedge a deaf radio
    MaybeRendezvous();                       // then: a PHY/mode mismatch
    MaybeReboot();                            // last: reboot a hard wedge
    uint8_t rx[link_layer::MAXFRAME]; size_t rl;
    if (g_radio.Rx(rx, sizeof(rx), rl, g_radio.listen_ms()) !=
            RADIOLIB_ERR_NONE)
        return;   // window must span a full frame's ToA
    if (DiscHandleRx(rx, rl)) return;   // peer still electing -> we replied
    // A session handshake INIT? Derive the key + reply, then we're done for
    // this turn (the reply was our transmit).
    if (SessionHandleRx(rx, rl)) { last_rx_ms_ = millis(); return; }
    bool my_turn = g_link.OnRx(rx, rl, millis());
    if (g_link.TakeValidRx()) last_rx_ms_ = millis();  // real frame only
    if (g_link.TakePeerReset()) SessionReset();   // initiator rebooted
    g_radio.LedBlink();             // per-RX-frame activity
    uint32_t t0 = millis();
    while (!my_turn && millis() - t0 < g_radio.turn_rx_ms()) {
        if (g_radio.Rx(rx, sizeof(rx), rl, g_radio.interframe_ms())
                == RADIOLIB_ERR_NONE) {
            if (DiscHandleRx(rx, rl)) { t0 = millis(); continue; }
            if (SessionHandleRx(rx, rl)) { last_rx_ms_ = millis(); return; }
            my_turn = g_link.OnRx(rx, rl, millis());
            if (g_link.TakeValidRx()) last_rx_ms_ = millis();
            if (g_link.TakePeerReset()) SessionReset();
            g_radio.LedBlink();     // per-RX-frame activity
            t0 = millis();          // got a frame -> reset deadline (receive
                                    // any-length burst)
        } else break;
    }
    // Got the initiator's burst: feed its control nibble to the switch engine
    // (so a mode-switch REQ is ACKed on our reply below). A received frame also
    // clears probation.
    g_modesw.AfterRecv(g_link.CtrlRx(), false, millis());
    // Report how well we heard the peer (peer-SNR auto-power); "no signal" when
    // we've heard nothing valid for a while, so the peer boosts to reach us.
    bool r_silent = (millis() - last_rx_ms_) > kApSilentMs;
    g_link.SetAuxTx(link_layer::SnrToAux(
        r_silent ? link_layer::kApNoSignalSnr : g_radio.snr()));
    size_t got = g_host.PumpLinkOut();
    g_host.Poll();   // grab fresh host data before replying
    // If we just delivered data (e.g. a keystroke) AND our host is actively
    // producing (interactive/bidirectional), briefly wait for its echo/response
    // so it piggybacks on THIS reply instead of a turn later — ~halves
    // interactive latency. SKIP it on a one-way bulk download (host produces
    // nothing), where the 30 ms is pure dead-time in the ack path that
    // throttles the sender's burst cadence.
    if (g_host.host_in() != lr_host_in_val_) {
        lr_host_in_val_ = g_host.host_in(); lr_host_in_at_ = millis();
    }
    bool interactive = (millis() - lr_host_in_at_) < kInteractiveMs;
    if (got > 0 && interactive) {
        uint32_t t = millis();
        while (millis() - t < kPiggybackMs && g_link.TxPending() == 0) {
            g_host.Poll(); delay(2);
        }
    }

    uint8_t fr[link_layer::MAXFRAME]; size_t fl;
    g_link.SetCtrlTx(g_modesw.TxCtrl());   // ACK rides our reply (old PHY)
    g_link.BeginTurn();
    bool sent = false;
    while (g_link.NextTx(fr, sizeof(fr), fl, millis())) {
        g_radio.Tx(fr, fl); sent = true;   // Tx blinks per TX frame
        if (tx_gap_ms_) delay(tx_gap_ms_);   // re-arm pacing (experimental)
    }
    if (!sent) {   // ack/poll so initiator continues
        fl = g_link.MakePoll(fr, sizeof(fr)); g_radio.Tx(fr, fl);
    }
    // We've transmitted our turn (incl. any ACK on the OLD PHY) — now it's safe
    // to apply an armed mode switch.
    g_modesw.AfterSend(millis()); ServiceModeSwitch();
    g_host.MaybeStatus();
}

// Apply any PHY change the mode-switch engine has decided — a committed switch
// or a probation revert. Idle-safe: does nothing unless a switch is in flight.
void Device::ServiceModeSwitch() {
    int idx;
    while (g_modesw.TakeApply(&idx, millis()))
        g_radio.ApplyModeByIndex(idx);
}

// --- Dead-link rendezvous: the guaranteed recovery from any PHY/mode mismatch
// (a mode switch that half-completed, a peer reboot onto another mode, etc).
// If we've heard NOTHING valid from the peer for the rendezvous timeout, fall
// back to a fixed mode ('medium' — robust + known-good, verified to carry data
// reliably). Both ends do this independently, so they re-converge on the same
// PHY and the link always recovers — no matter how a switch failed. Updated
// whenever a real frame is received.
//
// ONLY in 'auto' (FEAT_ADR): a deliberately PINNED mode must never silently
// change. A pinned pair is always on the same PHY (no divergence is possible
// without ADR switching), so rendezvous is unnecessary there — and firing it on
// a merely-lossy fixed fast link would wrongly drop the user's choice to medium
// (see test/test_sim: test_sim_fixed_mode_holds_on_loss).
void Device::MaybeRendezvous() {
    // Pinned mode never auto-changes — EXCEPT a pinned GFSK ('ludicrous') that
    // went deaf: with no fallback the loop churns on futile GFSK re-inits and
    // starves the host (a hard wedge), so let a deaf pinned GFSK rendezvous to
    // medium too — it always recovers.
    if (!(cfg.feat & FEAT_ADR) && !g_radio.phy_fsk()) return;
    uint32_t rz_ms = RecoveryMs(kRendezvousFloorMs, kRendezvousMult);
    if (millis() - last_rx_ms_ < rz_ms) return;          // silence
    if (millis() - last_rendezvous_ms_ < rz_ms) return;  // rate-limit
    last_rendezvous_ms_ = millis();
    int rz = g_radio.ModeIndexByName("medium");
    if (rz >= 0 && g_radio.CurrentModeIndex() != rz) {
        int failed = g_radio.CurrentModeIndex();   // mode we're abandoning
        g_radio.ApplyModeByIndex(rz);     // known-good common ground
        // Recovery is a clean slate: a full re-init (resets ARQ seq + bumps
        // the epoch) so the two ends, whose state diverged while mismatched,
        // resync from scratch on the rendezvous PHY. (Unlike a normal mode
        // switch, here we WANT the reset.)
        g_host.ApplyLinkConfig();
        g_modesw.Begin(g_radio.CurrentModeIndex());
        // Tell ADR the fast mode just died so its flap guard won't re-climb
        // straight back into it (or a faster one) until the penalty expires.
        g_adr.OnFallback(millis(), failed);
        rendezvous_count_++;
    }
    // NOTE: do NOT reset last_rx_ms_ — a dead link must keep the silence clock
    // counting up so MaybeReboot can escalate to a full reboot.
}

// --- Radio-stuck watchdog: the FIRST-line recovery, layered below rendezvous.
// A LoRa radio can silently wedge — RX just stops with no mode mismatch at all
// (seen on hardware as rssi pinned at the floor, snr garbage, tx=0, and ~100%
// retx on the still-trying peer). Rendezvous can't fix that: it only CHANGES
// MODE, and only in 'auto'. So if we've heard nothing valid for the radio-stuck
// timeout, do a full radio re-init at the CURRENT mode (Radio::Reinit) to
// unwedge
// it. Runs in ALL modes (a wedge is mode-independent) and preserves the mode
// and the link/ARQ/session, so it's safe even in a deliberately pinned mode —
// the one recovery rendezvous can't offer there. Fires SOONER than rendezvous
// so the cheap fix is tried first; because it does NOT touch last_rx_ms_, a
// genuine PHY mismatch still escalates to rendezvous on the unchanged silence
// clock. Validated in the native sim (test_sim_radio_stuck_*). Timeout scales
// with ToA (RecoveryMs) so it never fires mid-handshake on a slow mode.
void Device::MaybeReinitRadio() {
    uint32_t now = millis();
    uint32_t stuck_ms = RecoveryMs(kRadioStuckFloorMs, kRadioStuckMult);
    if (now - last_rx_ms_ < stuck_ms) return;      // heard recently
    if (now - last_reinit_ms_ < stuck_ms) return;  // rate-limit re-inits
    last_reinit_ms_ = now;   // NOTE: do NOT touch last_rx_ms_ (see above).
    reinit_count_++;
    g_radio.Reinit();
}

// --- No-progress REBOOT: the last-resort recovery, below re-init + rendezvous.
// On hardware a radio can wedge so hard that a begin() re-init does NOT recover
// it (only a full reboot did — observed: a reflash/reboot revived a responder
// that re-init couldn't, and a loop hang even defeated the software watchdog).
// So if we were LINKED (heard the peer this boot) and have then heard nothing
// valid for the reboot timeout despite the cheaper recoveries, reboot. Gated on
// last_rx_ms_!=0 so a node waiting for an absent peer won't boot-loop (after a
// reboot the clock is 0 until the link comes back). A healthy idle link keeps
// the clock fresh via the ~1 Hz keepalive, so this only fires on a truly dead
// link. Validated in sim (test_sim_hard_wedge_reboot_recovers). Timeout scales
// with ToA (RecoveryMs) so a slow mode's long handshake isn't mistaken for a
// dead link.
void Device::MaybeReboot() {
    if (last_rx_ms_ == 0) return;                    // never linked this boot
    if (millis() - last_rx_ms_ < RecoveryMs(kRebootFloorMs, kRebootMult))
        return;
    g_diag.RebootNoProgress();                       // breadcrumb + reboot
}

// Scale a recovery floor by the mode's airtime: max(floor, mult * ToA).
uint32_t Device::RecoveryMs(uint32_t floor_ms, uint32_t mult) {
    uint32_t scaled = mult * g_radio.retransmit_ms();   // tracks ToA
    return scaled > floor_ms ? scaled : floor_ms;
}

// Initiator-only ADR. On a slow cadence, move both ends toward the fastest mode
// the link can *actually carry* — judged by BOTH measured SNR and the real
// retransmit rate — and (opt-in) into GFSK when the link is very strong. Only
// runs when 'auto' (FEAT_ADR) is enabled; otherwise a no-op. The mode-switch
// counters reset on each switch, so the retx ratio reflects the current mode.
//
// All the decision logic lives in the portable, unit-tested AdrController
// (lib/linklayer/adr.h). This is just the firmware glue: gather the live link
// inputs, ask the controller what to do, and carry it out via the handshake.
void Device::DriveAdr() {
    if (!(cfg.feat & FEAT_ADR)) return;
    g_adr.cfg.gfsk_enabled = (cfg.feat & FEAT_GFSK) != 0;  // runtime-toggleable

    // Loss on the current mode since it was applied (counters reset on switch);
    // -1 until we've sent enough frames for the ratio to mean anything.
    uint32_t txc = g_link.DbgStatTx();
    int retx_pct = (txc >= kAdrMinFrames)
                       ? (int)(100 * g_link.DbgStatRetx() / txc) : -1;

    link_layer::AdrController::In in;
    in.now       = millis();
    in.busy      = g_modesw.Busy();
    in.have_link = g_radio.have_rssi();
    in.cur       = g_modesw.Current();
    in.snr       = g_radio.snr();
    in.rssi      = g_radio.rssi();
    in.retx_pct  = retx_pct;

    link_layer::AdrAction act = g_adr.Decide(in);
    switch (act.kind) {
        case link_layer::ADR_REQUEST: g_modesw.Request(act.mode); break;
        case link_layer::ADR_ABORT:   g_modesw.Abort();           break;
        case link_layer::ADR_NONE:    default:                     break;
    }
}

// --------------------------------------------------------------------------
// MAC / proximity role discovery
// --------------------------------------------------------------------------
#if MAC_ROLE
void Device::SendDiscBeacon() {
    uint8_t fr[kDiscFrameLen];
    memcpy(fr, kDiscMagic, 4);
    memcpy(fr + 4, my_mac_, 6);
    g_radio.Tx(fr, sizeof fr);
}

// Block (servicing host I/O so AT stays live, and a lone board waits patiently
// for its peer) until we hear the peer's beacon and elect a role; then set the
// address/peer/initiator from the MAC compare. Runs after the radio is armed.
void Device::DiscoverRole() {
    platform::DeviceId(my_mac_);
    uint8_t rx[link_layer::MAXFRAME]; size_t rl;
    bool elected = false;
    int beacons = 0;
    while (!elected) {
        SendDiscBeacon();
        if (pairing_) {                   // proximity pairing: surface progress
            g_radio.LedBlink();           // distinct blink at the beacon rate
            if (beacons % kDiscStatusBeacons == 0)
                g_host.Emit("[LoRa-Serial] PAIRING - bring the other board "
                            "close and pair it too...\r\n");
        }
        bool fast = ++beacons <= kDiscFastBeacons;   // back off if alone a bit
        uint32_t window = (fast ? kDiscBaseMs : kDiscSlowMs) +
            (platform::Random32() % (fast ? kDiscJitterMs : kDiscSlowJitterMs));
        if (g_radio.Rx(rx, sizeof rx, rl, window) == RADIOLIB_ERR_NONE &&
            rl == kDiscFrameLen && memcmp(rx, kDiscMagic, 4) == 0 &&
            memcmp(rx + 4, my_mac_, 6) != 0) {         // ignore our own echo
            initiator_ = link_layer::ElectInitiator(my_mac_, rx + 4);
            link_layer::AssignAddrs(initiator_, &cfg.addr, &cfg.peer);
            elected = true;
        }
    }
    for (int i = 0; i < kDiscConfirm; i++) {           // help a late peer elect
        SendDiscBeacon();
        delay(kDiscBaseMs + (platform::Random32() % kDiscJitterMs));
    }
    BG("[BG] discover: initiator=%d addr=%d peer=%d\r\n",
       (int)initiator_, cfg.addr, cfg.peer);
}
#endif  // MAC_ROLE

// In the normal loop: a peer still in discovery may beacon us (it missed our
// confirm beacons). Reply so it can elect. Returns true if it was a beacon.
#if MAC_ROLE
bool Device::DiscHandleRx(const uint8_t* rx, size_t rl) {
    if (rl != kDiscFrameLen || memcmp(rx, kDiscMagic, 4) != 0) return false;
    SendDiscBeacon();
    return true;
}
#else
bool Device::DiscHandleRx(const uint8_t*, size_t) { return false; }
#endif  // MAC_ROLE

// --------------------------------------------------------------------------
// Radio bring-up recovery
// --------------------------------------------------------------------------
// Recover a hung SX1262 without a power cycle, two ways:
//  1) NRST pulses far longer than RadioLib's brief reset.
//  2) An NSS wake pulse: the SX126x wakes from sleep() on an NSS falling edge +
//     a ~3.5 ms BUSY-clear delay, NOT reliably on NRST — so if a prior warm
//     sleep() left it asleep and NRST can't wake it, this can. (A
//     CHIP_NOT_FOUND that survives BOTH is a hardware/SPI fault.)
void Device::RadioHardNrst() {
    pinMode(kRadioRstPin, OUTPUT);
    for (int i = 0; i < 3; i++) {
        digitalWrite(kRadioRstPin, LOW);  delay(kNrstHoldMs);
        digitalWrite(kRadioRstPin, HIGH); delay(kNrstHoldMs);
    }
    // NSS wake-from-sleep: drive NSS low (the wake trigger), hold past the
    // chip's wake/BUSY-clear time, then release high.
    pinMode(kRadioNssPin, OUTPUT);
    digitalWrite(kRadioNssPin, LOW);  delay(10);
    digitalWrite(kRadioNssPin, HIGH);
    delay(kNrstSettleMs);
}
