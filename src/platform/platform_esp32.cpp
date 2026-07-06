/**
 * @file platform_esp32.cpp
 * @brief ESP32-S3 implementation of the platform abstraction (platform.h).
 *
 * Linked into the ESP32 (XIAO) node builds. The nRF52 (Wio Tracker L1) build
 * links platform_nrf52.cpp instead — the env's build_src_filter selects one.
 */
#include "platform.h"

#include <esp_mac.h>
#include <esp_random.h>

namespace platform {

// The factory Wi-Fi station MAC is a stable, unique-per-chip 6-byte id.
void DeviceId(uint8_t out[6]) { esp_read_mac(out, ESP_MAC_WIFI_STA); }

// The IDF hardware RNG (bootloader-seeded, RF-noise conditioned).
uint32_t Random32() { return esp_random(); }

}  // namespace platform
