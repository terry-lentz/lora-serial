/**
 * @file platform_nrf52.cpp
 * @brief nRF52840 implementation of the platform abstraction (platform.h).
 *
 * Linked into the Wio Tracker L1 build; the ESP32 (XIAO) build links
 * platform_esp32.cpp instead (selected by the env's build_src_filter).
 */
#include "platform.h"

#include <Arduino.h>   // Serial (Adafruit TinyUSB CDC), millis, nRF registers

namespace platform {

// The FICR holds two factory 32-bit device-id words, unique per chip; the low
// six bytes are a stable, unique hardware id.
void DeviceId(uint8_t out[6]) {
  const uint32_t id0 = NRF_FICR->DEVICEID[0];
  const uint32_t id1 = NRF_FICR->DEVICEID[1];
  out[0] = (uint8_t)id0;
  out[1] = (uint8_t)(id0 >> 8);
  out[2] = (uint8_t)(id0 >> 16);
  out[3] = (uint8_t)(id0 >> 24);
  out[4] = (uint8_t)id1;
  out[5] = (uint8_t)(id1 >> 8);
}

// The nRF RNG peripheral (hardware entropy). We do not enable the SoftDevice
// (no BLE), so the RNG is directly accessible; gather four random bytes.
uint32_t Random32() {
  NRF_RNG->TASKS_START = 1;
  uint32_t r = 0;
  for (int i = 0; i < 4; i++) {
    NRF_RNG->EVENTS_VALRDY = 0;
    while (NRF_RNG->EVENTS_VALRDY == 0) {
    }
    r = (r << 8) | (NRF_RNG->VALUE & 0xFF);
  }
  NRF_RNG->TASKS_STOP = 1;
  return r;
}

// No software watchdog on the nRF52 build yet — nothing to feed.
void WatchdogFeed() {}

// Clean software reset via the Cortex-M system reset request.
void Reboot() { NVIC_SystemReset(); }

// The Adafruit TinyUSB CDC RX FIFO is fixed at build time; nothing to resize.
void HostSetRxBufferSize(size_t bytes) { (void)bytes; }

// Device->host CDC over Adafruit TinyUSB's Serial. DTR gates output (see
// platform.h); availableForWrite() bounds each copy so a slow reader can't
// block the radio loop.
bool HostCdcConnected() { return Serial.dtr(); }
uint32_t HostCdcWriteAvailable() { return Serial.availableForWrite(); }
uint32_t HostCdcWrite(const uint8_t* buf, uint32_t len) {
  return (uint32_t)Serial.write(buf, len);
}
void HostCdcWriteFlush() { Serial.flush(); }

// Free RAM between the heap top and the stack (Adafruit nRF52 core helper).
uint32_t FreeInternalHeapKb() { return (uint32_t)dbgHeapFree() / 1024; }

// Battery voltage via the L1's 2:1 divider on PIN_VBAT (P0.31). BAT_READ
// (P0.04) gates the divider; we drive it HIGH once to enable the read. The
// SAADC runs 12-bit against the internal 0.6 V reference at 1/6 gain (3.6 V
// full scale — the variant's AREF_VOLTAGE), so the LiPo voltage is
// raw/4096 * AREF_VOLTAGE * ADC_MULTIPLIER. Callers should sample sparingly
// (a few times a second at most), not per display frame.
uint16_t BatteryMillivolts() {
  static bool init = false;
  if (!init) {
    analogReference(AR_INTERNAL);   // 0.6 V ref x 1/6 gain -> 3.6 V full scale
    analogReadResolution(12);       // 0..4095
    pinMode(BAT_READ, OUTPUT);
    digitalWrite(BAT_READ, HIGH);   // enable the resistor divider
    init = true;
  }
  uint32_t raw = (uint32_t)analogRead(PIN_VBAT);
  float mv = (float)raw / 4096.0f * AREF_VOLTAGE * ADC_MULTIPLIER * 1000.0f;
  return (uint16_t)(mv + 0.5f);
}

// No PSRAM on the nRF52: a fixed internal-RAM ring claimed once at boot (a
// pre-allocated static buffer — CLAUDE.md rule 5). `want` is ignored; the
// buffer is sized for the fallback case. 48 KiB fits comfortably in the
// 256 KiB SRAM alongside the link engine, rings, stacks, and USB buffers.
uint8_t* AllocIngest(size_t want, size_t fallback, size_t* out_cap) {
  (void)want;
  (void)fallback;
  static uint8_t s_ingest[48u * 1024];
  *out_cap = sizeof(s_ingest);
  return s_ingest;
}

}  // namespace platform
