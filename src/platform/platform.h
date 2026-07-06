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

#include <stddef.h>
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

// ---- Host USB-CDC output (device -> host) --------------------------------
// The transparent data plane writes to the CDC IN endpoint at this low level
// (not Arduino Serial.write) so a slow reader can never block the radio loop:
// HostCdcWriteAvailable() bounds each copy, and output is gated on the terminal
// actually being open (DTR) — writing before a host opens the port can wedge
// the pipe. Backed by TinyUSB on both targets.

/**
 * @brief Whether the host has the CDC port open (DTR asserted).
 * @return true once a terminal is attached and reading.
 */
bool HostCdcConnected();

/**
 * @brief Bytes the CDC IN endpoint can accept right now without blocking.
 * @return the free space in the USB TX FIFO.
 */
uint32_t HostCdcWriteAvailable();

/**
 * @brief Enqueue bytes to the host over CDC (non-blocking, up to FIFO room).
 * @param[in] buf  bytes to send. Must not be null when len > 0.
 * @param[in] len  number of bytes offered.
 * @return the number actually accepted (<= len).
 */
uint32_t HostCdcWrite(const uint8_t* buf, uint32_t len);

/** @brief Flush the CDC IN endpoint so queued bytes are sent. */
void HostCdcWriteFlush();

// ---- Memory / diagnostics ------------------------------------------------
/**
 * @brief Free internal-RAM heap, in KiB (AT+LINK?/AT+DIAG telemetry).
 * @return free internal heap in kilobytes.
 */
uint32_t FreeInternalHeapKb();

/**
 * @brief Provide the host->link ingest ring buffer, claimed once at boot.
 *
 * Prefers `want` bytes of large/external RAM (PSRAM on the ESP32); if that is
 * unavailable, returns a smaller internal-RAM buffer of at least `fallback`
 * bytes. The buffer is never freed (a fixed pre-allocated ring — CLAUDE.md
 * rule 5).
 *
 * @param[in]  want      preferred capacity in bytes (large/external RAM).
 * @param[in]  fallback  minimum capacity if `want` can't be satisfied.
 * @param[out] out_cap   the actual capacity provided (0 if allocation failed).
 * @return pointer to the buffer, or null if none could be provided.
 */
uint8_t* AllocIngest(size_t want, size_t fallback, size_t* out_cap);

}  // namespace platform

#endif  // LORA_SERIAL_PLATFORM_H_
