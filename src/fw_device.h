/**
 * @file fw_device.h
 * @brief Device orchestration: boot bring-up, role discovery + pairing, the
 *        half-duplex turn engine, layered link recovery, and the ADR loop.
 *
 * The device layer is a class (`Device`), instantiated ONCE as the static
 * global `g_device` (no heap — see CLAUDE.md rule 5). It owns the runtime role
 * (initiator vs responder), the pairing flag, the recovery clocks and counters,
 * and the turn loops' carry-over state — all private members. Arduino's
 * setup()/loop() (in main.cpp) are one-liners that call Setup()/Loop(); the
 * rest of the firmware reaches the role/telemetry it needs through the
 * accessors below. Implementation in fw_device.cpp.
 */
#ifndef LORA_SERIAL_FW_DEVICE_H_
#define LORA_SERIAL_FW_DEVICE_H_

#include <stddef.h>
#include <stdint.h>

#include "fw_config.h"   // MAC_ROLE / PROX_PAIR build toggles, shared globals

// The device-orchestration layer. ONE instance exists, the static global
// `g_device` (defined in fw_device.cpp) — never heap-allocated, so it is
// deterministic and fragmentation-free on a long-running radio (CLAUDE.md
// rule 5). It owns the runtime role, the pairing state, the recovery clocks /
// counters, and the turn loops' carry-over state as private members, and
// exposes the boot/loop entry points plus the role/telemetry helpers the host
// layer reads.
class Device {
 public:
  /**
   * @brief One-time boot bring-up: radio, settings, role, link, recovery.
   *
   * The body of Arduino's setup(). Sizes the USB RX buffer and claims the
   * PSRAM ingest ring, brings up the radio (retrying forever through escalating
   * resets so a wedged chip self-heals rather than hanging dark), loads NVS
   * settings, elects the role, seeds the mode-switch + ADR engines, resets the
   * session, starts the interrupt RX task, and — under MAC_ROLE — runs MAC
   * discovery or first-boot proximity pairing.
   */
  void Setup();

  /**
   * @brief One turn of the device: pet the watchdog, then run this role.
   *
   * The body of Arduino's loop(). Pets the software watchdog, then dispatches
   * to the initiator or responder turn engine by the elected role.
   */
  void Loop();

  /**
   * @brief Run first-boot / on-demand PROXIMITY pairing (see PROX_PAIR).
   *
   * Drops to kProxPairDbm so only an ADJACENT board is heard, elects the role
   * from the MAC, X25519-trains a unique per-pair key, persists role+key, then
   * restores full power. Run on BOTH boards placed close together. Blocks while
   * pairing (the host-I/O idle hook keeps USB serviced). Also invoked by
   * AT+PAIR for an on-demand re-pair.
   *
   * @return true on success; false if no adjacent peer was heard (or, in a
   *         non-MAC_ROLE build, always — that build has no MAC role election).
   */
  bool ProximityPair();

  // ---- Role / telemetry accessors (read by the host layer) -----------------
  /**
   * @brief Whether this board is the turn-driving initiator (lower address).
   * @return true on the initiator, false on the responder.
   */
  bool initiator() const { return initiator_; }
  /**
   * @brief Whether a proximity-pairing exchange is in progress, surfaced as
   *        state=pairing by ATI (and used to drive the pairing LED pattern).
   * @return true while pairing, false in normal operation.
   */
  bool pairing() const { return pairing_; }
  /**
   * @brief Radio re-init count this boot (the cheapest recovery, run by
   *        MaybeReinitRadio); surfaced by AT+LINK? so recoveries are
   *        observable.
   * @return the number of radio re-inits since boot.
   */
  uint32_t reinit_count() const { return reinit_count_; }
  /**
   * @brief Rendezvous-fallback count this boot (the mid-tier recovery, run by
   *        MaybeRendezvous); surfaced by AT+LINK? alongside reinit_count().
   * @return the number of rendezvous fallbacks since boot.
   */
  uint32_t rendezvous_count() const { return rendezvous_count_; }
  /**
   * @brief millis() timestamp of the last VALID received frame (real link
   *        traffic, not noise). Used by auto-power to tell when the link has
   *        gone silent so it can ramp TX power back up (fw_host AdjustTxPower).
   * @return the last-valid-RX time in millis(), or 0 if none since boot.
   */
  uint32_t last_rx_ms() const { return last_rx_ms_; }
  /**
   * @brief Experimental inter-frame TX pacing (ms) inside a burst, set via
   *        AT+TXGAP; a deliberate gap between frames for field experiments.
   * @return the inter-frame pacing in milliseconds (0 = off).
   */
  uint32_t tx_gap() const { return tx_gap_ms_; }

  /**
   * @brief Recompute the initiator role from the current address/peer config.
   *
   * The lower address initiates turns: initiator = (peer==0) ? (addr==1) :
   * (addr < peer). Call after AT+ADDR / AT+PEER / AT&F change the address.
   */
  void RecomputeRole();

  /**
   * @brief Set the experimental inter-frame TX pacing inside a burst
   *        (AT+TXGAP).
   * @param[in] ms  the inter-frame pacing in milliseconds (0 = off).
   */
  void SetTxGap(uint32_t ms) { tx_gap_ms_ = ms; }

 private:
  // ---- Turn engine (one binary runs both roles; Loop() picks by role) ------
  /**
   * @brief Initiator turn: send our burst (or a poll), then listen for the
   *        responder.
   */
  void LoopInitiator();

  /**
   * @brief Responder turn: wait for the initiator's burst, then send ours
   *        (data or a poll/ack).
   */
  void LoopResponder();

  /**
   * @brief Apply any PHY change the mode-switch engine has decided.
   *
   * Carries out a committed switch or a probation revert; idle-safe (does
   * nothing unless a switch is in flight).
   */
  void ServiceModeSwitch();

  // ---- Layered link recovery (radio-stuck < rendezvous < reboot) -----------
  /**
   * @brief Dead-link mode rendezvous: after long silence, fall back to a
   *        known-good mode so two diverged ends re-converge ('auto' only).
   */
  void MaybeRendezvous();

  /**
   * @brief Radio-stuck watchdog: after silence, re-init the radio at the
   *        current mode to unwedge a deaf chip.
   *
   * Runs in all modes and preserves the link/ARQ/session.
   */
  void MaybeReinitRadio();

  /**
   * @brief No-progress reboot: the last resort if a hard wedge survives re-init
   *        and rendezvous on a link that WAS up this boot.
   */
  void MaybeReboot();

  /**
   * @brief Scale a recovery floor by the mode's airtime.
   *
   * Returns max(floor, mult * ToA), so the watchdogs stay eager on fast modes
   * and patient on slow ones.
   *
   * @param[in] floor_ms  the minimum timeout (ms) for fast modes.
   * @param[in] mult      multiplier applied to the per-mode retransmit timeout.
   * @return the scaled recovery timeout in milliseconds.
   */
  uint32_t RecoveryMs(uint32_t floor_ms, uint32_t mult);

  // ---- Initiator-driven ADR ('auto' mode) ----------------------------------
  /**
   * @brief Drive adaptive data rate: gather the live link inputs, ask the
   *        portable AdrController what to do, and carry it out.
   *
   * Acts through the mode-switch handshake. A no-op unless FEAT_ADR is enabled.
   */
  void DriveAdr();

  // ---- MAC / proximity role discovery (MAC_ROLE builds) --------------------
#if MAC_ROLE
  /**
   * @brief Transmit one cleartext PHY discovery beacon ([magic][MAC]).
   */
  void SendDiscBeacon();

  /**
   * @brief Block until we hear the peer's beacon and elect a role.
   *
   * Services host I/O while waiting, then sets addr/peer/initiator from the MAC
   * compare. Runs after the radio is armed.
   */
  void DiscoverRole();
#endif
  /**
   * @brief In the normal loop: reply to a peer still in discovery so it can
   *        elect.
   *
   * Compiled in both builds (the non-MAC_ROLE version always returns false).
   *
   * @param[in] rx  the received frame bytes. Must not be null.
   * @param[in] rl  the received length in bytes.
   * @return true if the bytes were a discovery beacon (and were handled).
   */
  bool DiscHandleRx(const uint8_t* rx, size_t rl);

  /**
   * @brief Recover a hung SX1262 without a power cycle.
   *
   * Drives long NRST pulses plus an NSS wake-from-sleep edge (the SX126x wakes
   * on an NSS falling edge, not reliably on NRST).
   */
  void RadioHardNrst();

  // ---- Role / pairing state (was module-private globals) -------------------
  /**
   * @brief True if this board drives turns (the lower address). Set from the
   *        config in Setup()/RecomputeRole() and from MAC election in
   *        DiscoverRole().
   */
  bool     initiator_ = true;
  /// true while a proximity-pairing exchange is in progress (ATI/status/LED)
  bool     pairing_ = false;

  // ---- Recovery counters + clocks (this boot) ------------------------------
  uint32_t reinit_count_ = 0;      ///< radio re-inits this boot (AT+LINK?)
  uint32_t rendezvous_count_ = 0;  ///< rendezvous fallbacks this boot (LINK?)
  /// experimental inter-frame TX pacing (ms) inside a burst; 0 = off (AT+TXGAP)
  uint32_t tx_gap_ms_ = 0;
  /**
   * @brief THE silence clock: millis() of the last VALID received frame (drives
   *        all three recovery escalations). 0 until the link first comes up
   *        this boot.
   */
  uint32_t last_rx_ms_ = 0;
  /// rate-limit clock: last rendezvous fallback (acts once per its timeout)
  uint32_t last_rendezvous_ms_ = 0;
  /// rate-limit clock: last radio re-init (acts once per its timeout)
  uint32_t last_reinit_ms_ = 0;

  // ---- Turn-loop carry-over (were function-static locals) ------------------
  uint32_t li_last_turn_ = 0;      ///< initiator: time of the last turn
  uint32_t li_idle_gap_ = 0;       ///< initiator: idle backoff gap (seed:Setup)
  uint32_t li_last_activity_ = 0;  ///< initiator: last activity (poll hold)
  /**
   * @brief Responder: the last-seen host_in() value, with lr_host_in_at_ the
   *        time it last changed — used to detect an interactive (vs one-way
   *        bulk) host for the reply-piggyback heuristic.
   */
  uint32_t lr_host_in_val_ = 0;
  uint32_t lr_host_in_at_ = 0;     ///< responder: when host_in() last changed

#if MAC_ROLE
  /**
   * @brief This board's factory MAC, read once in DiscoverRole(); the role
   *        election and the discovery beacon both use it.
   */
  uint8_t  my_mac_[6];
#endif
};

// The single device-orchestration instance (static singleton; no heap —
// CLAUDE.md rule 5). Defined in fw_device.cpp.
extern Device g_device;

#endif  // LORA_SERIAL_FW_DEVICE_H_
