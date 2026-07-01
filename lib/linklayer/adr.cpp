/**
 * @file adr.cpp
 * @brief Implementation of the Adaptive Data Rate decision declared in adr.h.
 *
 * Defines the AdrLadder mode-math and the AdrController per-tick state machine.
 * The per-method contracts live with their declarations in adr.h.
 */
#include "adr.h"

namespace link_layer {

int AdrLadder::Ludicrous() const {
    for (int i = 0; i < count; i++) if (is_fsk[i]) return i;
    return -1;
}

int AdrLadder::FastestLora() const {
    for (int i = 0; i < count; i++) if (!is_fsk[i]) return i;
    return -1;
}

int AdrLadder::FastestAutoLora() const {
    for (int i = 0; i < count; i++) if (!is_fsk[i] && AutoOk(i)) return i;
    return -1;
}

int AdrLadder::LastLora() const {
    int last = -1;
    for (int i = 0; i < count; i++) if (!is_fsk[i]) last = i;
    return last;
}

int AdrLadder::PickBySnr(float snr_db) const {
    int last_lora = FastestAutoLora();
    for (int i = 0; i < count; i++) {
        if (is_fsk[i] || !AutoOk(i)) continue;   // skip GFSK + manual rungs
        last_lora = i;
        if (snr_db >= snr_floor[i] + margin_db) return i;
    }
    return last_lora;
}

int AdrLadder::MoreRobust(int i) const {
    if (IsFsk(i)) return FastestAutoLora();   // GFSK -> fastest auto LoRa rung
    int last = LastLora();
    return (i < last) ? i + 1 : i;
}

AdrAction AdrController::Commit(uint32_t now, int mode) {
    switch_at_ = now; want_ = -1; stable_ = 0;
    return {ADR_REQUEST, mode};
}

void AdrController::OnFallback(uint32_t now, int failed_mode) {
    cooldown_until_ = now + cfg.cooldown_ms;
    penalized_ = failed_mode;
    penalized_until_ = now + cfg.fallback_penalty_ms;
    want_ = -1; stable_ = 0;
}

AdrAction AdrController::Decide(const In& in) {
    const AdrAction none{ADR_NONE, -1};
    uint32_t now = in.now;
    // Cadence: only re-evaluate every period_ms.
    if (now - last_ < cfg.period_ms) return none;
    last_ = now;

    // A switch is in flight. If it can't complete (e.g. the handshake's ACK
    // keeps getting lost on a lossy link), abandon it and cool down: better
    // to sit on the working-but-lossy mode than dead-flap.
    if (in.busy) {
        if (now - switch_at_ > cfg.switch_timeout_ms) {
            cooldown_until_ = now + cfg.cooldown_ms;
            want_ = -1; stable_ = 0;
            return {ADR_ABORT, -1};
        }
        return none;
    }
    if (now < cooldown_until_ || !in.have_link) return none;
    int cur = in.cur;
    if (cur < 0) return none;
    int retx = in.retx_pct;   // -1 means "not enough frames to trust yet"

    // (1) Losing too many frames here -> step to a more robust mode at once,
    //     whatever the SNR says. This is the only signal that works on GFSK
    //     too, and it catches a mode that looks fine on SNR but isn't actually
    //     delivering.
    if (retx >= cfg.down_retx_pct) {
        int robust = ladder.MoreRobust(cur);
        if (robust != cur) return Commit(now, robust);
        return none;
    }
    if (ladder.IsFsk(cur)) return none;  // on GFSK: SNR unusable, down-only

    // (2) SNR-based target among the LoRa presets.
    int pick = ladder.PickBySnr(in.snr);
    // (2b) Opt-in GFSK top rung: from turbo only, and only when the link is
    //      very strong AND not already lossy.
    int lud = ladder.Ludicrous();
    if (cfg.gfsk_enabled && lud >= 0 && cur == ladder.FastestAutoLora()
        && in.rssi >= cfg.gfsk_up_rssi && retx <= cfg.up_max_retx_pct) {
        pick = lud;
    }

    // Flap guard: just after a rendezvous fallback, don't climb back into the
    // mode that died (or a faster one) until the penalty window expires; cap
    // the target to one step more robust than the failed mode.
    if (penalized_ >= 0 && now < penalized_until_ &&
        ladder.Rank(pick) >= ladder.Rank(penalized_)) {
        pick = ladder.MoreRobust(penalized_);
    }
    if (pick == cur) { want_ = -1; stable_ = 0; return none; }

    if (ladder.Faster(pick, cur)) {            // stepping UP (faster)
        if (retx > cfg.up_max_retx_pct) {      // don't climb into loss
            want_ = -1; stable_ = 0; return none;
        }
        if (pick == want_) stable_++; else { want_ = pick; stable_ = 1; }
        if (stable_ >= cfg.up_stable)          // sustained -> commit
            return Commit(now, pick);
        return none;
    }
    return Commit(now, pick);                  // SNR says go more robust
}

} // namespace link_layer
