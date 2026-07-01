/**
 * @file autopower.h
 * @brief Peer-SNR auto-power: the single source of truth for the TX-power
 *        feedback loop, shared verbatim by the firmware and the native sim.
 *
 * AUTO-POWER, IN PLAIN TERMS. Each radio measures how clearly it hears the
 * *other* end (the signal-to-noise ratio, SNR, of the peer's last frame) and
 * reports that one number back over the link's authenticated telemetry byte
 * (LinkLayer::SetAuxTx / AuxRx). When you read the peer's report you learn
 * "this is how well YOU come through to ME" — and you adjust YOUR OWN transmit
 * power to keep that a safe margin above the mode's demodulator floor (the
 * weakest SNR that mode can still decode):
 *   - peer hears you with lots of margin  -> ease your power down (save
 *     battery, avoid front-end saturation at close range);
 *   - peer is barely hearing you          -> push your power up.
 *
 * WHY PEER-SNR AND NOT OWN-RSSI. The older loop adjusted power from the *own*
 * received RSSI, which silently assumes the path loss is the same both ways. On
 * a real asymmetric bench link (different antennas/orientation each way, plus
 * an elevated close-range noise floor) that assumption breaks: a node that
 * hears its peer strongly floors its own power and *starves* the peer below its
 * demod SNR — the link dies while each side "thinks" it is healthy. Closing
 * the loop on the peer's reported SNR fixes exactly that (proven in
 * test/test_sim: test_autopower_peer_snr_holds_asym vs the broken own-RSSI
 * reference).
 *
 * This header is intentionally dependency-free (only <math.h>/<stdint.h>, no
 * Arduino) and header-only/inline, so the firmware and the native simulation
 * link the *identical* control law — one source of truth, no drift.
 */
#ifndef LINK_LAYER_AUTOPOWER_H_
#define LINK_LAYER_AUTOPOWER_H_

#include <math.h>     // lroundf — quantize a measured SNR into the aux byte
#include <stdint.h>   // int8_t/uint8_t fixed-width types for the wire byte

namespace link_layer {

/**
 * @brief Ease power off once the peer's SNR margin (dB above the mode's demod
 *        floor) exceeds this — the peer has plenty of headroom, so we can spend
 *        less power without risking the link.
 */
static const int kAutoPwrMarginHi = 12;
/**
 * @brief Boost power when the peer's SNR margin drops below this — the peer is
 *        getting marginal, so add power before frames start failing. The gap
 *        between Lo and Hi is deliberate hysteresis so the loop can't
 *        oscillate.
 */
static const int kAutoPwrMarginLo = 6;
/// per-adjustment change to TX power (dB); small so the loop settles smoothly
static const int8_t kAutoPwrStepDb = 2;

/**
 * @brief Decide the next TX power from the peer's reported SNR.
 *
 * Computes the peer's margin = peer_snr - snr_floor (how far above the mode's
 * demod floor the peer hears us) and nudges our power one step: down when the
 * margin is comfortably high (and we're above the power floor), up when it is
 * marginal (and we're below the power ceiling), holding otherwise. Returns the
 * unchanged power when nothing should move, so the caller can skip the radio
 * write. Pure and allocation-free (CLAUDE.md rule 5).
 *
 * @param[in] cur_tx     our current TX power (dBm).
 * @param[in] peer_snr   the SNR the peer reports for our signal (dB).
 * @param[in] snr_floor  the current mode's demodulator floor (dB).
 * @param[in] floor_dbm  lowest TX power the loop may drop to (dBm).
 * @param[in] max_dbm    highest TX power the loop may rise to (dBm).
 * @return the new TX power (dBm) to apply.
 */
inline int8_t AutoPowerStep(int8_t cur_tx, int peer_snr, int snr_floor,
                            int8_t floor_dbm, int8_t max_dbm) {
    int margin = peer_snr - snr_floor;   // dB the peer hears us above its floor
    if (margin > kAutoPwrMarginHi && cur_tx > floor_dbm)
        cur_tx -= kAutoPwrStepDb;        // peer has headroom -> ease off
    else if (margin < kAutoPwrMarginLo && cur_tx < max_dbm)
        cur_tx += kAutoPwrStepDb;        // peer is marginal -> boost
    return cur_tx;
}

/**
 * @brief Quantize a measured SNR into the link's 1-byte telemetry value.
 *
 * The aux byte carries a signed dB SNR; round to the nearest integer and clamp
 * to the int8_t range so the value survives the round-trip through the
 * authenticated header. The caller reads it back with AuxRx() and casts to
 * int8_t to recover the signed dB.
 *
 * @param[in] snr  the measured SNR in dB (e.g. radio.getSNR()).
 * @return the SNR quantized into the aux byte (a signed dB, stored unsigned).
 */
inline uint8_t SnrToAux(float snr) {
    if (snr > 127.0f) snr = 127.0f;      // clamp to the int8_t range so the
    if (snr < -128.0f) snr = -128.0f;    // dB value survives the wire byte
    return (uint8_t)(int8_t)lroundf(snr);
}

}  // namespace link_layer

#endif  // LINK_LAYER_AUTOPOWER_H_
