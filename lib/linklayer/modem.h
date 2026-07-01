/**
 * @file modem.h
 * @brief Portable half-duplex modem turn-taking.
 *
 * Shared by the firmware and the native sim so the exact same logic is
 * unit-tested. The radio is injected via the IRadio interface, so the
 * turn-taking can be driven against a simulated channel with no hardware.
 */
#ifndef LINK_LAYER_MODEM_H_
#define LINK_LAYER_MODEM_H_
#include <stddef.h>
#include <stdint.h>

#include "linklayer.h"

namespace link_layer {

/** @brief Injected radio transport the turn-taking drives (TX/RX/clock). */
struct IRadio {
    /**
     * @brief Transmit one frame.
     *
     * @param[in] buf the frame bytes to send.
     * @param[in] len number of bytes to send.
     * @return true if the frame was transmitted.
     */
    virtual bool     tx(const uint8_t* buf, size_t len) = 0;

    /**
     * @brief Block up to timeoutMs for a received frame.
     *
     * @param[out] buf       destination buffer for the frame.
     * @param[in]  cap       capacity of `buf`.
     * @param[out] len       number of bytes received.
     * @param[in]  timeoutMs how long to wait, in ms.
     * @return true with (buf,len) on receive; false on timeout.
     */
    virtual bool     rx(uint8_t* buf, size_t cap, size_t& len,
                        uint32_t timeoutMs) = 0;

    /**
     * @brief Current monotonic time in ms (the platform's millis()).
     * @return the millisecond clock value.
     */
    virtual uint32_t now() = 0;

    /** @brief Virtual destructor so concrete radios clean up correctly. */
    virtual ~IRadio() {}
};

/** @brief Half-duplex turn-taking timeouts (per role). */
struct ModemTiming {
    /// Initiator: how long to wait for the responder's whole burst.
    uint32_t turn_rx_ms    = 2500;
    /// Wait this long for the next frame within a burst before ending it.
    uint32_t interframe_ms = 1500;
    /// Responder: how long to wait for the initiator to start a turn.
    uint32_t responder_wait_ms  = 3000;
    /// Cap on the initiator's adaptive idle backoff between empty turns.
    uint32_t max_idle_gap_ms = 2000;
};

/** @brief Initiator's per-step state: last turn time + adaptive idle gap. */
struct ModemState {
    uint32_t last_turn = 0;  ///< millis() the last turn was started.
    uint32_t idle_gap = 50;  ///< Current adaptive gap between empty turns (ms).
};

/**
 * @brief Run one initiator turn: send our burst (or a poll), then listen.
 *
 * Sends every frame the link has this turn (or a poll to grant the responder
 * a turn), listens for the responder's burst, then adapts the idle gap by
 * backing off when both ends are idle.
 *
 * @param[in,out] lk the link layer to pump.
 * @param[in,out] r  the radio transport.
 * @param[in,out] st the initiator's persistent step state.
 * @param[in]     t  the turn-taking timeouts.
 */
template <class LL>
void InitiatorStep(LL& lk, IRadio& r, ModemState& st, const ModemTiming& t) {
    uint32_t now = r.now();
    uint32_t gap = lk.HasWork() ? 0 : st.idle_gap;
    if (now - st.last_turn < gap) return;
    st.last_turn = now;

    uint8_t fr[MAXFRAME]; size_t fl;
    lk.BeginTurn();
    bool sent = false;
    while (lk.NextTx(fr, sizeof(fr), fl, now)) { r.tx(fr, fl); sent = true; }
    if (!sent) {
        fl = lk.MakePoll(fr, sizeof(fr));
        r.tx(fr, fl);                       // grant the responder a turn
    }

    bool myTurn = false;
    uint32_t t0 = r.now();
    while (!myTurn && r.now() - t0 < t.turn_rx_ms) {
        size_t rl;
        if (r.rx(fr, sizeof(fr), rl, t.interframe_ms))
            myTurn = lk.OnRx(fr, rl, r.now());
        else break;
    }
    bool active = lk.HasWork() || lk.TakeGotData();
    st.idle_gap = active ? 50
                        : (st.idle_gap < t.max_idle_gap_ms ? st.idle_gap * 2
                                                       : t.max_idle_gap_ms);
}

/**
 * @brief Run one responder step: wait for the initiator, then reply.
 *
 * Waits for the initiator's burst, consumes it to the end, then sends our own
 * burst (data, or an ack/poll so the initiator continues).
 *
 * @param[in,out] lk the link layer to pump.
 * @param[in,out] r  the radio transport.
 * @param[in]     t  the turn-taking timeouts.
 */
template <class LL>
void ResponderStep(LL& lk, IRadio& r, const ModemTiming& t) {
    uint8_t fr[MAXFRAME]; size_t rl;
    if (!r.rx(fr, sizeof(fr), rl, t.responder_wait_ms))
        return;     // keep listening
    bool myTurn = lk.OnRx(fr, rl, r.now());
    while (!myTurn) {
        if (r.rx(fr, sizeof(fr), rl, t.interframe_ms))
            myTurn = lk.OnRx(fr, rl, r.now());
        else break;
    }
    lk.BeginTurn();
    bool sent = false;
    size_t fl;
    while (lk.NextTx(fr, sizeof(fr), fl, r.now())) {
        r.tx(fr, fl); sent = true;
    }
    if (!sent) {
        fl = lk.MakePoll(fr, sizeof(fr));
        r.tx(fr, fl);                       // ack/poll so initiator continues
    }
}

} // namespace link_layer

#endif  // LINK_LAYER_MODEM_H_
