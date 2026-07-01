/**
 * @file modeswitch.h
 * @brief Coordinated PHY mode-switch handshake over the control nibble.
 *
 * Driven over the link layer's 4-bit control nibble (see linklayer.h
 * SetCtrlTx()/CtrlRx()). Both ends must always agree on the PHY (SF/BW/CR, or
 * LoRa-vs-GFSK), or the link goes deaf. This is the make-before-break protocol
 * that changes the PHY at runtime safely: the INITIATOR proposes a target mode
 * (REQ) on every frame until answered; the RESPONDER, on seeing the REQ, sends
 * an ACK on the OLD PHY and *then* switches; the INITIATOR switches as soon as
 * it receives that ACK; both arm a PROBATION timer on the new PHY — if no
 * frame is heard before it elapses (e.g. the ACK was lost so only one side
 * switched), they REVERT to the previous mode and the initiator simply
 * retries. Because both revert to the same previous mode, they always
 * re-converge. It is pure logic with NO radio/link dependencies, so two
 * instances can be run against a simulated channel in the native unit tests.
 * The firmware wires TxCtrl()->SetCtrlTx, the received nibble->AfterRecv, and
 * TakeApply()->ApplyRadio(). The non-trivial methods are defined in
 * modeswitch.cpp (per the Google C++ style guide).
 *
 * Control-nibble layout (the 4 spare header flag bits, CTRL_MASK = 0xF0):
 *   bit 0x10        = ACK flag (1 = ACK of a request, 0 = a request)
 *   bits 0x20-0x80  = (mode index + 1); 0 means "no control in this frame"
 */
#ifndef LINK_LAYER_MODESWITCH_H_
#define LINK_LAYER_MODESWITCH_H_
#include <stdint.h>

namespace link_layer {

/**
 * @brief Encode a control nibble for a mode index.
 *
 * @param[in] mode_idx the target mode index (>= 0); negative -> "no control".
 * @param[in] ack      true marks the nibble an ACK; false marks a request.
 * @return the encoded control nibble, or 0 for a negative index.
 */
inline uint8_t CtrlEncode(int mode_idx, bool ack) {
    if (mode_idx < 0) return 0;
    return (uint8_t)((((mode_idx + 1) & 0x07) << 5) | (ack ? 0x10 : 0));
}
/**
 * @brief Does `nib` carry a control message? (mode-index bits non-zero).
 * @param[in] nib the received control nibble.
 * @return true if a mode index is encoded in `nib`.
 */
inline bool CtrlPresent(uint8_t nib) { return (nib & 0xE0) != 0; }

/**
 * @brief Decode the mode index carried by `nib`.
 * @param[in] nib the received control nibble.
 * @return the mode index, or -1 if `nib` carries no control.
 */
inline int  CtrlModeIdx(uint8_t nib) {
    return CtrlPresent(nib) ? (int)(((nib >> 5) & 0x07) - 1) : -1;
}

/**
 * @brief Is `nib` an ACK (vs a request)?
 * @param[in] nib the received control nibble.
 * @return true if the ACK flag (0x10) is set.
 */
inline bool CtrlIsAck(uint8_t nib) { return (nib & 0x10) != 0; }

/** @brief Make-before-break PHY mode-switch handshake state machine. */
class ModeSwitch {
public:
    /**
     * @brief Revert window: silence on the new PHY this long forces a revert.
     *
     * If no frame is heard on the new PHY within this many ms after switching,
     * fall back to the previous mode. Make it comfortably longer than a couple
     * of turn round-trips.
     */
    uint32_t probation_ms = 4000;

    /**
     * @brief Initialize at the current mode index (no switch in flight).
     * @param[in] mode_idx the mode we are currently running on.
     */
    void Begin(int mode_idx);

    /**
     * @brief The mode index we are currently running on.
     * @return the current mode index.
     */
    int  Current() const { return current_; }

    /**
     * @brief Is a mode switch in progress?
     * @return true while a switch is being negotiated, armed, applied, or on
     *         probation (i.e. not yet confirmed by hearing the peer).
     */
    bool Busy() const {
        return target_ >= 0 || armed_ >= 0 || apply_ >= 0 || probation_;
    }

    /**
     * @brief Initiator only: ask to move BOTH ends to a new mode.
     *
     * No-op if we're already there or a switch is already in flight.
     *
     * @param[in] mode_idx the target mode index.
     */
    void Request(int mode_idx);

    /**
     * @brief Give up an in-flight request and stay on the current mode.
     *
     * Use when a requested switch can't complete (e.g. the ACK keeps getting
     * lost on a lossy link) — better to sit on the working-but-lossy mode
     * than dead-flap.
     */
    void Abort() { target_ = -1; armed_ = -1; owe_ack_ = -1; }

    /**
     * @brief The control nibble to stamp on this turn's outgoing frames.
     * @return the request/ACK nibble, or 0 if there's nothing to send.
     */
    uint8_t TxCtrl() const;

    /**
     * @brief Process a frame received this turn (drives the handshake).
     *
     * Receiving ANY frame proves the current PHY is alive, so it clears
     * probation; a control nibble may advance the request/ACK exchange.
     *
     * @param[in] rx           the received frame's control nibble.
     * @param[in] is_initiator true if this end is the initiator.
     * @param[in] now          current millis() (currently unused).
     */
    void AfterRecv(uint8_t rx, bool is_initiator, uint32_t now);

    /**
     * @brief Responder: call right after transmitting the turn.
     *
     * Promotes an armed switch so the ACK leaves on the OLD PHY before we
     * move.
     *
     * @param[in] now current millis() (currently unused).
     */
    void AfterSend(uint32_t now);

    /**
     * @brief Periodic tick: revert if probation elapsed unanswered.
     * @param[in] now current millis().
     */
    void Poll(uint32_t now);

    /**
     * @brief Take a due PHY change so the caller can reconfigure the radio.
     *
     * Starts probation on the new mode when a change is applied.
     *
     * @param[out] mode_idx receives the mode index to apply (if true).
     * @param[in]  now      current millis() (probation start time).
     * @return true if a PHY change is due; false otherwise.
     */
    bool TakeApply(int* mode_idx, uint32_t now);

private:
    int current_ = -1;   ///< Mode index we are currently running on.
    int prev_ = -1;      ///< Previous mode, to revert to if probation fails.
    int target_ = -1;    ///< Initiator: mode being requested (REQ in flight).
    int armed_ = -1;     ///< Responder: mode to switch to after we ACK + TX.
    int apply_ = -1;     ///< Mode change due for the caller to apply (>=0).
    int owe_ack_ = -1;   ///< Responder: mode we still owe an ACK for.
    bool probation_ = false;       ///< On probation: new PHY not yet confirmed.
    uint32_t probation_start_ = 0;  ///< millis() probation began.
};

} // namespace link_layer

#endif  // LINK_LAYER_MODESWITCH_H_
