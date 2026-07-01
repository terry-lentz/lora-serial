/**
 * @file fw_radio.cpp
 * @brief Radio class implementation (see fw_radio.h).
 *
 * The radio layer is the `Radio` class, instantiated ONCE as the static
 * singleton `g_radio` (no heap — CLAUDE.md rule 5). The class owns the radio's
 * own state: the derived turn timing, the TX power, the PHY (LoRa/GFSK), and
 * the smoothed RSSI all live as private members here (the rest of the firmware
 * reads them through accessors). The file-static tuning constants below feed
 * DeriveTiming().
 */
#include "fw_radio.h"

#include <cmath>       // lroundf — round the SNR floor to an int for auto-power
#include <cstring>     // strcasecmp

#include "adr.h"        // link_layer::AdrLadder, from the radio mode table
#include "fw_host.h"   // g_host.ApplyLinkTiming() — re-push ToA-derived timing

// INTERRUPT-DRIVEN RX (docs/INTERRUPT_RX.md). A high-priority radio task,
// woken by the DIO1 interrupt, drains the SX1262 FIFO promptly on the DIO1 edge
// into an SPSC ring, so back-to-back burst frames can't overrun the 256-byte
// FIFO (the overrun that wedged the older polling receiver), and it re-arms RX
// within microseconds of RX_DONE (decoupled from the main loop's OnRx) so the
// re-arm deaf window shrinks. A per-frame STANDBY keeps the radio AGC-safe (the
// SX1262 AGC-lockup errata). A recursive mutex guards ALL radio
// SPI access (task read vs main-loop TX/arm/reconfig). This is the project's
// single RX path; the historical per-frame/continuous polling variants are gone
// (see CAPABILITIES_JOURNEY).
//
// The interrupt-driven RX state (the SPSC ring, the radio mutex, the RX task
// handle, the armed flag) and the activity-LED / ISR flag now live as PRIVATE
// members of Radio (see fw_radio.h); the methods below operate on them.

// The single radio-layer instance (static singleton; no heap — rule 5).
Radio g_radio;

// --------------------------------------------------------------------------
// Turn-timing tuning constants. These are derived from a single measured
// quantity — the time-on-air (ToA) of one full frame at the CURRENT mode — so
// the whole half-duplex turn schedule self-tunes across SF/BW (no per-mode
// hand-tuning). All time values are milliseconds unless noted.
// --------------------------------------------------------------------------
/**
 * @brief Representative "full frame" payload size (bytes) fed to getTimeOnAir()
 *        so the ToA estimate reflects a near-MTU frame, not a tiny ack.
 */
static constexpr size_t   kToaProbeBytes    = 250;
/**
 * @brief Floor on the measured ToA (ms). On fast modes ToA rounds toward 0;
 *        keep a small positive value so the derived timers never collapse.
 */
static constexpr uint32_t kToaFloorMs       = 5;
/**
 * @brief Interframe re-arm margin (ms) added to one ToA: long enough to catch
 *        the next back-to-back frame of a burst plus the receiver's re-arm
 *        slack.
 */
static constexpr uint32_t kInterframeMargin = 60;
// Per-frame RX deadline = ToA * mul + margin (ms). Reset on every frame, so it
// only has to cover one frame's airtime plus jitter, not a whole burst.
static constexpr uint32_t kTurnRxToaMul     = 6;    ///< ToA multiplier
static constexpr uint32_t kTurnRxMarginMs   = 200;  ///< added jitter margin
// Retransmit timeout (ms): a lost frame is resent only after we'd reasonably
// expect its ack — i.e. after ONE full burst round-trip. That RTT scales with
// the WINDOW (our burst is `window` frames; the peer replies with ~1), so the
// timer is window-AWARE: (window + spare) * ToA * safety + margin. A fixed
// multiple of ToA is wrong at both extremes — too SHORT vs a big turbo burst
// (it can fire before a 16-frame burst even round-trips -> spurious resends),
// and far TOO LONG vs a tiny far window (a lost far TAIL frame, with no later
// frame to trigger the SACK fast-retransmit, would sit idle for ~12 ToA = tens
// of seconds of dead air, which read as a hung link). kRetransSpareFrames
// covers the peer's reply + turn-around; kRetransSafetyPct adds jitter margin.
static constexpr uint32_t kRetransSpareFrames = 2;     ///< peer reply + turn
static constexpr uint32_t kRetransSafetyPct   = 140;   ///< 1.4x the bare RTT
static constexpr uint32_t kRetransMarginMs    = 400;   ///< added jitter margin
// Responder idle-listen window = ToA + interframe + this margin (ms); floored
// so fast modes stay responsive instead of cycling too tightly.
static constexpr uint32_t kListenMarginMs   = 500;   ///< listen-window margin
static constexpr uint32_t kListenFloorMs    = 3000;  ///< listen-window floor
/**
 * @brief BDP window sizing: target this many ms of burst airtime per
 *        turn-around, so one burst (N*ToA) dwarfs the fixed turn-around
 *        dead-time and amortizes it.
 */
static constexpr uint32_t kAirtimeBudgetMs  = 1500;
/**
 * @brief Smallest BDP window (frames) — never go below 2 so a turn carries real
 *        data.
 */
static constexpr uint32_t kWindowMin        = 2;
/**
 * @brief RSSI exponential-moving-average smoothing factor (alpha): new =
 *        alpha*old + (1-alpha)*sample. 0.8 = slow, stable tracking for auto
 *        power control.
 */
static constexpr float    kRssiEmaAlpha     = 0.8f;

// SPI BUSY-line wait cap (ms). Bounds how long ONE SPI op may block if the
// SX1262 wedges with BUSY stuck high (an AGC/state-machine lockup a mode switch
// can trigger); see RadioCommonSetup() for the full rationale. Far above any
// legitimate BUSY (command microseconds; POR calibration a few ms), so normal
// operation is unaffected — it only bounds the pathological stuck case.
static constexpr uint32_t kSpiBusyTimeoutMs = 100;

// RX storm-breaker (see RadioTask). A burst of this many consecutive FAILED
// reads means RX_DONE is flooding — a fast/sensitive mode after a switch
// hearing noise as packets, or a wedged radio. The RX task then halts RX and
// sleeps for the backoff, handing the lower-priority loop an uncontested window
// to feed its watchdog and run recovery (Reinit / rendezvous).
static const uint32_t kRxStormFails     = 8;   // consecutive failed reads
static const uint32_t kRxStormBackoffMs = 30;  // uncontested loop window (ms)

// Tx() safety timeout: if the DIO1 done-flag never fires (stuck radio)
// give up rather than block the loop forever. It MUST outlast the frame's real
// time-on-air or a long frame is aborted MID-TRANSMISSION — far/SF12/BW125 is
// ~13 s, far longer than a fixed 8 s, so the old timeout cut off every far
// frame (the peer heard nothing). So scale it off the per-frame ToA (toa_ms_)
// with generous headroom, floored for fast modes. A legitimately long wait here
// is safe: g_diag.Pet() (via the rx idle hook) feeds both watchdogs throughout.
static constexpr uint32_t kTxSafetyToaMul   = 2;      ///< >= one frame airtime
static constexpr uint32_t kTxSafetyMarginMs = 1000;   ///< jitter/finish margin
static constexpr uint32_t kTxSafetyFloorMs  = 8000;   ///< floor for fast modes

// ---- Named range/speed modes (presets over SF/BW/CR) ------------------------
/**
 * @brief The named range/speed mode table. A "mode" trades range for speed;
 *        per-mode turn timing is auto-derived from the time-on-air in
 *        ApplyMode(), so each just works. Both ends must use the SAME mode.
 *        bw_code: 0=125, 1=250, 2=500 kHz.
 */
static const RfMode kRfModes[] = {
    {"turbo",  5, 2, 5, 0},   // SF5/500  — fastest LoRa, shortest range
    {"fast",   7, 2, 5, 0},   // SF7/500
    {"medium", 7, 1, 5, 0},   // SF7/250  — default
    {"slow",   9, 0, 6, 0},   // SF9/125
    {"far",   12, 0, 8, 0},   // SF12/125 — slowest, longest range
    {"ludicrous", 0, 0, 0, 1},// GFSK — fastest of all, very short range
};

/**
 * @brief Approx. LoRa demodulator SNR floor (dB) for the LoRa presets, parallel
 *        to the first entries of kRfModes (turbo/fast/medium/slow/far). Higher
 *        SF tolerates lower SNR. The GFSK 'ludicrous' preset has no entry — ADR
 *        never picks it.
 */
static const float kModeSnrFloor[] = {
    -5.0f,    // turbo  SF5
    -7.5f,    // fast   SF7
    -7.5f,    // medium SF7
    -12.5f,   // slow   SF9
    -20.0f,   // far    SF12
};
/**
 * @brief Keep the link this many dB above a mode's floor before trusting it
 *        (headroom for fading); step DOWN to a more robust mode eagerly, UP
 *        only with margin.
 */
static const float kAdrMarginDb = 6.0f;

// Radio-op breadcrumb. Each blocking radio op the LOOP runs stamps a one-char
// tag into g_dbg_stage; the RX task stamps g_dbg_rx separately, so the two
// tasks never overwrite each other's marker. Both are RTC_NOINIT bytes that
// survive a watchdog reboot, snapshotted at the wedge and shown by AT+DIAG
// (wedgeop / rxop) — pinpointing which task hung where. A single byte write (no
// SPI, no print) so it can't perturb timing the way serial markers did. Loop
// tags: a ApplyMode, f ApplyRadioFsk, p setOutputPower, t startTransmit,
// r startReceive, i Reinit. RX-task tags: d in readData, '.' idle.
#define RMARK(c) do { g_dbg_stage = (uint8_t)(c); } while (0)

void Radio::LedBlink() {
    led_state_ = !led_state_; digitalWrite(LED_PIN, led_state_);
}

void Radio::ApplyMode(uint8_t sf, uint8_t bw_code, uint8_t cr) {
    Lock();
    // STANDBY before reconfiguring the modem: SX1262 config commands are
    // standby operations, and reconfiguring SF/BW/CR while the chip is
    // physically in continuous-RX wedges it (it hung the loop -> task watchdog
    // mid-transfer when ADR switched modes under load). Mirror Tx()'s standby
    // dance, then re-arm RX cleanly at the new config.
    radio.standby();
    rx_armed_ = false;
    RMARK('a');  // trace: ApplyMode (LoRa) reconfigure start
    if (phy_fsk_) {
        // Coming back from GFSK: re-initialise the radio in LoRa modulation,
        // then re-apply the saved carrier/sync.
        radio.begin(cfg.freq_mhz, BwFromCode(bw_code), sf, cr, cfg.sync,
                    tx_power_, kPreamble, kTcxoV, false);
        RadioCommonSetup(true);
        phy_fsk_ = false;
    } else {
        radio.setSpreadingFactor(sf);     // recomputes LowDataRateOptimize
        radio.setBandwidth(BwFromCode(bw_code));
        radio.setCodingRate(cr);
    }
    DeriveTiming();
    // Re-arm RX only once the RX task exists; in setup() StartTask() does the
    // initial arm (this runs there too, before the task).
    if (radio_task_) { radio.startReceive(); rx_armed_ = true; }
    Unlock();
}

// Recover a WEDGED radio at the CURRENT mode (deaf RX: rssi pinned, bad snr,
// tx=0, ~100% retx on the peer). This is the cheap, NON-DISRUPTIVE recovery: it
// resets only the RADIO chip, NOT the MCU, so the MCU/link/ARQ/session/host
// session all survive (unlike MaybeReboot's full esp_restart, which is reserved
// for an actual loop hang the TWDT catches). It does a thorough NRST HARDWARE
// reset — the same long, repeated NRST + NSS wake the boot bring-up uses — to
// clear AGC/state-machine wedges that begin() alone (a software reconfigure)
// can't. A SINGLE brief radio.reset() pulse is NOT enough for the deep deaf-
// lockup a mode-switch reconfigure-under-load can trigger; only the longer
// repeated toggle clears it at runtime (the documented SX126x errata are
// host-side resets like this; there's no radio firmware fix). Then re-apply the
// mode. Validated in sim (test_sim_radio_stuck_*); the harder wedge that needs
// a full reboot is test_sim_hard_wedge_reboot_recovers.
void Radio::Reinit() {
    Lock();                        // recursive: ApplyRadioFsk below re-locks OK
    rx_armed_ = false;             // reconfig leaves RX
    RMARK('i');   // breadcrumb: in Reinit — its NRST/begin set no other marker
    // Aggressive NRST (mirrors Device::RadioHardNrst): 3 long reset pulses then
    // an NSS falling-edge wake. A single brief radio.reset() does NOT clear the
    // deep deaf-lockup; this does. Pins per the Module ctor in main.cpp. We
    // feed the task-WDT around the ~0.9 s of resets AND before begin(): a
    // wedged radio makes both slow (each begin() SPI op can burn the full BUSY
    // timeout) and neither feeds the watchdog itself, so without this the
    // recovery itself trips the 5 s task-WDT (the breadcrumb caught it:
    // wedgeop=r, i.e. hung right after the last Rx, in this unmarked re-init).
    const int kNrstPin = 42;   // SX1262 NRST pin (per Module ctor in main.cpp)
    const int kNssPin = 41;    // SX1262 NSS pin  (per Module ctor in main.cpp)
    pinMode(kNrstPin, OUTPUT);
    for (int i = 0; i < 3; i++) {    // 3 long pulses to unstick it
        feedLoopWDT();
        digitalWrite(kNrstPin, LOW);
        delay(100);  // 100 ms low — far longer than RadioLib's reset()
        digitalWrite(kNrstPin, HIGH);
        delay(100);
    }
    pinMode(kNssPin, OUTPUT);  // NSS falling edge wakes the chip from sleep
    digitalWrite(kNssPin, LOW);
    delay(10);
    digitalWrite(kNssPin, HIGH);
    delay(250);  // XOSC/TCXO settle after reset
    feedLoopWDT();   // give begin()/ApplyRadioFsk() a fresh 5 s WDT window
    if (phy_fsk_) {
        ApplyRadioFsk();                 // re-init in GFSK at the GFSK params
        Unlock();
        return;
    }
    radio.begin(cfg.freq_mhz, BwFromCode(cfg.bw_code), cfg.sf, cfg.cr,
                cfg.sync, tx_power_, kPreamble, kTcxoV, false);
    feedLoopWDT();
    RadioCommonSetup(true);
    DeriveTiming();
    Unlock();
}

// Create the radio mutex + task and arm continuous RX. Call ONCE from setup()
// after the radio is configured. Higher priority than the loop task (so it
// preempts to read promptly); pinned to the same core as loop().
void Radio::StartTask() {
    radio_mutex_ = xSemaphoreCreateRecursiveMutex();
    // 8 KB stack: readData() drags in RadioLib's SPI path (4 KB overran).
    // Priority well above the loop (prompt preempt to drain the FIFO) but BELOW
    // the esp_timer/system tasks (~22) so it can't starve them.
    xTaskCreatePinnedToCore(Radio::RadioTask, "radio", 8192, nullptr, 18,
                            &radio_task_, 1);
    Lock();
    radio.startReceive();          // continuous RX; the task drains on DIO1
    rx_armed_ = true;
    Unlock();
}

const char* Radio::CurrentModeName() {
    int i = CurrentModeIndex();
    return i >= 0 ? kRfModes[i].name : "custom";
}

bool Radio::ApplyModeByName(const char* name) {
    return ApplyModeByIndex(ModeIndexByName(name));
}

int Radio::RfModeCount() {
    return (int)(sizeof(kRfModes) / sizeof(kRfModes[0]));
}

int Radio::ModeIndexByName(const char* name) {
    for (int i = 0; i < RfModeCount(); i++)
        if (!strcasecmp(name, kRfModes[i].name)) return i;
    return -1;
}

int Radio::CurrentModeIndex() {
    for (int i = 0; i < RfModeCount(); i++) {
        if (kRfModes[i].fsk) {                  // the GFSK preset
            if (phy_fsk_) return i;
            continue;
        }
        if (!phy_fsk_ && kRfModes[i].sf == cfg.sf &&
            kRfModes[i].bw_code == cfg.bw_code && kRfModes[i].cr == cfg.cr)
            return i;
    }
    return -1;
}

// Current LoRa mode's demod SNR floor (dB), rounded to an int, for the
// peer-SNR auto-power loop. kModeSnrFloor parallels the LoRa presets only
// (turbo..far); a GFSK or custom config has no entry, so return 0 — auto-power
// skips GFSK, so that value is never actually consumed.
int Radio::snr_floor() const {
    const int kFloorCount =
        (int)(sizeof(kModeSnrFloor) / sizeof(kModeSnrFloor[0]));
    int i = const_cast<Radio*>(this)->CurrentModeIndex();
    if (i < 0 || i >= kFloorCount) return 0;       // GFSK / unknown -> unused
    return (int)lroundf(kModeSnrFloor[i]);
}

// Next-faster LoRa rung's floor, for coordinated auto-power headroom under ADR
// (see link_layer::AutoPowerTargetFloor). The LoRa presets run fastest-first
// (turbo at index 0 .. far last), so the next-faster rung is index i-1; none
// exists at turbo (i == 0) or GFSK/custom -> false. Round UP (ceilf, not the
// lroundf snr_floor() uses): auto-power settles ~one margin above this target,
// and ADR climbs off the exact float floor, so rounding up keeps the held
// margin clear of the climb threshold.
bool Radio::next_faster_snr_floor(int& out_floor) const {
    const int kFloorCount =
        (int)(sizeof(kModeSnrFloor) / sizeof(kModeSnrFloor[0]));
    int i = const_cast<Radio*>(this)->CurrentModeIndex();
    if (i <= 0 || i >= kFloorCount) return false;  // no faster / non-LoRa
    out_floor = (int)ceilf(kModeSnrFloor[i - 1]);
    return true;
}

bool Radio::ApplyModeByIndex(int idx) {
    if (idx < 0 || idx >= RfModeCount()) return false;
    if (kRfModes[idx].fsk) {
        cfg.sf = cfg.bw_code = cfg.cr = 0;       // sentinel: GFSK (no SF/BW/CR)
        ApplyRadioFsk();
    } else {
        cfg.sf = kRfModes[idx].sf;
        cfg.bw_code = kRfModes[idx].bw_code;
        cfg.cr = kRfModes[idx].cr;
        ApplyMode(cfg.sf, cfg.bw_code, cfg.cr);
    }
    // Only update the per-mode timing — do NOT re-init the link. A mode change
    // must preserve the epoch/counter/ARQ/session so the switch is seamless (a
    // full re-init bumped the epoch, which made the peer re-run the session
    // handshake and revert the switch — notably breaking LoRa->GFSK).
    g_host.ApplyLinkTiming();
    return true;
}

// Build a portable AdrLadder (lib/linklayer/adr.h) from the live radio mode
// table. The ladder math + the whole ADR decision live in that header so they
// can be unit-tested in the native sim without a radio; this is the only place
// that knows the actual SF/BW/CR table, so it's the bridge between them. The
// arrays are static because the ladder keeps pointers into them.
void Radio::BuildAdrLadder(link_layer::AdrLadder* lad) {
    static float s_floor[sizeof(kRfModes) / sizeof(kRfModes[0])];
    static bool  s_fsk[sizeof(kRfModes) / sizeof(kRfModes[0])];
    static bool  s_auto[sizeof(kRfModes) / sizeof(kRfModes[0])];
    int n = RfModeCount();
    for (int i = 0; i < n; i++) {
        s_fsk[i] = kRfModes[i].fsk;
        // The SNR floor is only meaningful for LoRa presets (GFSK is never
        // SNR-picked, only opt-in gated), so park a dummy there.
        s_floor[i] = kRfModes[i].fsk ? 0.0f : kModeSnrFloor[i];
        // All LoRa presets are auto-eligible; the auto_ok mechanism stays in
        // case a preset ever needs to be made manual-only.
        s_auto[i] = true;
    }
    lad->count = n;
    lad->snr_floor = s_floor;
    lad->is_fsk = s_fsk;
    lad->auto_ok = s_auto;
    lad->margin_db = kAdrMarginDb;
}

// Receive one frame, waiting up to timeout_ms. The radio task drains the FIFO
// into the SPSC ring on each DIO1 RX_DONE; this just pops the ring and never
// polls/standbys the radio (the task owns RX), so the FIFO stays emptied even
// under a back-to-back burst. Returns RADIOLIB_ERR_NONE on a frame,
// RADIOLIB_ERR_RX_TIMEOUT on timeout. (RSSI tracking lives in the radio task.)
int16_t Radio::Rx(uint8_t* buf, size_t max_len, size_t& out_len,
                  uint32_t timeout_ms) {
    // Arm continuous RX if a prior TX left it disarmed; then drain the ring.
    if (!rx_armed_) {
        RMARK('r');  // trace: Rx startReceive
        Lock(); radio.startReceive(); rx_armed_ = true; Unlock();
    }
    uint32_t istart = millis();
    for (;;) {
        // Feed the task-WDT on EVERY pass, BEFORE the Pop. Two traps this
        // closes: (1) when the ring already holds a frame Pop returns at once,
        // so a burst of ready frames loops here (and in the caller) without
        // feeding; (2) on a long deaf wait the idle hook below feeds via Pet(),
        // but pairing NULLs that hook. Either way a > 5 s stretch would reboot
        // us. The wait is bounded by timeout_ms, so the WDT still catches a
        // true hang elsewhere.
        feedLoopWDT();
        size_t n = rx_ring_.Pop(buf, max_len);
        if (n) { out_len = n; return RADIOLIB_ERR_NONE; }
        if (millis() - istart > timeout_ms) return RADIOLIB_ERR_RX_TIMEOUT;
        if (g_rx_idle_hook) g_rx_idle_hook();
        delay(1);
    }
}

// radio.transmit() blocks for the whole time-on-air (~0.5s at SF8); during that
// the USB-CDC RX buffer would overflow and DROP host bytes. EMBEDDED/real-time:
// startTransmit + a serviced wait (calling the idle hook) keeps host I/O
// draining throughout, so no data is lost.
void Radio::Tx(const uint8_t* buf, size_t len) {
    // startTransmit() assumes the radio is in STANDBY (RadioLib's blocking
    // transmit() standbys first); with continuous RX we may still be in RX, so
    // standby explicitly or the TX is malformed and the peer hears nothing.
    Lock();                // own the radio for the whole TX (vs the radio task)
    radio.standby(); rx_armed_ = false;    // leave continuous RX before TX
    operation_done_ = false;
    RMARK('t');  // trace: startTransmit start
    if (radio.startTransmit((uint8_t*)buf, len) != RADIOLIB_ERR_NONE) {
        Unlock(); return;
    }
    // Safety deadline scaled to the frame's airtime (see kTxSafety* above) so a
    // slow mode (far/SF12 ~13 s) isn't aborted before it finishes transmitting.
    uint32_t tx_safety = toa_ms_ * kTxSafetyToaMul + kTxSafetyMarginMs;
    if (tx_safety < kTxSafetyFloorMs) tx_safety = kTxSafetyFloorMs;
    uint32_t start = millis();
    while (!operation_done_) {
        if (millis() - start > tx_safety) break;     // stuck-radio safety
        feedLoopWDT();   // keep the task-WDT fed even when the idle hook is off
        if (g_rx_idle_hook) g_rx_idle_hook();
        delay(1);
    }
    radio.finishTransmit();
    Unlock();
    LedBlink();   // per-FRAME activity: flickers at the real frame rate under
                  // load
    // GFSK ONLY: a tiny inter-frame gap so the peer finishes re-arming its
    // receiver before the next back-to-back frame (cuts GFSK's re-arm losses).
    // LoRa modes skip this entirely — phy_fsk_ is false for them.
    if (phy_fsk_ && kFskTxPaceMs) delay(kFskTxPaceMs);
}

#if defined(ESP32)
// EMBEDDED: the ISR must live in IRAM so it can run even while flash is busy;
// that's also why it does NOTHING but set a flag.
IRAM_ATTR
#endif
void Radio::OnDio1() {
    // Keep the ISR minimal and IRAM-safe: do NOT call flash-resident functions
    // (e.g. digitalWrite) here — under heavy interrupt load that crashes the
    // chip. The LED is driven from the main loop via LedBlink() instead.
    // Static member: reaches the singleton through g_radio to touch its
    // privates.
    g_radio.operation_done_ = true;   // Tx() watches this for TX-done
    // Wake the radio task to drain the FIFO promptly (ISR-safe). It checks
    // RX_DONE itself, so a TxDone edge just wakes it to a no-op.
    if (g_radio.radio_task_) {
        BaseType_t hpw = pdFALSE;
        vTaskNotifyGiveFromISR(g_radio.radio_task_, &hpw);
        portYIELD_FROM_ISR(hpw);
    }
}

// Set the TX power and push it to the SX1262. Centralizing this means external
// callers (AT+PWR, auto-power, proximity pairing) never touch the chip
// directly — they go through here so tx_power_ and the radio stay in sync.
void Radio::SetTxPower(int8_t dbm) {
    tx_power_ = dbm;
    // setOutputPower() drives the SX1262 over SPI, and so does the interrupt
    // RX task (RadioTask). Take the recursive radio mutex so auto-power's
    // periodic calls can't race the RX task on the bus and wedge the chip
    // (a missing lock here hung the loop -> task watchdog under load).
    Lock();
    RMARK('p');  // trace: setOutputPower start
    radio.setOutputPower(dbm);
    Unlock();
}

// Set the carrier frequency and push it to the SX1262. setFrequency() drives
// the radio over SPI, and so does the interrupt RX task, so take the recursive
// radio mutex to avoid racing it on the bus (the unlocked race wedged the chip
// under load — the same hazard SetTxPower guards against).
void Radio::SetFrequency(float mhz) {
    Lock();
    radio.setFrequency(mhz);
    Unlock();
}

// Set the LoRa sync word and push it to the SX1262. Locked vs the RX task for
// the same reason as SetFrequency above.
void Radio::SetSyncWord(uint8_t sync) {
    Lock();
    radio.setSyncWord(sync);
    Unlock();
}

// Lock/unlock all radio access. Recursive so nested radio calls (e.g.
// Reinit -> ApplyRadioFsk) don't self-deadlock. No-op before the mutex is
// created (setup()'s first radio config runs before StartTask()).
void Radio::Lock() {
    if (radio_mutex_) xSemaphoreTakeRecursive(radio_mutex_, portMAX_DELAY);
}
void Radio::Unlock() {
    if (radio_mutex_) xSemaphoreGiveRecursive(radio_mutex_);
}

// Re-derive all turn-taking timing + the BDP window from the current PHY's
// time-on-air, so every mode (LoRa fast/slow OR GFSK) self-tunes. Works for any
// modulation because getTimeOnAir() is PHY-aware. Writes the class's own timing
// members; the rest of the firmware reads them through the accessors.
void Radio::DeriveTiming() {
    uint32_t toa = radio.getTimeOnAir(kToaProbeBytes) / 1000;  // ms ~full frame
    if (toa < kToaFloorMs) toa = kToaFloorMs;
    // raw airtime; other budgets scale off it (e.g. the speedtest deadline).
    toa_ms_ = toa;
    interframe_ms_ = toa + kInterframeMargin;   // next back-to-back frame +
                                                // re-arm
    turn_rx_ms_     = toa * kTurnRxToaMul + kTurnRxMarginMs;   // per-frame cap
    listen_ms_     = toa + interframe_ms_ + kListenMarginMs;   // span a frame
    if (listen_ms_ < kListenFloorMs) listen_ms_ = kListenFloorMs;   // stay
                                                   // responsive on fast modes
    // BDP window: pick N so one full burst's airtime (N*ToA) dwarfs the fixed
    // turn-around dead-time, amortizing it. Big at fast modes (turbo -> 16),
    // small at slow ones (far -> 2) so a burst never outruns the retransmit
    // timer.
    uint32_t n = kAirtimeBudgetMs / (toa ? toa : 1);   // budgeted airtime per
                                                       // turn-around
    if (n < kWindowMin) n = kWindowMin;
    if (n > link_layer::MAXWIN) n = link_layer::MAXWIN;
    window_ = (uint8_t)n;
    // Retransmit timeout: window-AWARE (see kRetransSpareFrames). Computed
    // AFTER window_ because it scales with our burst size, so it tracks one
    // real round-trip at every mode instead of a fixed multiple of ToA.
    retransmit_ms_ = (window_ + kRetransSpareFrames) * toa
                     * kRetransSafetyPct / 100 + kRetransMarginMs;
}

// Re-apply the shared post-begin() radio setup (RF switch, IRQ, CRC) that any
// begin()/beginFSK() resets.
void Radio::RadioCommonSetup(bool lora_crc) {
    radio.setDio2AsRfSwitch(true);   // Wio-SX1262 RF switch on SX1262 DIO2
    if (lora_crc) radio.setCRC(true);
    radio.setDio1Action(Radio::OnDio1);
    // Bound the SPI BUSY-line wait (RadioLib defaults to 1000 ms). If the radio
    // wedges with BUSY stuck high, each SPI op would otherwise block a full
    // second: in the RX task that means holding the radio mutex ~3 s per pass
    // (readData + standby + startReceive), starving the loop task past its 5 s
    // task-watchdog BEFORE the loop's stuck-radio recovery can run — a reboot
    // that drops the transfer. Capping the wait makes a wedged op fail fast, so
    // the loop stays alive, sees the deaf radio, and Reinit()s it (cheap, non-
    // disruptive) instead of rebooting. The breadcrumb that exposed this read
    // wedgeop=d (a readData stuck on BUSY); see docs/CAPABILITIES_JOURNEY.md.
    radio_module.spiConfig.timeout = kSpiBusyTimeoutMs;
}

// Switch the SX1262 into GFSK modulation (the 'ludicrous' mode). The portable
// link layer rides on top unchanged — only the PHY differs.
void Radio::ApplyRadioFsk() {
    Lock();
    radio.standby();               // leave continuous-RX before re-init (see
    rx_armed_ = false;             //   ApplyMode: reconfigure from STANDBY)
    RMARK('f');  // trace: ApplyRadioFsk (GFSK) reconfigure start
    radio.beginFSK(cfg.freq_mhz, kFskBitrate, kFskFreqDev, kFskRxBw,
                   tx_power_, kFskPreamble, kTcxoV, false);
    RadioCommonSetup(false);
    uint8_t sync[2] = { 0x12, cfg.sync };   // private-link FSK sync word
    radio.setSyncWord(sync, 2);
    radio.setCRC(2);                         // 2-byte packet CRC
    phy_fsk_ = true;
    DeriveTiming();
    if (radio_task_) { radio.startReceive(); rx_armed_ = true; }   // re-arm RX
    Unlock();
}

// High-priority radio task: woken by the DIO1 ISR, it drains the SX1262 FIFO
// PROMPTLY into the SPSC ring (under the radio mutex), so back-to-back burst
// frames can't overrun the 256-byte FIFO. It does NOT touch g_link — the main
// loop pops the ring and runs OnRx, keeping link state single-threaded.
//
// It blocks on the notification with NO timeout (portMAX_DELAY) and re-arms RX
// only AFTER a completed RX_DONE — never on a timer. A periodic timed re-arm
// would fire MID-RECEPTION for any mode whose time-on-air exceeds it (slow/far
// frames are seconds on air), aborting the frame. Re-arm-after-RX_DONE is the
// proven Meshtastic/RNode shape and correct for every mode; a genuinely deaf
// radio is caught by the main-loop radio-stuck watchdog (MaybeReinitRadio).
//
// Static member (FreeRTOS `void(*)(void*)` entry); operates on the singleton
// through g_radio.
void Radio::RadioTask(void*) {
    uint32_t consec_fail = 0;   // consecutive failed reads -> storm detector
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // wait for a DIO1 event
        bool read_failed = false;   // RX_DONE fired but the read didn't succeed
        g_radio.Lock();
        if (radio.checkIrq(RADIOLIB_IRQ_RX_DONE) > 0) {   // real RX, not TxDone
            uint8_t fr[link_layer::MAXFRAME];
            size_t len = radio.getPacketLength();
            if (len > sizeof fr) len = sizeof fr;
            g_dbg_rx = 'd';  // breadcrumb: RX task is inside readData
            if (radio.readData(fr, len) == RADIOLIB_ERR_NONE) {  // clears IRQ
                float r = radio.getRSSI();
                g_radio.rssi_ema_ = g_radio.have_rssi_
                    ? (g_radio.rssi_ema_ * kRssiEmaAlpha
                       + r * (1.0f - kRssiEmaAlpha))
                    : r;
                g_radio.have_rssi_ = true;
                g_radio.last_snr_ = radio.getSNR();   // cache packet SNR under
                                                      // the lock
                g_radio.rx_ring_.Push(fr, len);   // dropped if full -> ARQ
                                                  // resends it
                consec_fail = 0;   // a real frame -> not a deaf-radio storm
            } else {
                read_failed = true;   // CRC fail / garbage / wedged radio
            }
            // Re-arm RX for the next frame (standby first to reset the AGC
            // between frames — cheap insurance against the desense errata).
            // Continuous RX for BOTH PHYs: draining + re-arming with no delay
            // is what catches GFSK's back-to-back burst frames (the proven
            // ludicrous-bulk path). Only when RX is meant to be active
            // (rx_armed_): during a TX, Tx() clears it and owns the radio under
            // this same mutex, so we must not stomp the TX. Runs AFTER a
            // completed RX_DONE, so it never aborts an in-flight frame.
            if (g_radio.rx_armed_) { radio.standby(); radio.startReceive(); }
        }
        g_radio.Unlock();
        g_dbg_rx = '.';   // RX task done handling (idle, not in readData)
        // Deaf-radio backstop ONLY — deliberately NOT a per-frame throttle.
        // The hot path above drains + re-arms with no delay: that is what lets
        // GFSK's back-to-back burst frames through (a per-RX_DONE yield here
        // gapped them and broke ludicrous). A genuinely wedged/deaf radio
        // instead fires RX_DONE whose reads ALL fail with no real frame in
        // between — a successful frame resets consec_fail, so ordinary GFSK
        // noise (which mostly passes CRC) never trips this. After enough
        // consecutive failures halt RX briefly for an uncontested window so the
        // loop's stuck-radio recovery can run, then re-arm. The loop stays fed
        // via its own Rx() pop path (feedLoopWDT there), not a yield here.
        if (read_failed && ++consec_fail >= kRxStormFails) {
            consec_fail = 0;
            g_radio.Lock(); radio.standby(); g_radio.Unlock();
            vTaskDelay(pdMS_TO_TICKS(kRxStormBackoffMs));   // loop's window
            g_radio.Lock();
            if (g_radio.rx_armed_) radio.startReceive();    // resume RX
            g_radio.Unlock();
        }
    }
}
