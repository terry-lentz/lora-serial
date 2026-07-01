/**
 * @file adr.h
 * @brief The Adaptive Data Rate (ADR) *decision*, as pure portable logic.
 *
 * The 'auto' mode lets the initiator move BOTH ends up and down the speed
 * ladder at runtime so the link runs as fast as conditions actually allow.
 * Picking the next mode is a small state machine with real subtlety (loss vs
 * SNR, hysteresis so it can't oscillate, a GFSK top rung that's gated behind
 * a strong link, and a cooldown so it can't dead-flap). It is split into two
 * pure, radio-free pieces so the native unit tests can drive it directly:
 * AdrLadder (the mode-ladder math — which mode is faster / more robust / fast
 * enough for a given SNR, table-driven) and AdrController (the per-tick
 * decision: given the link's current mode, SNR and retransmit rate, what, if
 * anything, to do). The firmware is a thin adapter: it builds an AdrLadder
 * from the radio mode table, gathers the live inputs, calls
 * AdrController::Decide(), and carries out the returned action with the
 * mode-switch handshake. The actual RF behaviour still only shows up on
 * hardware, but every *decision* this code makes is reproducible and asserted
 * in sim. This header declares the interface; the methods that loop or carry
 * the decision logic are defined in adr.cpp (per the Google C++ style guide).
 * Only trivial one-line accessors stay inline.
 */
#ifndef LINK_LAYER_ADR_H_
#define LINK_LAYER_ADR_H_
#include <stdint.h>

namespace link_layer {

// --- The mode ladder -------------------------------------------------------
/**
 * @brief Pure description of the available PHY presets (parallel arrays).
 *
 * Parallel arrays indexed by mode index. The firmware fills this in from its
 * radio mode table; the tests fill it in with a hand-written table. No radio
 * dependency either way. Note the ladder is NOT in speed order: the LoRa
 * presets run fastest-first (turbo at index 0 .. far last), and the GFSK
 * 'ludicrous' rung sits last in the table but is the *fastest* of all — so
 * speed comparisons must go through Rank(), never a raw index compare.
 */
struct AdrLadder {
    int          count     = 0;        ///< Number of presets in the table.
    const float* snr_floor = nullptr;  ///< Per-mode LoRa demod SNR floor (dB).
    const bool*  is_fsk    = nullptr;  ///< Per-mode: is this the GFSK PHY?
    /// Per-mode: may 'auto' pick this LoRa rung? (null = all eligible). Lets us
    /// keep a structurally lossy preset like turbo as manual-only.
    const bool*  auto_ok   = nullptr;
    float        margin_db = 6.0f;     ///< dB of headroom required over a floor

    /**
     * @brief Is preset `i` the GFSK PHY? (bounds-checked).
     * @param[in] i preset index.
     * @return true if `i` is a valid index and that preset is GFSK.
     */
    bool IsFsk(int i) const { return i >= 0 && i < count && is_fsk[i]; }

    /**
     * @brief May 'auto' pick preset `i`? (bounds-checked).
     *
     * A null auto_ok table means every preset is eligible.
     *
     * @param[in] i preset index.
     * @return true if `i` is valid and eligible for automatic selection.
     */
    bool AutoOk(int i) const {
        return i >= 0 && i < count && (auto_ok == nullptr || auto_ok[i]);
    }

    /**
     * @brief Speed rank of a preset; higher = faster.
     *
     * GFSK tops the ladder; among LoRa a lower index (lower SF / wider BW)
     * is faster, hence the negation.
     *
     * @param[in] i preset index (negative -> sentinel "slower than any").
     * @return the comparable speed rank.
     */
    int Rank(int i) const {
        if (i < 0) return -1000;
        if (is_fsk[i]) return 1000;
        return -i;
    }
    /**
     * @brief Is preset `a` faster than preset `b`? (compares by Rank()).
     * @param[in] a candidate preset index.
     * @param[in] b reference preset index.
     * @return true if `a` ranks faster than `b`.
     */
    bool Faster(int a, int b) const { return Rank(a) > Rank(b); }

    /**
     * @brief Index of the GFSK ('ludicrous') preset.
     * @return the GFSK preset index, or -1 if the table has none.
     */
    int Ludicrous() const;

    /**
     * @brief Fastest LoRa preset overall: the first non-GFSK entry.
     * @return that preset's index (e.g. "turbo"), or -1 if none.
     */
    int FastestLora() const;

    /**
     * @brief Fastest LoRa preset 'auto' is allowed to use.
     *
     * The first non-GFSK entry that is auto_ok (e.g. "fast" when turbo is
     * manual-only). This is the rung the GFSK top rung is reached from.
     *
     * @return that preset's index, or -1 if none.
     */
    int FastestAutoLora() const;

    /**
     * @brief Most robust LoRa preset (this is "far"): the last non-GFSK entry.
     * @return that preset's index, or -1 if none.
     */
    int LastLora() const;

    /**
     * @brief Fastest LoRa preset the measured SNR clears (floor + margin).
     *
     * If none clear, returns the most robust LoRa preset. GFSK is never
     * auto-picked here (it's opt-in and gated separately) — only by SNR.
     *
     * @param[in] snr_db measured LoRa SNR in dB.
     * @return the chosen LoRa preset index.
     */
    int PickBySnr(float snr_db) const;

    /**
     * @brief One step toward more robustness.
     *
     * GFSK -> turbo, otherwise one slower LoRa preset (or stay put if
     * already the most robust).
     *
     * @param[in] i current preset index.
     * @return the more-robust preset index.
     */
    int MoreRobust(int i) const;
};

// --- The controller --------------------------------------------------------
/**
 * @brief Controller tunables (defaults mirror the firmware's kAdr*).
 *
 * Pulled out so the tests can shrink the timers and so the firmware keeps
 * one source of truth.
 */
struct AdrConfig {
    uint32_t period_ms        = 3000;   ///< How often to re-evaluate (ms).
    int      up_stable        = 3;      ///< Clean evals needed before step UP.
    int      down_retx_pct    = 25;     ///< Step DOWN at/above this loss %.
    int      up_max_retx_pct  = 8;      ///< Only step UP below this loss %.
    /// RSSI (dBm) needed to try GFSK. RSSI, not SNR: LoRa SNR saturates ~+11 dB
    /// so it can't tell a 5 cm link from a marginal one; RSSI can.
    float    gfsk_up_rssi     = -70.0f;
    uint32_t switch_timeout_ms = 6000;  ///< Give up a stuck switch after this.
    uint32_t cooldown_ms      = 30000;  ///< ...then don't retry for this long.
    bool     gfsk_enabled     = false;  ///< Is the GFSK top rung opt-in on?
    /// After a rendezvous fallback, keep the failed mode off-limits this long
    /// (ms) so ADR can't immediately re-climb into the mode that just died.
    uint32_t fallback_penalty_ms = 60000;
};

/** @brief What the controller decided to do this tick. */
enum AdrActionKind {
    ADR_NONE,     ///< Do nothing this tick.
    ADR_REQUEST,  ///< Ask the mode-switch handshake to move to `mode`.
    ADR_ABORT,    ///< Abandon the in-flight switch and start a cooldown.
};

/** @brief One controller decision: a kind plus its target preset. */
struct AdrAction {
    AdrActionKind kind;   ///< What to do (none / request / abort).
    int           mode;   ///< Target preset (valid for ADR_REQUEST).
};

/** @brief Per-tick ADR decision engine (the firmware's old DriveAdr). */
class AdrController {
public:
    AdrLadder ladder;   ///< The mode ladder this controller decides over.
    AdrConfig cfg;       ///< The controller tunables.

    /** @brief The live inputs the firmware gathers each loop tick. */
    struct In {
        uint32_t now;        ///< Current millis().
        bool     busy;       ///< A mode switch is in flight (handshake Busy()).
        bool     have_link;  ///< We have a fresh RSSI/SNR reading.
        int      cur;        ///< Current mode index.
        float    snr;        ///< Measured LoRa SNR (dB); ignored on GFSK.
        /// Smoothed RSSI (dBm) — gates the GFSK rung because LoRa SNR saturates
        /// ~+11 dB and can't signal "very strong link".
        float    rssi;
        int      retx_pct;   ///< Loss % on current mode (-1 if too few frames).
    };

    /** @brief Reset the hysteresis/timer state. Call once at startup. */
    void Begin() {
        last_ = switch_at_ = cooldown_until_ = 0;
        want_ = -1; stable_ = 0;
        penalized_ = -1; penalized_until_ = 0;
    }

    /**
     * @brief Run one tick of the ADR decision (the old DriveAdr()).
     *
     * @param[in] in the live inputs for this tick (mode, SNR, RSSI, loss).
     * @return the action to take this tick (often ADR_NONE).
     */
    AdrAction Decide(const In& in);

    /**
     * @brief Record that `failed_mode` died and the firmware fell back to a
     *        robust mode (a rendezvous).
     *
     * Arms the cooldown and penalizes that mode so Decide() won't re-climb into
     * it (or any faster mode) until fallback_penalty_ms elapses — preventing a
     * medium<->fast-mode flap when a fast rung keeps dying.
     *
     * @param[in] now         current millis().
     * @param[in] failed_mode the mode index that just died (now off-limits).
     */
    void OnFallback(uint32_t now, int failed_mode);

private:
    /**
     * @brief Emit a switch request and arm the in-flight timer.
     *
     * Resets the hysteresis and starts the timer the busy/abort branch
     * watches.
     *
     * @param[in] now  current millis().
     * @param[in] mode target preset to request.
     * @return an ADR_REQUEST action for `mode`.
     */
    AdrAction Commit(uint32_t now, int mode);

    uint32_t last_ = 0;            ///< millis() of the last evaluation tick.
    uint32_t switch_at_ = 0;       ///< millis() the in-flight switch started.
    uint32_t cooldown_until_ = 0;  ///< Don't switch again until this millis().
    int want_ = -1;                ///< Up-step target being confirmed.
    int stable_ = 0;               ///< Consecutive clean evals for `want_`.
    int penalized_ = -1;           ///< Mode that died in a fallback (no climb).
    uint32_t penalized_until_ = 0; ///< Penalty holds until this millis().
};

} // namespace link_layer

#endif  // LINK_LAYER_ADR_H_
