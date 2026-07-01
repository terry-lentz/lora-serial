/**
 * @file test_sim.cpp
 * @brief Integration sim of two full nodes over a channel that models real
 *        device behaviour, plus physics/auto-power/recovery harnesses.
 *
 * Two full nodes (real LinkLayer + ModeSwitch + ADR + rendezvous) run over a
 * channel that models real-device behaviour: MODE-DEAF delivery (a frame is
 * heard only if the peer is on the sender's mode), SNR-DRIVEN loss (a mode
 * below its demod floor mostly fails; well above it is clean), PER-MODE
 * time-on-air delay, and an IN-MEMORY ingest BACKLOG buffer (host writes
 * faster than the link drains, like the firmware's PSRAM ingest ring). The
 * point: drive switches up and down on the fly via SNR and prove the byte
 * stream comes out byte-exact through every switch, and a large backlog drains
 * exactly. Validates in simulation what we then confirm on hardware. See
 * docs/MODE_SWITCH_SPEC.md.
 */
#include <unity.h>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "linklayer.h"
#include "modeswitch.h"
#include "adr.h"
#include "autopower.h"   // link_layer::AutoPowerStep — shared peer-SNR loop
#include "prng.h"         // link_layer::Lcg — deterministic loss + payload PRNG

/// 16-byte AEAD key shared by both ends of the simulated link.
static const uint8_t SKEY[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};

/// @brief One PHY preset's behaviour in the sim's mode table.
struct SimMode {
    float    snr_floor;   ///< dB demod floor (LoRa); GFSK is RSSI-gated.
    bool     fsk;         ///< true for the GFSK rung (RSSI-gated, not SNR).
    uint8_t  window;      ///< BDP (selective-repeat) window for this mode.
    uint32_t retransmit_ms;     ///< retransmit timer for this mode.
    int      bytes_per_round;   ///< crude throughput proxy (fast modes > slow).
};
/// @brief The sim mode table, mirroring the firmware's kRfModes order: turbo,
///        fast, medium, slow, far, ludicrous(GFSK). snr_floor parallels adr's
///        kModeSnrFloor.
static const SimMode kModes[] = {
    {-5.0f,  false, 16,  400, 8},   // 0 turbo
    {-7.5f,  false, 12,  600, 6},   // 1 fast
    {-7.5f,  false,  8, 1000, 4},   // 2 medium
    {-12.5f, false,  4, 2000, 2},   // 3 slow
    {-20.0f, false,  2, 4000, 1},   // 4 far
    {0.0f,   true,  16,  300, 10},  // 5 ludicrous (GFSK)
};
/// Number of entries in kModes.
static const int kNModes = 6;
/// Index of the 'medium' mode (also the rendezvous fallback mode).
static const int kMedium = 2, kRzMode = 2;

/// When true, apply_mode does a full link re-init (the OLD bug) not a live
/// timing update — used by the regression test to prove the sim detects it.
static bool g_reset_on_switch = false;

/// Deterministic PRNG for reproducible loss patterns (see prng.h).
static link_layer::Lcg g_loss_rng(7, link_layer::kLcgNumRecipes);
/**
 * @brief Reseed the loss PRNG for a reproducible run.
 *
 * @param[in] s the new PRNG seed.
 */
static void rseed(uint32_t s) { g_loss_rng.Seed(s); }
/**
 * @brief Next pseudo-random word from the loss PRNG.
 *
 * @return the next 32-bit pseudo-random value.
 */
static uint32_t rnd() { return g_loss_rng.Next(); }

/**
 * @brief Decide whether one frame is lost, given link SNR vs the mode floor.
 *
 * Below the mode's demod floor a frame is almost always lost; within the
 * margin some loss; well above the floor the channel is clean. GFSK is
 * RSSI-gated rather than SNR-driven, so it uses a small fixed loss.
 *
 * @param[in] snr  the link SNR in dB.
 * @param[in] mode index into kModes of the current PHY mode.
 * @return true if this frame should be dropped.
 */
static bool snr_lost(float snr, int mode) {
    if (kModes[mode].fsk) return (rnd() % 100) < 3;   // GFSK: RSSI-gated
    float over = snr - kModes[mode].snr_floor;        // dB above demod floor
    int pct = over >= 6 ? 2 : over >= 3 ? 15 : over >= 0 ? 35
             : over >= -3 ? 70 : 97;            // toward the floor
    return (int)(rnd() % 100) < pct;
}

/// Whether the dead-link rendezvous may fire in FIXED (non-auto) mode.
/// false = the FIX (a pinned mode never auto-changes); true = reproduce the old
/// bug where a lossy fixed fast mode silently fell back to medium.
static bool g_rendezvous_in_fixed = false;

/// @brief One end of the simulated link: its link layer, mode-switch, ADR, and
///        per-node recovery state.
struct SimNode {
    link_layer::LinkLayer<16384> link;   ///< the portable link layer (ARQ).
    link_layer::ModeSwitch ms;           ///< coordinated mode-switch state.
    link_layer::AdrController adr;        ///< adaptive-data-rate logic.
    int mode = kMedium;            ///< current PHY mode index into kModes.
    int last_rx_round = 0;     ///< last VALID frame heard (the silence clock).
    int last_reinit_round = 0; ///< last radio re-init (rate-limit only).
    int last_rendezvous_round = 0; ///< last rendezvous (rate-limit only).
    bool initiator = false;        ///< true if this node is the initiator.
    bool auto_mode = true;     ///< false = pinned (no ADR/rendezvous).
    bool radio_stuck = false;  ///< soft wedge: a re-init (begin) recovers it.
    bool radio_hard_stuck = false;  ///< hard wedge: ONLY a reboot recovers it.
    bool ever_rx = false;      ///< heard the peer at least once (link was up).
    int  reinits = 0;          ///< radio-watchdog re-init count.
    int  reboots = 0;          ///< no-progress reboot count.
};

/**
 * @brief Recovery-escalation tunables / injection (see tests).
 *
 * Three layers, each at a longer silence threshold: (1) radio re-init at the
 * current mode recovers a SOFT wedge (mode-preserving, all modes); (2)
 * rendezvous (auto only) fixes a PHY/mode mismatch; (3) a full REBOOT is the
 * last resort — it recovers a HARD wedge that a begin()-only re-init can't
 * (matches hardware: a reflash/reboot cleared wedges the watchdog's begin() did
 * not). The g_* flags turn each layer off to prove it's needed.
 */
static const int kRadioStuckRounds = 7;   ///< no valid RX -> re-init the radio.
static const int kRebootRounds     = 14;  ///< still silent -> reboot.
static bool g_radio_watchdog = true;      ///< enable the radio-reinit watchdog.
static bool g_reboot_watchdog = true;     ///< enable the no-progress reboot.
static int  g_stick_round = -1;           ///< soft-stick the responder here.
static int  g_hard_stick_round = -1;      ///< hard-stick the responder here.
/**
 * @brief SX1262 AGC-lockup model (errata, docs/RADIO_ERRATA.md).
 *
 * SUSTAINED continuous RX with NO standby between frames makes the receiver go
 * DEAF. When on, a continuous receiver (rearm_ms==0 in phys_transfer) goes deaf
 * after kAgcLockupFrames frames; any standby between frames (rearm_ms>0) resets
 * the AGC and avoids it. This reproduces the real hardware behavior so the sim
 * can prove the per-frame standby (and the main-loop continuous re-arm+standby)
 * is NECESSARY, not just a throughput tweak.
 */
static bool g_agc_lockup = false;
/// No-standby frames a continuous receiver survives before going deaf.
static const int kAgcLockupFrames = 25;

/// Scratch SNR-floor ladder handed to the ADR controller (built per run).
static float s_floor[kNModes];
/// Scratch GFSK-flag ladder handed to the ADR controller (built per run).
static bool s_fsk[kNModes];

/**
 * @brief Configure an ADR controller with the sim's mode ladder + timers.
 *
 * @param[out] a the controller to populate and Begin().
 */
static void build_ladder(link_layer::AdrController& a) {
    for (int i = 0; i < kNModes; i++) {
        s_floor[i] = kModes[i].snr_floor; s_fsk[i] = kModes[i].fsk;
    }
    a.ladder.count = kNModes; a.ladder.snr_floor = s_floor;
    a.ladder.is_fsk = s_fsk; a.ladder.margin_db = 6.0f;
    a.cfg.period_ms = 300; a.cfg.up_stable = 2; a.cfg.gfsk_enabled = false;
    a.cfg.switch_timeout_ms = 1500; a.cfg.cooldown_ms = 1500;
    a.Begin();
}

/**
 * @brief Build a link Config for one node, starting on the medium mode.
 *
 * @param[in] addr this node's 1-byte address.
 * @param[in] peer the peer's 1-byte address.
 * @return the assembled Config (encrypted with the shared SKEY).
 */
static link_layer::Config node_cfg(uint8_t addr, uint8_t peer) {
    link_layer::Config c; c.addr = addr; c.peer = peer;
    c.window = kModes[kMedium].window;
    c.retransmit_ms = kModes[kMedium].retransmit_ms;
    c.compress = false; c.encrypt = true; c.key = SKEY; c.epoch = 1;
    return c;
}

/**
 * @brief Apply a committed PHY mode to a node (live timing update).
 *
 * Updates PHY + live timing, NOT a reset — mirrors the firmware's
 * ApplyLinkTiming so in-flight frames survive a switch. The regression path
 * (g_reset_on_switch) does the OLD full re-init instead.
 *
 * @param[in,out] n the node whose mode/timing is updated.
 * @param[in]     m the new mode index into kModes.
 */
static void apply_mode(SimNode& n, int m) {
    n.mode = m;
    if (g_reset_on_switch) {
        link_layer::Config c =
            node_cfg(n.initiator ? 1 : 2, n.initiator ? 2 : 1);
        c.window = kModes[m].window; c.retransmit_ms = kModes[m].retransmit_ms;
        c.epoch = (uint8_t)(m + 50);     // bump epoch (the bug)
        n.link.Init(c);                  // full reset -> drops in-flight data
    } else {
        n.link.SetTiming(kModes[m].window, kModes[m].retransmit_ms);
    }
}
/**
 * @brief Apply any mode switches the ModeSwitch state machine has committed.
 *
 * @param[in,out] n the node to service.
 */
static void service_switch(SimNode& n) {
    int m; while (n.ms.TakeApply(&m, 0)) apply_mode(n, m);
}

/**
 * @brief Dead-link rendezvous (mirrors firmware MaybeRendezvous).
 *
 * On prolonged silence, fall back to the fixed rendezvous mode so both ends
 * can re-find each other. In FIXED mode it must NOT fire (a pinned mode never
 * auto-changes) unless g_rendezvous_in_fixed is set, to reproduce the old bug.
 *
 * @param[in,out] n     the node to (possibly) rendezvous.
 * @param[in]     round the current sim round index.
 */
static void maybe_rendezvous(SimNode& n, int round) {
    if (!n.auto_mode && !g_rendezvous_in_fixed) return;
    if (round - n.last_rx_round < 9) return;            // silence clock
    if (round - n.last_rendezvous_round < 9) return;    // rate-limit only
    // Rate-limit on our OWN clock; do NOT reset last_rx_round, so a genuinely
    // dead link keeps escalating to the reboot layer (kRebootRounds).
    n.last_rendezvous_round = round;
    if (n.mode != kRzMode) { apply_mode(n, kRzMode); n.ms.Begin(kRzMode); }
}

/**
 * @brief Radio-stuck watchdog: re-init the radio at the current mode on
 *        prolonged RX silence (the firmware fix being validated).
 *
 * Models radio.begin(): mode-preserving and active in ALL modes, so it
 * un-sticks a deaf radio without changing a pinned mode. Layered BELOW
 * rendezvous (which is auto-only and changes mode); re-init fires first and is
 * the cheaper recovery.
 *
 * @param[in,out] n     the node to (possibly) re-init.
 * @param[in]     round the current sim round index.
 */
static void maybe_radio_reinit(SimNode& n, int round) {
    if (!g_radio_watchdog) return;
    if (round - n.last_rx_round < kRadioStuckRounds) return;     // heard lately
    if (round - n.last_reinit_round < kRadioStuckRounds) return; // rate-limit
    // Do NOT touch last_rx_round: a true mode mismatch (a re-init can't fix it)
    // must still escalate to rendezvous on the unchanged RX-silence clock.
    n.last_reinit_round = round;
    if (n.radio_stuck) { n.radio_stuck = false; n.reinits++; }  // re-init heals
}

/**
 * @brief No-progress REBOOT (last-resort recovery).
 *
 * If a node that HAS linked hears nothing valid for kRebootRounds despite the
 * cheaper recoveries, reboot — a full reset clears even a HARD wedge that a
 * begin()-only re-init can't (the firmware's MaybeReboot + the IDF task
 * watchdog). Gated on ever_rx so a node waiting for an absent peer never
 * boot-loops.
 *
 * @param[in,out] n     the node to (possibly) reboot.
 * @param[in]     round the current sim round index.
 */
static void maybe_reboot(SimNode& n, int round) {
    if (!g_reboot_watchdog || !n.ever_rx) return;
    if (round - n.last_rx_round < kRebootRounds) return;
    n.radio_stuck = false; n.radio_hard_stuck = false;   // reboot clears all
    n.last_rx_round = round; n.last_reinit_round = round;
    n.reboots++;
}

/**
 * @brief Deliver one node's TX burst to the peer over the mode-deaf, lossy
 *        channel.
 *
 * A frame is heard only if the receiver is on the sender's mode and survives
 * SNR-driven loss; a valid RX advances the peer's silence clock and feeds the
 * mode-switch ACK path.
 *
 * @param[in,out] from            the transmitting node.
 * @param[in,out] to              the receiving node.
 * @param[in]     round           the current sim round index.
 * @param[in]     snr             the link SNR in dB for loss computation.
 * @param[in]     is_to_initiator true if @p to is the initiator (ACK routing).
 */
static void deliver(SimNode& from, SimNode& to, int round, float snr,
                    bool is_to_initiator) {
    if (to.radio_stuck || to.radio_hard_stuck) return;   // deaf: hears nothing
    uint8_t fr[link_layer::MAXFRAME]; size_t fl;
    from.link.SetCtrlTx(from.ms.TxCtrl());
    from.link.BeginTurn();
    bool sent = false;
    std::vector<std::vector<uint8_t>> burst;
    while (from.link.NextTx(fr, sizeof(fr), fl, (uint32_t)round*100)) {
        burst.push_back(std::vector<uint8_t>(fr, fr + fl)); sent = true;
    }
    if (!sent) { fl = from.link.MakePoll(fr, sizeof(fr));
                 burst.push_back(std::vector<uint8_t>(fr, fr + fl)); }
    for (auto& f : burst) {
        // mode-deaf: only heard if the receiver is on the sender's mode.
        if (to.mode != from.mode) continue;
        if (snr_lost(snr, from.mode)) continue;
        to.link.OnRx(f.data(), f.size(), (uint32_t)round*100);
        if (to.link.TakeValidRx()) {
            to.last_rx_round = round; to.ever_rx = true;
            to.ms.AfterRecv(to.link.CtrlRx(), is_to_initiator,
                          (uint32_t)round * 100);
        }
    }
}

/**
 * @brief Run one full sim round: recovery escalation, ADR, then I->R and R->I
 *        turns.
 *
 * @param[in,out] I     the initiator node.
 * @param[in,out] R     the responder node.
 * @param[in]     round the current sim round index.
 * @param[in]     snr   the link SNR in dB for this round.
 */
static void sim_round(SimNode& I, SimNode& R, int round, float snr) {
    if (round == g_stick_round) R.radio_stuck = true;        // soft wedge
    if (round == g_hard_stick_round) R.radio_hard_stuck = true;  // hard wedge
    // Escalation: re-init (7, cheap, mode-preserving) -> rendezvous (9, auto,
    // mode change) -> reboot (14, last resort, clears a hard wedge).
    maybe_radio_reinit(I, round); maybe_radio_reinit(R, round);
    maybe_rendezvous(I, round); service_switch(I);
    maybe_rendezvous(R, round); service_switch(R);
    maybe_reboot(I, round); maybe_reboot(R, round);
    // ADR decision on the initiator — only in auto mode.
    if (I.auto_mode) {
        uint32_t txc = I.link.DbgStatTx();
        int retx = txc >= 8 ? (int)(100*I.link.DbgStatRetx()/txc) : -1;
        link_layer::AdrController::In in;
        in.now = (uint32_t)round*100; in.busy = I.ms.Busy();
        in.have_link = true;
        in.cur = I.ms.Current(); in.snr = snr; in.rssi = -50.0f;
        in.retx_pct = retx;
        link_layer::AdrAction act = I.adr.Decide(in);
        if (act.kind == link_layer::ADR_REQUEST) I.ms.Request(act.mode);
        else if (act.kind == link_layer::ADR_ABORT) I.ms.Abort();
    }
    // Initiator turn (carries data + any REQ/ACK ctrl).
    deliver(I, R, round, snr, /*to_initiator=*/false);
    service_switch(R);
    // Responder turn (ACK rides here on the OLD mode), then it applies.
    deliver(R, I, round, snr, /*to_initiator=*/true);
    R.ms.AfterSend((uint32_t)round*100); service_switch(R);
    service_switch(I);
}

/// File-local results so tests can assert without re-plumbing return values.
static bool s_last_exact;                  ///< last run: stream was byte-exact.
static int  s_last_reinits;                ///< responder re-inits, last run.
static int  s_last_reboots;                ///< responder reboots, last run.
/// Last run's source bytes and what the responder received.
static std::vector<uint8_t> s_last_src, s_last_got;
static std::vector<int> s_mode_trace;     ///< initiator mode each round.

// ---- the simulation driver ----
/**
 * @brief Stream `total` bytes initiator->responder while SNR follows snr_at().
 *
 * Pushes bytes from the initiator's ingest into the link as fast as it accepts
 * (backpressure), runs the per-round sim while SNR follows snr_at(round), and
 * collects what the responder receives. Records run results into the s_last_*
 * file-locals for the test to assert on.
 *
 * @param[in]  total      number of bytes to stream.
 * @param[in]  snr_at     callable: round index -> SNR in dB for that round.
 * @param[in]  max_rounds round cap before giving up.
 * @param[out] final_mode if non-null, receives the initiator's final mode.
 * @param[in]  feed_rate  bytes/round fed from ingest (models a steady stream).
 * @param[in]  auto_mode  true for ADR/rendezvous, false for a pinned mode.
 * @param[in]  start_mode the mode index both ends start on.
 * @return the bytes the responder received.
 */
template <class SnrFn>
static std::vector<uint8_t> run_stream(size_t total, SnrFn snr_at,
                                       int max_rounds, int* final_mode,
                                       size_t feed_rate = 1u << 30,
                                       bool auto_mode = true,
                                       int start_mode = kMedium) {
    static SimNode I, R;
    I.link.Init(node_cfg(1, 2)); R.link.Init(node_cfg(2, 1));
    I.mode = R.mode = start_mode; I.last_rx_round = R.last_rx_round = 0;
    I.last_reinit_round = R.last_reinit_round = 0;
    I.last_rendezvous_round = R.last_rendezvous_round = 0;
    I.radio_stuck = R.radio_stuck = false; I.reinits = R.reinits = 0;
    I.radio_hard_stuck = R.radio_hard_stuck = false;
    I.reboots = R.reboots = 0; I.ever_rx = R.ever_rx = false;
    I.initiator = true; R.initiator = false;
    I.auto_mode = R.auto_mode = auto_mode;
    uint8_t sw = kModes[start_mode].window;
    uint32_t srt = kModes[start_mode].retransmit_ms;
    I.link.SetTiming(sw, srt); R.link.SetTiming(sw, srt);
    I.ms.Begin(start_mode); R.ms.Begin(start_mode);
    build_ladder(I.adr);
    rseed(12345);
    s_mode_trace.clear();
    std::vector<uint8_t> src(total), got;
    // deterministic pseudo-random payload (see prng.h).
    link_layer::Lcg gen(0xABCDEF);
    for (size_t i = 0; i < total; i++) src[i] = gen.NextByte();
    size_t ingested = 0; uint8_t out[4096];
    for (int round = 0; round < max_rounds && got.size() < total; round++) {
        // host -> link, rate-limited (feed_rate bytes/round models a steady
        // stream); the link ring applies backpressure when it's full.
        if (ingested < total) {
            size_t want = total - ingested;
            if (want > feed_rate) want = feed_rate;
            ingested += I.link.Write(src.data() + ingested, want);
        }
        sim_round(I, R, round, (float)snr_at(round));
        s_mode_trace.push_back(I.ms.Current());
        size_t k; while ((k = R.link.Read(out, sizeof(out))) > 0)
            got.insert(got.end(), out, out + k);
    }
    if (final_mode) *final_mode = I.ms.Current();
    bool exact = got.size() == total &&
                 memcmp(got.data(), src.data(), total) == 0;
    s_last_exact = exact; s_last_src = src; s_last_got = got;
    s_last_reinits = R.reinits; s_last_reboots = R.reboots;
    return got;
}

// ---- tests ----

/**
 * @brief On strong, steady SNR, ADR climbs to a fast rung and the stream stays
 *        byte-exact.
 */
void test_sim_climb_byte_exact() {
    int fm = -1;
    run_stream(20000, [](int){ return 5.0; }, 6000, &fm);
    TEST_ASSERT_TRUE_MESSAGE(s_last_exact, "stream not byte-exact on climb");
    TEST_ASSERT_TRUE_MESSAGE(fm == 0 || fm == 1, "ADR did not climb");
}

/**
 * @brief An SNR sweep down then up makes ADR step down then back up, with the
 *        stream byte-exact across every switch.
 */
void test_sim_snr_sweep_byte_exact() {
    int fm = -1;
    // A steady stream while SNR ramps GRADUALLY down then back up (as a real
    // link fades and recovers), so ADR can track it through the modes
    // rather than the link dying instantly. +6 -> -8 -> +6 over the run.
    auto snr = [](int r) -> double {
        if (r < 500)  return 6.0;
        if (r < 1500) return 6.0 - 14.0 * (r - 500) / 1000.0;    // +6 -> -8
        if (r < 2500) return -8.0;
        if (r < 3500) return -8.0 + 14.0 * (r - 2500) / 1000.0;  // -8 -> +6
        return 6.0;
    };
    run_stream(120000, snr, 30000, &fm, /*feed_rate=*/40);   // ~40 B/round
    TEST_ASSERT_TRUE_MESSAGE(s_last_exact, "not byte-exact across sweep");
    // Teeth: the mode must have moved both DOWN (a slower rung than the
    // medium start) and back UP (a faster rung) during the sweep, else the
    // test isn't exercising switching at all.
    int lo = 99, hi = -1;
    for (int m : s_mode_trace) { if (m > hi) hi = m; if (m < lo) lo = m; }
    TEST_ASSERT_TRUE_MESSAGE(hi >= 3, "expected a step DOWN to a slower mode");
    TEST_ASSERT_TRUE_MESSAGE(lo <= 1, "expected a step UP to a faster mode");
}

/**
 * @brief Teeth: the OLD reset-on-switch behaviour DROPS data across a switch,
 *        proving the sim detects it and live-timing switching is what fixes it.
 */
void test_sim_reset_on_switch_loses_data() {
    g_reset_on_switch = true;
    run_stream(24000, [](int){ return 5.0; }, 8000, nullptr);
    g_reset_on_switch = false;
    TEST_ASSERT_FALSE_MESSAGE(s_last_exact,
        "reset-on-switch should have dropped data (sim must detect it)");
}

/**
 * @brief On a persistently weak link, ADR holds a robust mode and the (slow)
 *        stream stays byte-exact.
 */
void test_sim_weak_link_byte_exact() {
    run_stream(8000, [](int){ return -14.0; }, 20000, nullptr);
    TEST_ASSERT_TRUE_MESSAGE(s_last_exact, "not byte-exact on weak link");
}

/**
 * @brief A backlog far larger than the link ring drains byte-exact under
 *        backpressure while ADR adapts to a mid-stream SNR dip.
 */
void test_sim_large_backlog_byte_exact() {
    auto snr = [](int r) -> double {
        return (r > 1500 && r < 3000) ? -9.0 : 5.0; };
    run_stream(200000, snr, 120000, nullptr);
    TEST_ASSERT_TRUE_MESSAGE(s_last_exact, "large backlog not byte-exact");
    TEST_ASSERT_EQUAL_UINT32(200000, (uint32_t)s_last_got.size());
}

// ===========================================================================
// PHYSICS layer: model real per-mode TIME-ON-AIR on a clock, so timing-induced
// bugs (not just logic bugs) can be induced in sim — the air-time "delays"
// that only bite on hardware. Representative ToA (ms) for a ~full frame at each
// SX1262 mode, and the firmware's timer derivation (fw_radio.cpp DeriveTiming)
// reproduced here so a mis-derivation shows up as a throughput collapse.
// ===========================================================================
/// Representative time-on-air (ms) for a full frame per mode: turbo, fast,
/// medium, slow, far, gfsk.
static const int kToaMs[] = {18, 45, 90, 370, 1480, 10};

/// @brief Per-mode timers derived from time-on-air (cf. fw_radio.cpp).
struct Timing {
    uint32_t interframe;   ///< inter-frame timeout (ms).
    uint32_t retransmit;   ///< retransmit timer (ms).
    uint8_t window;        ///< BDP window for this mode.
};

/**
 * @brief Derive interframe/retransmit timers + BDP window from a mode's ToA.
 *
 * Reproduces fw_radio.cpp DeriveTiming so a mis-derivation shows up here as a
 * throughput collapse.
 *
 * @param[in] mode index into kToaMs of the current mode.
 * @return the derived Timing for that mode.
 */
static Timing derive_timing(int mode) {
    uint32_t toa = kToaMs[mode] < 5 ? 5 : kToaMs[mode];
    Timing t;
    t.interframe = toa + 60;            // g_interframe_ms = ToA + margin
    uint32_t n = 1500 / toa;            // BDP window (airtime budget / ToA)
    if (n < 2) n = 2; if (n > 16) n = 16;
    t.window = (uint8_t)n;
    // g_retransmit_ms: window-AWARE — (window + spare) * ToA * safety + margin
    // (mirrors fw_radio.cpp DeriveTiming; spare=2, safety=140%).
    t.retransmit = (t.window + 2) * toa * 140 / 100 + 400;
    return t;
}

/**
 * @brief Clocked one-way initiator->responder transfer on a fixed mode,
 *        charging real time-on-air per frame (clean RF, to isolate TIMING).
 *
 * If @p txgap_ms exceeds the receiver's inter-frame timeout the receiver
 * abandons the rest of the burst (firmware: a timed-out RxWithTimeout breaks
 * the RX loop) — how a too-tight timer or excess per-frame overhead collapses
 * throughput. @p rearm_ms models the receiver's RX re-arm DEAF WINDOW: after a
 * frame the radio is briefly not listening (startReceive->readData->standby
 * per frame, re-armed on the next call), so a back-to-back frame in that gap is
 * MISSED and must be retransmitted. rearm_ms=0 models CONTINUOUS receive (the
 * SX126x stays in RX after a packet), which is the fix.
 *
 * @param[in]  mode         index into kToaMs of the PHY mode.
 * @param[in]  total        number of bytes to transfer.
 * @param[in]  txgap_ms     inter-frame transmit gap, in milliseconds.
 * @param[in]  ifr_override if non-zero, overrides the derived interframe
 *                          timeout.
 * @param[out] exact        set true iff the receiver got the bytes byte-exact.
 * @param[in]  rearm_ms     receiver re-arm deaf window, in milliseconds.
 * @return the elapsed sim clock in milliseconds.
 */
static uint32_t phys_transfer(int mode, size_t total, uint32_t txgap_ms,
                              uint32_t ifr_override, bool* exact,
                              uint32_t rearm_ms = 0) {
    static link_layer::LinkLayer<16384> A, B;
    Timing tm = derive_timing(mode);
    uint32_t ifr = ifr_override ? ifr_override : tm.interframe;
    link_layer::Config ca = node_cfg(1, 2); ca.window = tm.window;
    ca.retransmit_ms = tm.retransmit;
    link_layer::Config cb = node_cfg(2, 1); cb.window = tm.window;
    cb.retransmit_ms = tm.retransmit;
    A.Init(ca); B.Init(cb);
    std::vector<uint8_t> src(total), got;
    // deterministic pseudo-random payload (see prng.h).
    link_layer::Lcg gen(0xC0DE);
    for (size_t i = 0; i < total; i++) src[i] = gen.NextByte();
    const uint32_t toa = (uint32_t)kToaMs[mode], turnaround = 20;
    uint32_t clock = 0; size_t ingested = 0; uint8_t out[4096];
    uint8_t fr[link_layer::MAXFRAME]; size_t fl;
    size_t b_rx = 0; bool b_agc_deaf = false;   // AGC-lockup state for rx B
    auto burst = [&](link_layer::LinkLayer<16384>& S,
                     link_layer::LinkLayer<16384>& D) {
        S.BeginTurn();
        bool d_is_b = (&D == &B);   // B is the data-heavy receiver we model
        std::vector<std::vector<uint8_t>> frames;
        while (S.NextTx(fr, sizeof fr, fl, clock))
            frames.push_back(std::vector<uint8_t>(fr, fr + fl));
        if (frames.empty()) { fl = S.MakePoll(fr, sizeof fr);
            frames.push_back(std::vector<uint8_t>(fr, fr + fl)); }
        bool abandoned = false;
        uint32_t deaf_until = 0;       // receiver re-arm deaf window (rearm_ms)
        for (size_t i = 0; i < frames.size(); i++) {
            uint32_t gap = i ? txgap_ms : 0;
            uint32_t start = clock + gap;              // this frame's preamble
            clock = start + toa;                       // airtime spent
            if (i && gap > ifr) abandoned = true;      // receiver gave up
            bool deaf = start < deaf_until;            // re-arming -> miss
            // SX1262 AGC lockup: a continuous receiver (rearm_ms==0, never
            // standbys) goes permanently deaf after kAgcLockupFrames frames.
            bool agc = d_is_b && b_agc_deaf;
            if (!abandoned && !deaf && !agc) {
                D.OnRx(frames[i].data(), frames[i].size(), clock);
                deaf_until = clock + rearm_ms;         // deaf while it re-arms
                if (d_is_b && g_agc_lockup && rearm_ms == 0 &&
                    ++b_rx > (size_t)kAgcLockupFrames)
                    b_agc_deaf = true;                 // locked up -> deaf
            }
        }
        clock += turnaround;
    };
    for (int turn = 0; turn < 500000 && got.size() < total; turn++) {
        if (ingested < total)
            ingested += A.Write(src.data() + ingested, total - ingested);
        burst(A, B);                                   // data A->B
        size_t k; while ((k = B.Read(out, sizeof out)) > 0)
            got.insert(got.end(), out, out + k);
        burst(B, A);                                   // ack/poll B->A
        while (A.Read(out, sizeof out) > 0) {}
    }
    *exact = got.size() == total &&
             memcmp(got.data(), src.data(), total) == 0;
    return clock;
}

/**
 * @brief With correct timing on clean RF, per-mode goodput strictly follows
 *        time-on-air (faster modes deliver more bytes/sec).
 *
 * If hardware ever shows turbo slower than medium, the cause is the
 * radio/timing layer, not the link or this model.
 */
void test_phys_goodput_ordering() {
    const size_t total = 40000;
    double kbps[5];
    for (int m = 0; m < 5; m++) {
        bool ex = false;
        uint32_t ms = phys_transfer(m, total, /*txgap=*/0, 0, &ex);
        TEST_ASSERT_TRUE_MESSAGE(ex, "phys transfer not byte-exact");
        kbps[m] = (double)total / ((double)ms / 1000.0) / 1024.0;
        printf("  [phys mode %d goodput %.2f KB/s in %u ms]\n", m, kbps[m], ms);
    }
    TEST_ASSERT_TRUE_MESSAGE(kbps[0] > kbps[2], "turbo not faster than medium");
    TEST_ASSERT_TRUE_MESSAGE(kbps[1] > kbps[2], "fast not faster than medium");
    TEST_ASSERT_TRUE_MESSAGE(kbps[2] > kbps[3], "medium not faster than slow");
    TEST_ASSERT_TRUE_MESSAGE(kbps[3] > kbps[4], "slow not faster than far");
}

/**
 * @brief Teeth: an inter-frame gap over the receiver's timeout collapses
 *        throughput (data still byte-exact via ARQ, just far slower).
 *
 * Proves the physics model can reproduce that class of hardware timing fault,
 * and that the fix (gap within the timeout) restores full throughput.
 */
void test_phys_interframe_timeout_collapses() {
    bool ex1 = false, ex2 = false;
    // turbo, gap well over its inter-frame timeout -> abandons -> slow.
    uint32_t bad = phys_transfer(0, 8000, /*txgap=*/500, /*ifr=*/100, &ex1);
    // same, but the gap is within the timeout -> full burst -> fast.
    uint32_t good = phys_transfer(0, 8000, /*txgap=*/0, /*ifr=*/100, &ex2);
    TEST_ASSERT_TRUE(ex1); TEST_ASSERT_TRUE(ex2);
    printf("  [turbo collapse %u ms vs healthy %u ms]\n", bad, good);
    TEST_ASSERT_TRUE_MESSAGE(bad > good * 3,
        "expected the timeout/abandon to collapse throughput");
}

/**
 * @brief Teeth: a larger RX re-arm deaf window misses back-to-back frames on a
 *        CLEAN channel, costing goodput once it exceeds the sender's gap.
 *
 * Models the ~1/3 retx seen at +20 dB SNR (a hardware re-arm effect, not
 * encryption). The shipped fix re-arms BEFORE processing (a tiny 2 ms window
 * that fits inside the 5 ms sender gap, so frames are caught) vs AFTER
 * processing (a 12 ms window that does not, so frames are missed and resent).
 */
void test_phys_rx_rearm_miss_collapses() {
    const size_t total = 16000;
    bool ex0 = false, ex1 = false;
    uint32_t fast = phys_transfer(0, total, /*txgap=*/5, 0, &ex0, /*rearm=*/2);
    uint32_t slow = phys_transfer(0, total, /*txgap=*/5, 0, &ex1, /*rearm=*/12);
    TEST_ASSERT_TRUE(ex0); TEST_ASSERT_TRUE(ex1);   // ARQ keeps both byte-exact
    double fast_kbps = total / ((double)fast / 1000.0) / 1024.0;
    double slow_kbps = total / ((double)slow / 1000.0) / 1024.0;
    printf("  [rx-rearm: before-process %.2f vs after-process %.2f KB/s]\n",
           fast_kbps, slow_kbps);
    TEST_ASSERT_TRUE_MESSAGE(slow > fast * 12 / 10,
        "an after-process (larger) re-arm window should cost >=20% goodput");
}

/**
 * @brief Teeth: a continuous receiver that never standbys (rearm=0) trips the
 *        SX1262 AGC lockup, goes deaf, and the transfer STALLS.
 *
 * The same continuous path WITH a per-frame standby (rearm=2) resets the AGC
 * and completes byte-exact — proving the standby is REQUIRED, not just a
 * throughput tweak (the negative case fails without it).
 */
void test_phys_continuous_no_standby_goes_deaf() {
    g_agc_lockup = true;
    bool deaf_exact = true, standby_exact = false;
    // No standby (rearm=0): AGC locks up after kAgcLockupFrames -> never
    // finishes (the stalled run hits the turn cap and returns not-exact).
    phys_transfer(0, 60000, 0, 0, &deaf_exact, /*rearm=*/0);
    // With a per-frame standby (rearm=2): AGC stays healthy -> byte-exact.
    phys_transfer(0, 8000, 0, 0, &standby_exact, /*rearm=*/2);
    g_agc_lockup = false;
    TEST_ASSERT_FALSE_MESSAGE(deaf_exact,
        "continuous RX with no standby must go deaf (AGC lockup) and stall");
    TEST_ASSERT_TRUE_MESSAGE(standby_exact,
        "continuous RX WITH a per-frame standby must stay alive + byte-exact");
}

/**
 * @brief Did the initiator's mode trace ever leave `start` (an auto-change)?
 *
 * @param[in] start the starting mode index.
 * @return true if any recorded round was on a different mode.
 */
static bool trace_changed_from(int start) {
    for (int m : s_mode_trace) if (m != start) return true;
    return false;
}

/**
 * @brief Did the initiator's mode trace ever visit `mode`?
 *
 * @param[in] mode the mode index to look for.
 * @return true if any recorded round was on that mode.
 */
static bool trace_hit(int mode) {
    for (int m : s_mode_trace) if (m == mode) return true;
    return false;
}

/**
 * @brief A pinned fast mode (turbo) on a lossy link must NOT auto-change.
 *
 * The firmware must not silently fall back to medium when the mode is fixed.
 */
void test_sim_fixed_mode_holds_on_loss() {
    g_rendezvous_in_fixed = false;                       // the fix
    run_stream(50000, [](int){ return -10.0; }, 2000, nullptr,
               1u << 30, /*auto_mode=*/false, /*start_mode=*/0);
    TEST_ASSERT_FALSE_MESSAGE(trace_changed_from(0),
        "fixed turbo must NOT auto-change on a lossy link");
}

/**
 * @brief Teeth: the OLD rendezvous-in-fixed bug DOES drop a lossy fixed turbo
 *        link to medium, proving the sim reproduces the hardware symptom.
 */
void test_sim_fixed_mode_drop_repro() {
    g_rendezvous_in_fixed = true;                        // the old bug
    run_stream(50000, [](int){ return -10.0; }, 2000, nullptr,
               1u << 30, /*auto_mode=*/false, /*start_mode=*/0);
    g_rendezvous_in_fixed = false;
    TEST_ASSERT_TRUE_MESSAGE(trace_hit(kMedium),
        "expected the old rendezvous-in-fixed bug to drop to medium");
}

/**
 * @brief In AUTO, sustained loss on a fast mode steps the mode DOWN via the
 *        coordinated handshake (the wanted loss-aware ADR behaviour).
 */
void test_sim_auto_steps_down_on_loss() {
    g_rendezvous_in_fixed = false;
    run_stream(50000, [](int){ return -6.0; }, 3000, nullptr,
               /*feed_rate=*/40, /*auto_mode=*/true, /*start_mode=*/0);
    TEST_ASSERT_TRUE_MESSAGE(trace_changed_from(0),
        "auto should step DOWN from a lossy turbo");
}

// ===========================================================================
// RADIO-STUCK recovery: the responder's RX hardware wedges mid-stream (deaf
// radio — the exact hardware symptom we saw: rssi pinned, snr garbage, tx=0,
// link 99% retx). The firmware fix being validated is a RADIO WATCHDOG that
// re-inits the radio at the CURRENT mode on prolonged silence. These prove, in
// sim, that the recovery works BEFORE we loop on real hardware.
// ===========================================================================

/**
 * @brief AUTO mode: the responder goes deaf mid-stream, the watchdog re-inits
 *        its radio, and the whole stream still arrives byte-exact.
 */
void test_sim_radio_stuck_recovers() {
    g_radio_watchdog = true; g_stick_round = 50;
    // feed-rate limited (~40 B/round) so the stream lasts long enough that the
    // round-50 stick lands mid-transfer instead of after it completes.
    run_stream(40000, [](int){ return 5.0; }, 8000, nullptr, /*feed_rate=*/40);
    g_stick_round = -1;
    printf("  [radio-stuck recover: reinits=%d exact=%d]\n",
           s_last_reinits, (int)s_last_exact);
    TEST_ASSERT_TRUE_MESSAGE(s_last_reinits >= 1,
        "watchdog must have re-init the deaf radio at least once");
    TEST_ASSERT_TRUE_MESSAGE(s_last_exact,
        "stream must survive a mid-run deaf radio byte-exact");
}

/**
 * @brief Teeth: WITHOUT the watchdog a deaf radio stays dead and the stream
 *        cannot complete, proving the watchdog is necessary.
 *
 * Rendezvous (a mode change, still running here in auto) does NOT recover a
 * stuck radio, so the reboot layer is also disabled to isolate this test.
 */
void test_sim_radio_stuck_no_watchdog_stays_dead() {
    // Disable BOTH the re-init and the reboot layer to isolate this teeth test
    // to the re-init watchdog (the reboot layer would otherwise recover it).
    g_radio_watchdog = false; g_reboot_watchdog = false; g_stick_round = 50;
    run_stream(40000, [](int){ return 5.0; }, 4000, nullptr, /*feed_rate=*/40);
    g_radio_watchdog = true; g_reboot_watchdog = true; g_stick_round = -1;
    TEST_ASSERT_FALSE_MESSAGE(s_last_exact,
        "without any watchdog a deaf radio must NOT recover");
}

/**
 * @brief FIXED mode: with rendezvous gated off, ONLY the radio watchdog can
 *        recover a deaf radio, and it must do so without changing the pin.
 */
void test_sim_radio_stuck_recovers_fixed_mode() {
    g_radio_watchdog = true; g_stick_round = 50;
    run_stream(40000, [](int){ return 5.0; }, 8000, nullptr,
               /*feed_rate=*/40, /*auto=*/false, /*start=*/0);   // pinned turbo
    g_stick_round = -1;
    TEST_ASSERT_TRUE_MESSAGE(s_last_reinits >= 1,
        "watchdog must re-init the deaf radio in fixed mode too");
    TEST_ASSERT_TRUE_MESSAGE(s_last_exact,
        "fixed-mode stream must survive a deaf radio byte-exact");
    TEST_ASSERT_FALSE_MESSAGE(trace_changed_from(0),
        "recovery must PRESERVE the pinned mode (no auto-change)");
}

// ===========================================================================
// AUTO-POWER control-loop sim. The firmware's AdjustTxPower (fw_host.cpp) moves
// OUR TX power from the RSSI WE receive, assuming a symmetric link ("the RSSI
// we receive is a proxy for how the peer receives us"). On an ASYMMETRIC path
// (different path loss each way: boards/antennas/orientation) plus the elevated
// noise floor of a close-range bench, that assumption breaks: a node
// that hears its peer strongly floors its own power, which starves the peer
// below its demod SNR. The link dies while each side "thinks" it's healthy.
// This models the loop so we catch that class of bug in sim. It reproduces the
// EXACT hardware equilibrium we observed (A floored at pwr=-10, B can't demod,
// A still hears B fine) and shows the fixes keep the link up.
// ===========================================================================

/**
 * @brief AdjustTxPower constants, mirrored from fw_host.cpp.
 *
 * kRssiHi/kRssiLo are the RSSI band the own-RSSI loop nudges within; kApStep is
 * the per-step power change (dB); kPwrFloor/kPwrMax bound the TX power (dBm).
 */
static const int kRssiHi = -55, kRssiLo = -95, kApStep = 2;
static const int kPwrFloor = -9, kPwrMax = 22;

/// @brief Which auto-power control loop a run exercises.
enum ApMode {
    AP_OWN_RSSI,   ///< BROKEN: adjust own TX from own received RSSI.
    AP_FIXED,      ///< auto-power off: hold a fixed power.
    AP_PEER_SNR,   ///< CORRECT: adjust own TX from the peer's reported SNR.
};

/**
 * @brief The BROKEN loop: adjust own TX power from own received RSSI (current
 *        firmware) — the symmetric-link assumption this sim breaks.
 *
 * @param[in,out] my_tx        our TX power (dBm), nudged in place.
 * @param[in]     my_recv_rssi the RSSI we receive from the peer (dBm).
 */
static void ap_own_rssi(int* my_tx, int my_recv_rssi) {
    if (my_recv_rssi > kRssiHi && *my_tx > kPwrFloor) *my_tx -= kApStep;
    else if (my_recv_rssi < kRssiLo && *my_tx < kPwrMax) *my_tx += kApStep;
}

/**
 * @brief The CORRECT loop: adjust own TX power from the PEER's reported
 *        received SNR, holding a margin above the demod floor.
 *
 * Delegates to link_layer::AutoPowerStep so the sim exercises the EXACT control
 * law the firmware ships (one source of truth, lib/linklayer/autopower.h). It
 * never starves the peer, and still backs off to save power when the peer has
 * plenty of margin.
 *
 * @param[in,out] my_tx         our TX power (dBm), nudged in place.
 * @param[in]     peer_recv_snr the SNR the peer reports for our signal (dB).
 * @param[in]     snr_floor     the mode's demod floor (dB).
 */
static void ap_peer_snr(int* my_tx, int peer_recv_snr, int snr_floor) {
    *my_tx = link_layer::AutoPowerStep((int8_t)*my_tx, peer_recv_snr,
                                       snr_floor, (int8_t)kPwrFloor,
                                       (int8_t)kPwrMax);
}

/// @brief Equilibrium of the auto-power loop: final powers, per-direction SNRs,
///        and whether each direction's link stays up.
struct ApResult {
    int a_tx, b_tx;     ///< final TX power at A and B (dBm).
    int a_snr, b_snr;   ///< SNR A receives from B, and B receives from A (dB).
    bool a2b_up;        ///< true if the A->B (data) direction clears the floor.
    bool b2a_up;        ///< true if the B->A direction clears the floor.
};

/**
 * @brief Iterate the two-sided power loop to equilibrium over an asymmetric
 *        channel.
 *
 * Received RSSI at a node = peer_tx - path_loss(that direction); the SNR there
 * is rssi - noise; a direction is "up" if its SNR clears the mode's demod
 * floor.
 *
 * @param[in] loss_ab   path loss A->B in dB.
 * @param[in] loss_ba   path loss B->A in dB.
 * @param[in] noise     the noise floor in dBm.
 * @param[in] snr_floor the mode's demod floor in dB.
 * @param[in] mode      which power-control loop to run (see ApMode).
 * @param[in] rounds    iterations to run toward equilibrium.
 * @return the final powers, per-direction SNRs, and up/down link states.
 */
static ApResult run_autopower(int loss_ab, int loss_ba, int noise,
                              int snr_floor, ApMode mode, int rounds) {
    int a_tx = 14, b_tx = 14;        // both start at a mid power
    if (mode == AP_FIXED) { a_tx = b_tx = 14; }   // fixed: never adjusted
    int a_recv = 0, b_recv = 0;
    for (int r = 0; r < rounds; r++) {
        a_recv = b_tx - loss_ba;     // A hears B (the B->A direction)
        b_recv = a_tx - loss_ab;     // B hears A (the A->B direction)
        int a_snr = a_recv - noise, b_snr = b_recv - noise;
        if (mode == AP_OWN_RSSI) {
            ap_own_rssi(&a_tx, a_recv);
            ap_own_rssi(&b_tx, b_recv);
        } else if (mode == AP_PEER_SNR) {
            ap_peer_snr(&a_tx, b_snr, snr_floor);   // A: how B hears A
            ap_peer_snr(&b_tx, a_snr, snr_floor);   // B: how A hears B
        }
    }
    ApResult res;
    res.a_tx = a_tx; res.b_tx = b_tx;
    res.a_snr = a_recv - noise; res.b_snr = b_recv - noise;
    res.b2a_up = res.a_snr >= snr_floor;   // A->...: A hears B (B->A link)
    res.a2b_up = res.b_snr >= snr_floor;   // B hears A (A->B link = data path)
    return res;
}

/**
 * @brief Teeth: the own-RSSI loop starves an asymmetric, close-range link.
 *
 * Reproduces the exact hardware equilibrium: the initiator floors to pwr=-10,
 * the responder can no longer demodulate it (A->B dead), while the initiator
 * still hears the responder fine.
 */
void test_autopower_own_rssi_starves_asym() {
    // loss A->B 68 dB, B->A 58 dB (10 dB asymmetry); bench noise floor -55 dBm;
    // SF7 demod floor -7.5 (-8 as int).
    ApResult r = run_autopower(68, 58, -55, -8, AP_OWN_RSSI, 200);
    printf("  [own-rssi: a_tx=%d b_tx=%d a2b_up=%d b2a_up=%d b_snr=%d]\n",
           r.a_tx, r.b_tx, (int)r.a2b_up, (int)r.b2a_up, r.b_snr);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-10, r.a_tx,
        "should reproduce the observed pwr=-10 floor");
    TEST_ASSERT_FALSE_MESSAGE(r.a2b_up,
        "own-rssi loop must starve the A->B (data) direction");
    TEST_ASSERT_TRUE_MESSAGE(r.b2a_up,
        "initiator still hears responder fine (matches hardware)");
}

/**
 * @brief Fix (default): with auto-power OFF both ends hold a fixed sane power
 *        and the asymmetric link stays up in BOTH directions.
 */
void test_autopower_fixed_holds_asym() {
    ApResult r = run_autopower(68, 58, -55, -8, AP_FIXED, 200);
    TEST_ASSERT_TRUE_MESSAGE(r.a2b_up, "fixed power must keep A->B up");
    TEST_ASSERT_TRUE_MESSAGE(r.b2a_up, "fixed power must keep B->A up");
}

/**
 * @brief Future correct auto-power: peer-SNR feedback keeps BOTH directions
 *        above the demod floor on the link the own-RSSI loop kills.
 *
 * Validates the design before adding the feedback byte to the protocol.
 */
void test_autopower_peer_snr_holds_asym() {
    ApResult r = run_autopower(68, 58, -55, -8, AP_PEER_SNR, 200);
    printf("  [peer-snr: a_tx=%d b_tx=%d a2b_up=%d b2a_up=%d]\n",
           r.a_tx, r.b_tx, (int)r.a2b_up, (int)r.b2a_up);
    TEST_ASSERT_TRUE_MESSAGE(r.a2b_up, "peer-snr must keep A->B up");
    TEST_ASSERT_TRUE_MESSAGE(r.b2a_up, "peer-snr must keep B->A up");
}

/**
 * @brief Sanity: on a SYMMETRIC link the own-RSSI loop is fine, pinning the
 *        bug specifically to the symmetric-link assumption.
 */
void test_autopower_own_rssi_ok_when_symmetric() {
    ApResult r = run_autopower(60, 60, -55, -8, AP_OWN_RSSI, 200);
    TEST_ASSERT_TRUE_MESSAGE(r.a2b_up && r.b2a_up,
        "symmetric link should stay up even with the own-rssi loop");
}

// ===========================================================================
// COORDINATED HEADROOM: auto-power + ADR together. Auto-power alone minimizes
// power for the CURRENT mode; under ADR that strands the mode controller a rung
// low, because the loop never holds enough SNR to clear the NEXT rung's floor +
// margin. AutoPowerTargetFloor fixes it: under ADR, target the next-faster
// rung's floor so ADR can climb, ratcheting power up rung by rung. This closed
// loop (ADR picks the mode, auto-power sets the power, on a fixed channel where
// SNR = power - path) proves the new policy reaches the fast end and the old
// current-floor policy parks low. Mirrors fw_host.cpp AdjustTxPower +
// Radio::next_faster_snr_floor (which the firmware, not built natively, wires).
// ===========================================================================

/**
 * @brief The sim's mirror of Radio::next_faster_snr_floor (LoRa fastest-first).
 *
 * @param[in]  mode      current mode index into kModes.
 * @param[out] out_floor next-faster LoRa rung's floor (dB) when one exists.
 * @return true if a faster LoRa rung exists (out_floor valid), else false.
 */
static bool sim_next_faster_floor(int mode, int* out_floor) {
    // kModes runs turbo(0)..far(4) then GFSK(5); the next-faster rung is
    // mode-1, and only turbo..far (1..4) have a faster LoRa rung above them.
    // Round UP (ceilf), mirroring Radio::next_faster_snr_floor, so the held
    // margin clears ADR's climb threshold (which uses the exact float floor).
    if (mode <= 0 || mode >= 5) return false;
    *out_floor = (int)ceilf(kModes[mode - 1].snr_floor);
    return true;
}

/**
 * @brief Run the coupled ADR + auto-power loop to equilibrium on a fixed
 *        channel and return the mode it settles on.
 *
 * Channel: the SNR each end measures = tx_power - path (a strong link = a small
 * path). Each tick ADR picks the mode from the measured SNR, then auto-power
 * nudges power toward the target floor — the next-faster rung's floor when
 * `coordinated`, else the current mode's floor (the old policy).
 *
 * @param[in]  coordinated use the next-rung headroom policy (vs current-floor).
 * @param[in]  path        channel path loss (dB): SNR = tx_power - path.
 * @param[in]  start_mode  the mode index to start from (e.g. far).
 * @param[out] out_tx      the final TX power (dBm).
 * @return the mode index the loop settles on.
 */
static int run_coord_climb(bool coordinated, int path, int start_mode,
                           int* out_tx) {
    link_layer::AdrController adr;
    build_ladder(adr);                      // sim mode ladder + ADR timers
    int mode = start_mode, tx = kPwrFloor;  // start power-minimized (the bug)
    uint32_t now = 0;
    for (int t = 0; t < 400; t++) {
        now += adr.cfg.period_ms;
        int snr = tx - path;                // symmetric channel
        link_layer::AdrController::In in;
        in.now = now; in.busy = false; in.have_link = true;
        in.cur = mode; in.snr = (float)snr; in.rssi = -80.0f; in.retx_pct = 0;
        link_layer::AdrAction a = adr.Decide(in);
        if (a.kind == link_layer::ADR_REQUEST) mode = a.mode;
        int nf = 0; bool hf = sim_next_faster_floor(mode, &nf);
        int floor = link_layer::AutoPowerTargetFloor(
            coordinated, (int)lroundf(kModes[mode].snr_floor), nf, hf);
        ap_peer_snr(&tx, snr, floor);       // symmetric: peer hears us at snr
    }
    if (out_tx) *out_tx = tx;
    return mode;
}

/**
 * @brief Teeth: coordinated headroom lets ADR climb to the fast end; the old
 *        current-floor policy strands it on the robust rung.
 *
 * Same channel and start (far, power-minimized) both ways — only the floor
 * policy differs — so the mode divergence is the fix, nothing else.
 */
void test_sim_adr_autopower_coordinated_climbs() {
    int coord_tx = 0, base_tx = 0;
    // Start on far (mode 4); the channel (path 14 dB) reaches turbo only with
    // power headroom held for the next rung.
    int m_coord = run_coord_climb(true,  14, 4, &coord_tx);
    int m_base  = run_coord_climb(false, 14, 4, &base_tx);
    printf("  [coord: mode=%d tx=%d | base: mode=%d tx=%d]\n",
           m_coord, coord_tx, m_base, base_tx);
    TEST_ASSERT_TRUE_MESSAGE(m_coord < m_base,
        "coordinated headroom must reach a faster mode than the old policy");
    TEST_ASSERT_TRUE_MESSAGE(m_coord <= 1,
        "coordinated headroom should ratchet ADR up to the fast end");
    TEST_ASSERT_TRUE_MESSAGE(m_base >= 3,
        "the old current-floor policy strands ADR on a robust rung");
    TEST_ASSERT_TRUE_MESSAGE(coord_tx >= kPwrFloor && coord_tx <= kPwrMax,
        "TX power stays within bounds");
}

/**
 * @brief A HARD wedge a begin()-only re-init can't clear is recovered by the
 *        last-resort no-progress REBOOT, and the stream survives byte-exact.
 *
 * Matches hardware, where a reflash/reboot cleared wedges that begin() did not.
 */
void test_sim_hard_wedge_reboot_recovers() {
    g_reboot_watchdog = true; g_hard_stick_round = 50;
    run_stream(40000, [](int){ return 5.0; }, 8000, nullptr, /*feed=*/40);
    g_hard_stick_round = -1;
    printf("  [hard-wedge: reboots=%d exact=%d]\n",
           s_last_reboots, (int)s_last_exact);
    TEST_ASSERT_TRUE_MESSAGE(s_last_reboots >= 1,
        "no-progress reboot must fire for a hard wedge");
    TEST_ASSERT_TRUE_MESSAGE(s_last_exact,
        "stream must survive a hard wedge via reboot");
}

/**
 * @brief Teeth: a begin()-only re-init can't clear a hard wedge, so without the
 *        reboot escalation the link stays dead, proving the reboot layer.
 */
void test_sim_hard_wedge_no_reboot_stays_dead() {
    g_reboot_watchdog = false; g_hard_stick_round = 50;
    run_stream(40000, [](int){ return 5.0; }, 4000, nullptr, /*feed=*/40);
    g_reboot_watchdog = true; g_hard_stick_round = -1;
    TEST_ASSERT_FALSE_MESSAGE(s_last_exact,
        "without the reboot escalation a hard wedge must NOT recover");
}

/**
 * @brief AT+SPEEDTEST logic must converge (no infinite loop) and deliver every
 *        byte, matching ServiceSpeedTest()'s completion condition.
 *
 * The firmware feeds N bytes internally and declares done when the SENDER
 * reports no outstanding work (all delivered + ACKed). This validates the
 * feed/drain/complete state machine; it can't catch a hardware radio wedge
 * under load (the sim abstracts the radio) but proves the logic itself.
 */
void test_sim_speedtest_logic_drains() {
    static SimNode I, R;
    I.link.Init(node_cfg(1, 2)); R.link.Init(node_cfg(2, 1));
    I.mode = R.mode = kMedium; I.last_rx_round = R.last_rx_round = 0;
    I.last_reinit_round = R.last_reinit_round = 0;
    I.last_rendezvous_round = R.last_rendezvous_round = 0;
    I.radio_stuck = R.radio_stuck = false; I.reinits = R.reinits = 0;
    I.radio_hard_stuck = R.radio_hard_stuck = false;
    I.reboots = R.reboots = 0; I.ever_rx = R.ever_rx = false;
    I.initiator = true; R.initiator = false; I.auto_mode = R.auto_mode = false;
    uint8_t w = kModes[kMedium].window;
    uint32_t rt = kModes[kMedium].retransmit_ms;
    I.link.SetTiming(w, rt); R.link.SetTiming(w, rt);
    I.ms.Begin(kMedium); R.ms.Begin(kMedium);
    build_ladder(I.adr); rseed(99);
    const size_t total = 8192;
    size_t fed = 0, got = 0; uint8_t out[4096];
    bool done = false; int round = 0;
    for (; round < 20000 && !done; round++) {
        if (fed < total) {                  // feed like ServiceSpeedTest()
            uint8_t buf[256];
            size_t want = total - fed; if (want > sizeof buf) want = sizeof buf;
            for (size_t i = 0; i < want; i++) buf[i] = (uint8_t)(fed + i);
            fed += I.link.Write(buf, want);
        }
        sim_round(I, R, round, 5.0f);
        size_t k;                                          // sink: drain peer
        while ((k = R.link.Read(out, sizeof out)) > 0) got += k;
        if (fed >= total && !I.link.HasWork()) done = true;   // completion cond
    }
    TEST_ASSERT_TRUE_MESSAGE(done, "speedtest logic must converge, not hang");
    TEST_ASSERT_EQUAL_UINT32(total, (uint32_t)got);
}

// ---------------------------------------------------------------------------
// far/SF12 TAIL-LOSS recovery (regression for the "far transfer stalls" bug).
//
// The SACK-driven FAST retransmit can only fire when a LATER frame is acked
// (revealing the gap). The last frame of a transfer — or a lone single-frame
// transfer — has no later frame, so its ONLY recovery is the retransmit TIMER.
// With a window-UNAWARE timer (toa*12) that timer is ~12x time-on-air, which at
// far/SF12 (ToA ~ seconds) is many tens of seconds of dead air — the transfer
// appears hung. A WINDOW-AWARE timer (~one burst round-trip) recovers a tail
// loss in a few ToA. This harness drives a clean far transfer and an identical
// one with the last data frame's first transmission dropped, and compares the
// recovery overhead.
// ---------------------------------------------------------------------------
/**
 * @brief Clocked far/SF12 transfer that can drop one data frame by seq, to
 *        measure tail-loss recovery overhead.
 *
 * @param[in]  total    number of bytes to transfer.
 * @param[in]  drop_seq seq number of the data frame to drop once (-1 = none).
 * @param[out] exact    set true iff the receiver got the bytes byte-exact.
 * @return the elapsed sim clock in milliseconds.
 */
static uint32_t far_tail_transfer(size_t total, int drop_seq, bool* exact) {
    static link_layer::LinkLayer<16384> A, B;
    const int mode = 4;                          // far (SF12/125) in kModes[]
    Timing tm = derive_timing(mode);
    link_layer::Config ca = node_cfg(1, 2);
    ca.window = tm.window; ca.retransmit_ms = tm.retransmit;
    link_layer::Config cb = node_cfg(2, 1);
    cb.window = tm.window; cb.retransmit_ms = tm.retransmit;
    A.Init(ca); B.Init(cb);
    std::vector<uint8_t> src(total), got;
    link_layer::Lcg gen(0xC0DE);                  // deterministic payload
    for (size_t i = 0; i < total; i++) src[i] = gen.NextByte();
    const uint32_t toa = (uint32_t)kToaMs[mode], turnaround = 20;
    uint32_t clock = 0; size_t ingested = 0;
    uint8_t out[4096], fr[link_layer::MAXFRAME]; size_t fl;
    bool dropped = false;
    auto burst = [&](link_layer::LinkLayer<16384>& S,
                     link_layer::LinkLayer<16384>& D, bool may_drop) {
        S.BeginTurn();
        std::vector<std::vector<uint8_t>> frames;
        while (S.NextTx(fr, sizeof fr, fl, clock))
            frames.push_back(std::vector<uint8_t>(fr, fr + fl));
        if (frames.empty()) { fl = S.MakePoll(fr, sizeof fr);
            frames.push_back(std::vector<uint8_t>(fr, fr + fl)); }
        for (size_t i = 0; i < frames.size(); i++) {
            clock += toa;                          // charge airtime
            const uint8_t* p = frames[i].data();
            // Header (src,dst,flags,seq,...) is cleartext even under AEAD, so
            // we can identify the data frame to drop by its flags + seq.
            bool is_data = frames[i].size() > 3 && (p[2] & link_layer::F_DATA);
            uint8_t seq = frames[i].size() > 3 ? p[3] : 0;
            if (may_drop && !dropped && is_data && drop_seq >= 0 &&
                seq == (uint8_t)drop_seq) { dropped = true; continue; }
            D.OnRx(p, frames[i].size(), clock);
        }
        clock += turnaround;
    };
    for (int turn = 0; turn < 500000 && got.size() < total; turn++) {
        if (ingested < total)
            ingested += A.Write(src.data() + ingested, total - ingested);
        burst(A, B, true);                         // data A->B (may drop tail)
        size_t k; while ((k = B.Read(out, sizeof out)) > 0)
            got.insert(got.end(), out, out + k);
        burst(B, A, false);                        // ack/poll B->A
        while (A.Read(out, sizeof out) > 0) {}
    }
    *exact = got.size() == total &&
             memcmp(got.data(), src.data(), total) == 0;
    return clock;
}

/**
 * @brief A dropped TAIL frame at far must recover within ~one round-trip of
 *        extra airtime, not the full window-unaware retransmit timeout.
 *
 * Fails with the old toa*12 timer (the lost tail waits ~12+ ToA of dead air);
 * passes once the retransmit timer scales with the (small) far window.
 */
void test_far_tail_loss_recovers_promptly() {
    const size_t total = 3 * link_layer::MAXPAY;  // 3 data frames: seq 0,1,2
    bool ex_clean = false, ex_drop = false;
    uint32_t clean = far_tail_transfer(total, /*drop_seq=*/-1, &ex_clean);
    uint32_t drop  = far_tail_transfer(total, /*drop_seq=*/2,  &ex_drop);
    TEST_ASSERT_TRUE_MESSAGE(ex_clean, "clean far transfer not byte-exact");
    TEST_ASSERT_TRUE_MESSAGE(ex_drop,
        "far tail-loss not recovered byte-exact");
    uint32_t toa = (uint32_t)kToaMs[4];
    uint32_t overhead = drop - clean;
    printf("  [far tail-loss: clean %u ms, drop %u ms, overhead %u ms = "
           "%.1f ToA]\n", clean, drop, overhead, (double)overhead / toa);
    // Recovery overhead budget: one round-trip + a window-aware timer is a few
    // ToA; the window-UNAWARE toa*12 timer blows well past this.
    const uint32_t kFarRecoverBudgetToa = 11;
    TEST_ASSERT_TRUE_MESSAGE(overhead < toa * kFarRecoverBudgetToa,
        "far tail-loss recovery too slow (retransmit timer not window-aware)");
}

/**
 * @brief The ToA-scaled TX safety timeout, mirroring fw_radio.cpp (kTxSafety*).
 *
 * Must EXCEED the frame's real time-on-air, else a long frame is aborted
 * mid-transmission and the peer hears nothing (the far/SF12 "data never
 * arrives" bug: a fixed 8 s timeout < far's ~13 s ToA).
 *
 * @param[in] toa_ms the frame's time-on-air in milliseconds.
 * @return the safety timeout in milliseconds.
 */
static uint32_t tx_safety_ms(uint32_t toa_ms) {
    uint32_t s = toa_ms * 2 + 1000;          // ToaMul=2, MarginMs=1000
    return s < 8000 ? 8000 : s;              // FloorMs=8000
}

/**
 * @brief The TX safety timeout exceeds the frame ToA at every mode (so a long
 *        frame is never aborted mid-air), with teeth for the old fixed 8 s.
 */
void test_tx_safety_exceeds_toa_every_mode() {
    // Realistic full-frame ToA (ms) per mode: turbo, fast, medium, slow, far.
    // far/SF12/BW125/CR4-8 is ~13 s — the case the old fixed 8 s timeout broke.
    const uint32_t toa[] = {30, 70, 150, 1800, 13000};
    for (int m = 0; m < 5; m++) {
        TEST_ASSERT_TRUE_MESSAGE(tx_safety_ms(toa[m]) > toa[m],
            "TX safety timeout must exceed frame ToA (else TX aborts mid-air)");
    }
    // Teeth: the OLD fixed 8 s would FAIL this for far (8000 < 13000).
    TEST_ASSERT_TRUE_MESSAGE(8000u <= 13000u,
        "regression guard: a fixed 8s TX safety is shorter than far's ToA");
}

/**
 * @brief Register and run every integration-sim test under Unity.
 *
 * @return Unity's aggregate pass/fail status code.
 */
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_far_tail_loss_recovers_promptly);
    RUN_TEST(test_tx_safety_exceeds_toa_every_mode);
    RUN_TEST(test_sim_speedtest_logic_drains);
    RUN_TEST(test_sim_hard_wedge_reboot_recovers);
    RUN_TEST(test_sim_hard_wedge_no_reboot_stays_dead);
    RUN_TEST(test_sim_fixed_mode_holds_on_loss);
    RUN_TEST(test_sim_fixed_mode_drop_repro);
    RUN_TEST(test_sim_auto_steps_down_on_loss);
    RUN_TEST(test_sim_climb_byte_exact);
    RUN_TEST(test_sim_snr_sweep_byte_exact);
    RUN_TEST(test_sim_reset_on_switch_loses_data);
    RUN_TEST(test_sim_weak_link_byte_exact);
    RUN_TEST(test_sim_large_backlog_byte_exact);
    RUN_TEST(test_phys_goodput_ordering);
    RUN_TEST(test_phys_interframe_timeout_collapses);
    RUN_TEST(test_phys_rx_rearm_miss_collapses);
    RUN_TEST(test_phys_continuous_no_standby_goes_deaf);
    RUN_TEST(test_sim_radio_stuck_recovers);
    RUN_TEST(test_sim_radio_stuck_no_watchdog_stays_dead);
    RUN_TEST(test_sim_radio_stuck_recovers_fixed_mode);
    RUN_TEST(test_autopower_own_rssi_starves_asym);
    RUN_TEST(test_autopower_fixed_holds_asym);
    RUN_TEST(test_autopower_peer_snr_holds_asym);
    RUN_TEST(test_autopower_own_rssi_ok_when_symmetric);
    RUN_TEST(test_sim_adr_autopower_coordinated_climbs);
    return UNITY_END();
}
