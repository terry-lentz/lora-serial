/**
 * @file platform_esp32.cpp
 * @brief ESP32-S3 implementation of the platform abstraction (platform.h).
 *
 * Linked into the ESP32 (XIAO) node builds. The nRF52 (Wio Tracker L1) build
 * links platform_nrf52.cpp instead — the env's build_src_filter selects one.
 */
#include "platform.h"

#include <stdlib.h>

#include <Arduino.h>             // ps_malloc
#include <esp32-hal-tinyusb.h>  // tud_cdc_* (device->host CDC IN endpoint)
#include <esp_heap_caps.h>      // heap_caps_get_free_size, MALLOC_CAP_INTERNAL
#include <esp_mac.h>
#include <esp_random.h>

namespace platform {

// The factory Wi-Fi station MAC is a stable, unique-per-chip 6-byte id.
void DeviceId(uint8_t out[6]) { esp_read_mac(out, ESP_MAC_WIFI_STA); }

// The IDF hardware RNG (bootloader-seeded, RF-noise conditioned).
uint32_t Random32() { return esp_random(); }

// Feed the Arduino loop task's IDF Task Watchdog (armed in Diag::Init).
void WatchdogFeed() { feedLoopWDT(); }

// Clean software reset.
void Reboot() { ESP.restart(); }

// The ESP32 USB-CDC RX FIFO is resizable; a large buffer absorbs host bursts.
void HostSetRxBufferSize(size_t bytes) { Serial.setRxBufferSize(bytes); }

// Device->host CDC, at the TinyUSB level (see platform.h for why not Serial).
bool HostCdcConnected() { return tud_cdc_connected(); }
uint32_t HostCdcWriteAvailable() { return tud_cdc_write_available(); }
uint32_t HostCdcWrite(const uint8_t* buf, uint32_t len) {
  return tud_cdc_write(buf, len);
}
void HostCdcWriteFlush() { tud_cdc_write_flush(); }

// Internal SRAM is the scarce, crash-relevant pool; report its free size.
uint32_t FreeInternalHeapKb() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
}

// Prefer a 2 MB PSRAM ring; fall back to internal RAM. Claimed once at boot.
uint8_t* AllocIngest(size_t want, size_t fallback, size_t* out_cap) {
  uint8_t* p = (uint8_t*)ps_malloc(want);
  if (p) {
    *out_cap = want;
    return p;
  }
  p = (uint8_t*)malloc(fallback);
  *out_cap = p ? fallback : 0;
  return p;
}

}  // namespace platform
