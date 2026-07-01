/**
 * @file modeswitch.cpp
 * @brief Implementation of the PHY mode-switch handshake from modeswitch.h.
 *
 * The request/ACK/probation state transitions of the make-before-break PHY
 * change. See modeswitch.h for the protocol overview and the per-method
 * contracts.
 */
#include "modeswitch.h"

namespace link_layer {

void ModeSwitch::Begin(int mode_idx) {
    current_ = prev_ = mode_idx;
    target_ = armed_ = apply_ = owe_ack_ = -1;
    probation_ = false; probation_start_ = 0;
}

void ModeSwitch::Request(int mode_idx) {
    if (mode_idx == current_ || Busy()) return;
    target_ = mode_idx;
}

uint8_t ModeSwitch::TxCtrl() const {
    if (owe_ack_ >= 0) return CtrlEncode(owe_ack_, true);   // responder ACK
    if (target_  >= 0) return CtrlEncode(target_, false);   // initiator REQ
    return 0;
}

void ModeSwitch::AfterRecv(uint8_t rx, bool is_initiator, uint32_t now) {
    (void)now;
    probation_ = false;                       // heard the peer -> PHY good
    if (!CtrlPresent(rx)) return;
    int m = CtrlModeIdx(rx);
    bool ack = CtrlIsAck(rx);
    if (is_initiator) {
        if (ack && m == target_) {            // peer accepted our request
            apply_ = m; target_ = -1;         // we switch after this turn
        }
    } else {
        if (!ack && m != current_ && armed_ < 0) {
            owe_ack_ = m; armed_ = m;          // ACK now, switch after TX
        }
    }
}

void ModeSwitch::AfterSend(uint32_t now) {
    (void)now;
    if (armed_ >= 0) { apply_ = armed_; armed_ = -1; owe_ack_ = -1; }
}

void ModeSwitch::Poll(uint32_t now) {
    if (probation_ &&
        (uint32_t)(now - probation_start_) >= probation_ms) {
        probation_ = false;
        apply_ = prev_;                        // fall back to the old mode
    }
}

bool ModeSwitch::TakeApply(int* mode_idx, uint32_t now) {
    if (apply_ < 0) return false;
    int m = apply_; apply_ = -1;
    if (m != current_) { prev_ = current_; current_ = m; }
    probation_ = true; probation_start_ = now;
    *mode_idx = m;
    return true;
}

} // namespace link_layer
