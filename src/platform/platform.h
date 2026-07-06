/**
 * @file platform.h
 * @brief Platform abstraction — the handful of MCU-specific primitives the
 *        firmware needs, so the SAME src/fw_*.cpp compiles on both the ESP32-S3
 *        (XIAO node) and the nRF52840 (Wio Tracker L1). Exactly one
 *        implementation is linked per target: platform_esp32.cpp on the ESP32,
 *        platform_nrf52.cpp on the nRF52 (selected by the env's build filter).
 *
 * The portable link protocol lives in lib/linklayer; this covers only the
 * genuinely hardware-specific calls that were previously ESP-IDF direct. It is
 * being filled in incrementally as the nRF52 port proceeds — currently the
 * low-level primitives (device id, RNG); the persistent-settings store and the
 * software watchdog / crash breadcrumb follow.
 */
#ifndef LORA_SERIAL_PLATFORM_H_
#define LORA_SERIAL_PLATFORM_H_

#include <stdint.h>

/// MCU-specific primitives, implemented once per target (see file comment).
namespace platform {

/**
 * @brief Read this board's unique 6-byte hardware id.
 *
 * Stable across reboots and unique per chip; used for MAC-based half-duplex
 * role election and as the USB serial-number seed. On the ESP32 this is the
 * factory Wi-Fi MAC; on the nRF52 it is derived from the FICR device id.
 *
 * @param[out] out  six bytes written with the identifier. Must not be null.
 */
void DeviceId(uint8_t out[6]);

/**
 * @brief Return 32 bits of hardware randomness.
 *
 * Entropy for pairing key generation, discovery/listen-window jitter, and the
 * initial AEAD counter epoch. Backed by the on-chip RNG on both targets.
 *
 * @return 32 uniformly-distributed random bits.
 */
uint32_t Random32();

}  // namespace platform

#endif  // LORA_SERIAL_PLATFORM_H_
