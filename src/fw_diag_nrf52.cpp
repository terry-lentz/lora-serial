/**
 * @file fw_diag_nrf52.cpp
 * @brief nRF52 implementation of the Diag class (see fw_diag.h).
 *
 * A minimal but functional diagnostics layer for the Wio Tracker L1: the
 * last-reset cause (decoded from POWER->RESETREAS), a persisted boot count, and
 * the AT+DIAG report. The ESP32 build's software watchdog and RTC crash
 * breadcrumb are not ported yet — the link runs fine without them; AT+DIAG
 * still reports the essentials. (The ESP32 version lives in fw_diag_esp32.cpp.)
 */
#include "fw_diag.h"

#include <Arduino.h>   // millis, NRF_POWER, NVIC_SystemReset

#include "platform/platform.h"  // platform::FreeInternalHeapKb
#include "platform/prefs.h"     // Preferences (NVS-style store)

/// Settings store; opened ("loramodem") in Host::LoadSettings(). Defined in
/// main.cpp. On the nRF52 it is currently the defaults-only stub (see prefs.h).
extern Preferences prefs;

// The single diagnostics-layer instance (static singleton; no heap — rule 5).
Diag g_diag;

// Radio-op breadcrumbs (declared extern in fw_config.h). On the nRF52 these are
// plain globals — no reset-surviving RTC memory is used yet — so the RX/loop
// tasks stamp them and AT+DIAG reports the live values.
volatile uint8_t g_dbg_stage;
volatile uint8_t g_dbg_rx;

void Diag::Init() {
  reason_ = NRF_POWER->RESETREAS;
  NRF_POWER->RESETREAS = 0xFFFFFFFFu;   // write-1-to-clear, ready for next boot
  boots_ = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", boots_);
  // No software watchdog on the nRF52 yet.
}

void Diag::Pet() { petted_ = true; }

void Diag::RebootNoProgress() { NVIC_SystemReset(); }

size_t Diag::Report(char* buf, size_t cap) {
  return (size_t)snprintf(
      buf, cap, "boots=%lu lastreset=%s uptime=%lus iram=%luK\r\n",
      (unsigned long)boots_, ReasonStr(),
      (unsigned long)(millis() / 1000),
      (unsigned long)platform::FreeInternalHeapKb());
}

// Present to satisfy the portable Diag interface; unused here (there is no
// periodic software-watchdog timer on the nRF52 build yet).
void Diag::WdtTick(void*) {}

// Decode the nRF POWER->RESETREAS bits captured in Init(). RESETREAS is 0 after
// a clean power-on; otherwise a set bit names the cause.
const char* Diag::ReasonStr() const {
  if (reason_ == 0) return "power-on";
  if (reason_ & (1u << 1)) return "watchdog";
  if (reason_ & (1u << 2)) return "software (reset req)";
  if (reason_ & (1u << 0)) return "pin reset";
  if (reason_ & (1u << 3)) return "CPU lockup";
  return "other";
}
