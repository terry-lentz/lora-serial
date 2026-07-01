/**
 * @file fw_radio.h
 * @brief Radio layer: SX1262 control, the named range/speed modes, and the
 *        serviced transmit/receive path, behind a static-singleton `Radio`.
 *
 * The radio layer is a class (`Radio`), instantiated ONCE as the static global
 * `g_radio` (no heap — see CLAUDE.md rule 5). It owns the radio's own state:
 * the interrupt-driven RX path (the SPSC ring, the radio mutex, the RX task),
 * the derived turn timing, the TX power, the PHY (LoRa/GFSK), and the smoothed
 * RSSI — all private members, read elsewhere through the accessors below.
 * Implementation in fw_radio.cpp.
 */
#ifndef LORA_SERIAL_FW_RADIO_H_
#define LORA_SERIAL_FW_RADIO_H_

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "fw_config.h"
#include "frame_ring.h"   // SPSC ring for the interrupt-driven RX path

/**
 * @brief Activity LED pin — green D1 on the Wio-SX1262 board, GPIO48 via 330R,
 *        active HIGH (per the board schematic). Toggled on every TX-done /
 *        RX-done event, so it flickers during traffic and is steady when idle.
 */
#define LED_PIN 48

// Forward declaration: the ADR ladder type lives in lib/linklayer/adr.h. We
// only take a pointer to it here, so a forward declare keeps this header light.
namespace link_layer { struct AdrLadder; }

// The radio layer. ONE instance exists, the static global `g_radio` (defined in
// fw_radio.cpp) — never heap-allocated, so it is deterministic and
// fragmentation-free on a long-running radio (CLAUDE.md rule 5). It owns the
// interrupt-driven RX path's internal state (the SPSC ring, the radio mutex,
// the RX task handle, the armed flag) and exposes the transmit/receive,
// live-config, and named-mode helpers the rest of the firmware calls.
class Radio {
 public:
  /**
   * @brief Toggle the activity LED. Call only from normal (non-ISR) context.
   *
   * The LED is driven here, NOT in the DIO1 ISR: digitalWrite() is
   * flash-resident and calling it from the IRAM ISR under heavy interrupt load
   * crashes the chip, so the ISR only sets a flag and the LED flips here.
   */
  void LedBlink();

  /**
   * @brief Apply a radio config live (SF/BW/CR) and re-derive turn timing.
   *
   * Reconfigures the radio to the given LoRa parameters, then re-derives the
   * per-mode turn timing and BDP window from the resulting time-on-air, so the
   * half-duplex schedule self-tunes across modes.
   *
   * @param[in] sf       LoRa spreading factor (e.g. 5..12).
   * @param[in] bw_code  bandwidth code: 0=125, 1=250, 2=500 kHz.
   * @param[in] cr       LoRa coding rate denominator (e.g. 5..8 for 4/5..4/8).
   */
  void ApplyMode(uint8_t sf, uint8_t bw_code, uint8_t cr);

  /**
   * @brief Force a FULL radio re-init at the CURRENT mode (recovery path).
   *
   * Does a real begin()/beginFSK() (not ApplyMode's incremental path) plus an
   * NRST hardware reset, to recover a WEDGED/deaf radio. It resets the chip's
   * state machine to bring RX back while preserving the mode and the
   * link/ARQ/session state. Used by the radio-stuck watchdog in main.cpp.
   */
  void Reinit();

  /**
   * @brief Start the interrupt-driven RX task and arm continuous RX.
   *
   * Creates the radio mutex + high-priority RX task and arms continuous RX
   * (the INTR_RX path). Call ONCE from setup() after the radio is configured.
   */
  void StartTask();

  /**
   * @brief Name of the current SF/BW/CR preset.
   *
   * @return the matching preset name (e.g. "medium"), or "custom" if the live
   *         SF/BW/CR don't match any named preset. The string is a static
   *         literal owned by the mode table — do not free it.
   */
  const char* CurrentModeName();

  /**
   * @brief Switch to a named mode (turbo/fast/medium/slow/far/ludicrous).
   *
   * Sets SF/BW/CR for the named preset, reconfigures the radio, and re-derives
   * timing. Both ends must use the same mode.
   *
   * @param[in] name  the preset name (case-insensitive). Must not be null.
   * @return false on an unknown name; true on success.
   */
  bool ApplyModeByName(const char* name);

  /**
   * @brief Number of named presets in the radio mode table.
   *
   * @return the count of entries in the mode table.
   */
  int RfModeCount();

  /**
   * @brief Index of a named preset in the radio mode table.
   *
   * @param[in] name  the preset name (case-insensitive). Must not be null.
   * @return the table index, or -1 if the name is unknown.
   */
  int ModeIndexByName(const char* name);

  /**
   * @brief Index of the live SF/BW/CR (or GFSK) in the radio mode table.
   *
   * @return the matching table index, or -1 if the live config is custom.
   */
  int CurrentModeIndex();

  /**
   * @brief Apply the preset at a table index (radio + link timing).
   *
   * Configures the radio to that preset and re-pushes the derived per-mode
   * timing to the link, WITHOUT re-initialising the link (so the
   * epoch/counter/ARQ/session survive a seamless switch).
   *
   * @param[in] idx  the mode-table index to apply.
   * @return false if idx is out of range; true on success.
   */
  bool ApplyModeByIndex(int idx);

  /**
   * @brief Fill an AdrLadder from the live radio mode table.
   *
   * Bridges the firmware's SF/BW/CR table to the portable, unit-tested ADR
   * controller: it populates the ladder (counts, SNR floors, GFSK flags, auto
   * eligibility, margin) so the pure ADR logic can make decisions over it. This
   * is the only place that knows the actual SF/BW/CR table.
   *
   * @param[out] lad  the ladder to fill. Must not be null. The ladder keeps
   *                  pointers into static arrays owned by this method, so it
   *                  stays valid for the program's lifetime.
   */
  void BuildAdrLadder(link_layer::AdrLadder* lad);

  /**
   * @brief Receive one frame, waiting up to a wall-clock timeout.
   *
   * Pops the next frame the RX task drained into the SPSC ring; never polls or
   * standbys the radio (the task owns RX). Leaves the radio in continuous RX.
   *
   * @param[in]  buf         destination buffer. Must not be null.
   * @param[in]  max_len     capacity of buf in bytes.
   * @param[out] out_len     set to the received frame length on success.
   * @param[in]  timeout_ms  how long to wait for a frame, in milliseconds.
   * @return RADIOLIB_ERR_NONE on a frame, RADIOLIB_ERR_RX_TIMEOUT on timeout.
   */
  int16_t Rx(uint8_t* buf, size_t max_len, size_t& out_len,
             uint32_t timeout_ms);

  /**
   * @brief Transmit a frame while still servicing host I/O.
   *
   * radio.transmit() blocks for the whole time-on-air (~0.5 s at SF8); a
   * serviced wait (calling the idle hook) keeps host I/O draining throughout,
   * so the USB-CDC RX buffer can't overflow and drop host bytes.
   *
   * @param[in] buf  the frame bytes to transmit. Must not be null.
   * @param[in] len  the frame length in bytes.
   */
  void Tx(const uint8_t* buf, size_t len);

  /**
   * @brief DIO1 interrupt handler (TX-done / RX-done). Lives in IRAM.
   *
   * Static so it has the plain `void(*)()` signature RadioLib's
   * setDio1Action() requires; it touches the singleton through `g_radio`. It
   * sets the done-flag and wakes the RX task, and NOTHING else (no
   * flash-resident calls), so it is IRAM-safe under heavy interrupt load.
   */
  static void OnDio1();

  // ---- Derived turn timing (written only in DeriveTiming) ------------------
  /**
   * @brief Per-frame airtime (ms) of a full frame at the current mode; the raw
   *        measured quantity the other timings scale off (e.g. AT+SPEEDTEST).
   * @return the per-frame time-on-air in milliseconds.
   */
  uint32_t toa_ms() const { return toa_ms_; }
  /**
   * @brief Interframe re-arm window (ms): > one full-frame ToA + the RX re-arm
   *        slack, so the receiver is listening before the next frame arrives.
   * @return the interframe re-arm window in milliseconds.
   */
  uint32_t interframe_ms() const { return interframe_ms_; }
  /**
   * @brief Per-frame burst RX deadline (ms), reset on each received frame, used
   *        to bound how long a turn waits for the next frame in a burst.
   * @return the per-frame burst RX deadline in milliseconds.
   */
  uint32_t turn_rx_ms() const { return turn_rx_ms_; }
  /**
   * @brief Retransmit timeout (ms): > a full window-burst round-trip time, so
   *        the ARQ waits a real RTT before resending unacked frames.
   * @return the retransmit timeout in milliseconds.
   */
  uint32_t retransmit_ms() const { return retransmit_ms_; }
  /**
   * @brief Responder idle-listen window (ms): must span one frame's ToA
   *        (~13 s on SF12) so the responder doesn't end its turn mid-frame.
   * @return the responder idle-listen window in milliseconds.
   */
  uint32_t listen_ms() const { return listen_ms_; }
  /**
   * @brief BDP-sized burst window: how many frames are sent before an ACK,
   *        sized to fill the bandwidth-delay product of the current mode.
   * @return the burst window size in frames.
   */
  uint8_t window() const { return window_; }

  /**
   * @brief Current TX power setting (dBm).
   *
   * @return the TX power last applied via the constructor default or
   *         SetTxPower(). Save this before a temporary power change and pass it
   *         back to SetTxPower() to restore.
   */
  int8_t tx_power() const { return tx_power_; }

  /**
   * @brief Set the TX power and push it to the radio chip.
   *
   * Records the new power AND calls radio.setOutputPower(), so callers no
   * longer touch the chip directly. VERIFY the value is legal for your band
   * before field use.
   *
   * @param[in] dbm  the TX power to apply, in dBm.
   */
  void SetTxPower(int8_t dbm);

  /**
   * @brief Set the carrier frequency and push it to the SX1262.
   *
   * setFrequency() drives the radio over SPI, as does the interrupt RX task, so
   * this takes the recursive radio mutex to avoid racing it on the bus (the
   * unlocked race wedged the chip under load). Both ends must agree.
   *
   * @param[in] mhz  the carrier frequency to apply, in MHz.
   */
  void SetFrequency(float mhz);

  /**
   * @brief Set the LoRa sync word and push it to the SX1262.
   *
   * Like SetFrequency(), takes the recursive radio mutex so it can't race the
   * RX task's SPI access. A coarse network filter; both ends must match.
   *
   * @param[in] sync  the 1-byte LoRa sync word to apply.
   */
  void SetSyncWord(uint8_t sync);

  /**
   * @brief Current PHY modulation: which family of radio settings is live.
   * @return false for LoRa, true for GFSK (the 'ludicrous' mode).
   */
  bool phy_fsk() const { return phy_fsk_; }
  /**
   * @brief Smoothed received-signal strength (dBm) from the RX path's RSSI EMA;
   *        consumed by AT+LINK? and the auto-power loop.
   * @return the smoothed RSSI in dBm (only meaningful once have_rssi()).
   */
  float rssi() const { return rssi_ema_; }
  /**
   * @brief Whether the smoothed RSSI is yet valid (a sample has been seen).
   * @return true once at least one RSSI sample has been observed.
   */
  bool have_rssi() const { return have_rssi_; }
  /**
   * @brief SNR (dB) of the last frame the RX task received, cached under the
   *        radio mutex; consumed by AT+LINK?/speedtest, ADR, and peer-SNR
   *        auto-power. Reading the cached value avoids an unlocked getSNR() SPI
   *        call that would race the RX task and wedge the chip.
   * @return the last received frame's SNR in dB (0 until the first frame).
   */
  float snr() const { return last_snr_; }

  /**
   * @brief Current LoRa mode's demodulator SNR floor (dB), for auto-power.
   *
   * The peer-SNR auto-power loop holds a margin above this floor. Returns the
   * floor for the live LoRa preset; for GFSK or an unrecognized/custom config
   * it returns 0 (auto-power skips GFSK, so the value is then unused).
   *
   * @return the current mode's demod SNR floor in dB, or 0 if not a LoRa mode.
   */
  int snr_floor() const;

  /**
   * @brief Demod floor (dB) of the LoRa rung one faster than the current mode.
   *
   * For coordinated auto-power headroom under ADR: the loop targets this so it
   * holds enough SNR for ADR to climb, instead of minimizing for the current
   * mode (see link_layer::AutoPowerTargetFloor). There is no faster rung when
   * already on the fastest LoRa preset (turbo) or on GFSK/custom — then this
   * returns false and out_floor is left untouched.
   *
   * @param[out] out_floor set to the next-faster rung's floor (dB) when one
   *                       exists; unchanged otherwise.
   * @return true if a faster LoRa rung exists (out_floor valid), else false.
   */
  bool next_faster_snr_floor(int& out_floor) const;

  /**
   * @brief Re-apply the shared post-begin() radio setup that any
   *        begin()/beginFSK() resets: RF switch, DIO1 IRQ, optional LoRa CRC,
   *        and the bounded SPI BUSY-line timeout. Call after every begin()
   *        AND once at initial bring-up so the timeout is active from boot.
   *
   * @param[in] lora_crc  true to enable the LoRa packet CRC (LoRa modes only).
   */
  void RadioCommonSetup(bool lora_crc);

 private:
  /**
   * @brief RX ring slot count (usable capacity = kRxRingSlots - 1). The
   *        DIO1-woken task pushes; Rx() pops. Sized so a back-to-back burst
   *        can't overrun the FIFO.
   */
  static const size_t kRxRingSlots = 16;

  /**
   * @brief Take the recursive radio-access (SPI) mutex.
   *
   * Recursive so nested radio calls (e.g. Reinit -> ApplyRadioFsk) don't
   * self-deadlock. No-op before the mutex is created (setup()'s first radio
   * config runs before StartTask()).
   */
  void Lock();

  /**
   * @brief Release the recursive radio-access mutex (no-op before it exists).
   */
  void Unlock();

  /**
   * @brief Re-derive all turn-taking timing + the BDP window from the current
   *        PHY's time-on-air, so every mode (LoRa or GFSK) self-tunes.
   */
  void DeriveTiming();

  /**
   * @brief Switch the SX1262 into GFSK modulation (the 'ludicrous' mode).
   */
  void ApplyRadioFsk();

  /**
   * @brief High-priority radio task: drain the SX1262 FIFO on each DIO1 event.
   *
   * Woken by the DIO1 ISR, it drains the FIFO into the SPSC ring under the
   * radio mutex, then re-arms RX after RX_DONE. Static so it has the FreeRTOS
   * `void(*)(void*)` entry signature; it operates on the singleton through
   * `g_radio`.
   *
   * @param[in] arg  unused FreeRTOS task argument.
   */
  static void RadioTask(void* arg);

  // ---- Interrupt-driven RX path state (was module-private file statics) -----
  volatile bool led_state_ = false;       ///< activity LED state (LedBlink())
  /// set by the DIO1 ISR on a TX-done / RX-done edge; Tx() watches it
  volatile bool operation_done_ = false;
  /**
   * @brief Buffered RX frames (capacity kRxRingSlots - 1); the DIO1-woken task
   *        pushes, Rx() pops. Sized so a burst can't overrun the FIFO.
   */
  link_layer::FrameRing<kRxRingSlots, link_layer::MAXFRAME> rx_ring_;
  /**
   * @brief Recursive mutex guarding ALL radio SPI access (RX task vs main-loop
   *        TX/arm/reconfig). Null until StartTask() creates it.
   */
  SemaphoreHandle_t radio_mutex_ = nullptr;
  /**
   * @brief Handle of the high-priority RX task; the ISR notifies it. Null until
   *        StartTask() creates it.
   */
  TaskHandle_t radio_task_ = nullptr;
  /// true while the radio is in continuous RX (so the RX task may re-arm it)
  volatile bool rx_armed_ = false;

  // ---- Derived turn timing (written only in DeriveTiming) ------------------
  // These are derived per-mode from the time-on-air so the half-duplex
  // schedule self-tunes across SF/BW (no hand-tuned constants per mode). The
  // defaults are for ~SF7/BW250 until DeriveTiming() re-derives them. See the
  // tuning constants in fw_radio.cpp for how each is computed.
  uint32_t toa_ms_        = 90;     ///< per-frame airtime (medium default)
  uint32_t interframe_ms_ = 320;    ///< > one full-frame ToA + RX re-arm
  uint32_t turn_rx_ms_    = 2000;   ///< per-frame burst deadline (reset/frame)
  uint32_t retransmit_ms_ = 1500;   ///< > a full window-burst RTT
  uint32_t listen_ms_     = 3000;   ///< responder idle-listen window
  uint8_t  window_        = 8;      ///< BDP-sized burst window (frames)

  // ---- TX power / PHY / RSSI ----------------------------------------------
  /**
   * @brief Current TX power (dBm); set via SetTxPower(), which also pushes it
   *        to the chip. Defaults to the configured maximum (kTxPowerDbm).
   */
  int8_t tx_power_ = kTxPowerDbm;
  /**
   * @brief Current PHY modulation: false = LoRa, true = GFSK ('ludicrous').
   *        Switching re-inits the radio (begin vs beginFSK).
   */
  bool phy_fsk_ = false;
  /**
   * @brief Smoothed received-signal strength (dBm), an EMA the RX task updates
   *        per frame; consumed by AT+LINK?/auto-power. -80 dBm until the first
   *        sample.
   */
  float rssi_ema_ = -80.0f;
  /// true once at least one RSSI sample has been seen (rssi_ema_ is valid)
  bool have_rssi_ = false;
  /// SNR (dB) of the last received frame, cached by the RX task under the
  /// radio mutex (read elsewhere via snr(), never with an unlocked getSNR())
  float last_snr_ = 0.0f;
};

// The single radio-layer instance (static singleton; no heap — CLAUDE.md
// rule 5). Defined in fw_radio.cpp.
extern Radio g_radio;

#endif  // LORA_SERIAL_FW_RADIO_H_
