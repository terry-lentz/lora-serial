/**
 * @file fw_diag.h
 * @brief Crash & health diagnostics: reset-reason / boot-count / pre-crash
 *        breadcrumb, heap watermarks, a hung-loop software watchdog.
 *
 * Surfaced to the user via the AT+DIAG command. The diagnostics layer is a
 * class (`Diag`), instantiated ONCE as the static global `g_diag` (no heap —
 * see CLAUDE.md rule 5). It owns the captured boot state (reset reason, boot
 * count, pre-crash uptime) and the software-watchdog state (the esp_timer
 * handle and the pet/miss counters) as private members. NOTE the crash
 * breadcrumb lives in RTC_NOINIT memory and CANNOT be a class member — see the
 * long comment in fw_diag.cpp for why. Implementation in fw_diag.cpp.
 */
#ifndef LORA_SERIAL_FW_DIAG_H_
#define LORA_SERIAL_FW_DIAG_H_

#include <stddef.h>

#include "esp_system.h"   // esp_reset_reason_t
#include "esp_timer.h"    // esp_timer_handle_t

// The diagnostics layer. ONE instance exists, the static global `g_diag`
// (defined in fw_diag.cpp) — never heap-allocated, so it is deterministic and
// fragmentation-free on a long-running radio (CLAUDE.md rule 5). It owns the
// captured boot state and the software-watchdog state and exposes the
// init/pet/report/reboot helpers the rest of the firmware calls.
class Diag {
 public:
  /**
   * @brief Capture last-reset state, bump the boot counter, start the watchdog.
   *
   * Call once near the end of setup() (after NVS/prefs is open): captures why
   * the board last reset, bumps the boot counter, and starts both the software
   * watchdog (a 1 Hz esp_timer counting missed pets) and the IDF Task Watchdog
   * backstop on the loop task.
   */
  void Init();

  /**
   * @brief "Pet" the watchdog and refresh the pre-crash uptime breadcrumb.
   *
   * Call every loop iteration AND from the radio-wait idle hook, so only a true
   * hang (the loop stops running for kWdtSecs) trips a reboot.
   */
  void Pet();

  /**
   * @brief Reboot NOW because the link made no progress (a radio wedge that the
   *        cheaper recoveries re-init/rendezvous couldn't clear).
   *
   * Leaves a breadcrumb so the next boot's AT+DIAG reports
   * lastreset=NO-PROGRESS. Does not return.
   */
  void RebootNoProgress();

  /**
   * @brief Format a human-readable diagnostics line into buf (for AT+DIAG).
   *
   * @param[out] buf  destination buffer. Must not be null.
   * @param[in]  cap  capacity of buf in bytes.
   * @return the length written (as snprintf would return).
   */
  size_t Report(char* buf, size_t cap);

 private:
  /**
   * @brief Reboot the board if the loop hasn't "petted" the watchdog in this
   *        long (seconds). Must exceed the longest legitimate blocking radio
   *        wait (TX safety ~8 s, SF12 listen ~13 s), so normal operation never
   *        trips it.
   */
  static constexpr uint32_t kWdtSecs = 25;

  /**
   * @brief Software-watchdog tick: a 1 Hz esp_timer callback counting missed
   *        pets; reboots the board if the loop hasn't petted in kWdtSecs.
   *
   * Static so it has the plain `void(*)(void*)` signature esp_timer requires;
   * it operates on the singleton through g_diag.
   *
   * @param[in] arg  unused esp_timer callback argument.
   */
  static void WdtTick(void* arg);

  /**
   * @brief Human-readable name of the last reset cause (for AT+DIAG).
   *
   * Const because it only reads the captured boot state.
   *
   * @return a static string naming the reset cause (e.g. "PANIC (crash)").
   */
  const char* ReasonStr() const;

  // ---- Captured-at-boot state (for reporting) -----------------------------
  esp_reset_reason_t reason_;        ///< raw last-reset reason from the IDF
  bool     was_wdt_ = false;         ///< last reset was our software watchdog
  bool     was_noprog_ = false;      ///< last reset was a no-progress reboot
  uint32_t prev_uptime_ms_ = 0;      ///< uptime reached before the last reset
  uint32_t boots_ = 0;               ///< boot count (persisted in NVS)

  // ---- Software-watchdog state (a 1 Hz esp_timer counts missed pets) -------
  esp_timer_handle_t wdt_timer_ = nullptr;   ///< null until Init() creates it
  volatile bool petted_ = true;              ///< set by Pet(), cleared per tick
  volatile uint32_t misses_ = 0;             ///< consecutive ticks with no pet
};

// The single diagnostics-layer instance (static singleton; no heap — CLAUDE.md
// rule 5). Defined in fw_diag.cpp.
extern Diag g_diag;

#endif  // LORA_SERIAL_FW_DIAG_H_
