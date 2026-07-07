/**
 * @file prefs.h
 * @brief Platform-selected persistent key/value store, API-compatible with the
 *        Arduino ESP32 `Preferences` class.
 *
 * The firmware keeps its settings in a `Preferences` handle named `prefs`. On
 * the ESP32 that is the real IDF NVS-backed class. On the nRF52 (Wio Tracker
 * L1) there is no `Preferences`, so this provides a same-signature class backed
 * by the Adafruit nRF52 core's internal LittleFS (InternalFS): each key is a
 * small file under a namespace directory, so settings survive a reboot exactly
 * like the ESP32's NVS. The bodies live in prefs_nrf52.cpp (headers declare,
 * .cpp defines); because the API matches, no call site changes between MCUs.
 *
 * Include this instead of <Preferences.h> so the same src/ builds on both MCUs.
 */
#ifndef LORA_SERIAL_PLATFORM_PREFS_H_
#define LORA_SERIAL_PLATFORM_PREFS_H_

#if defined(ARDUINO_ARCH_ESP32)

#include <Preferences.h>   // the real NVS-backed class on the ESP32

#else  // ---- nRF52 (and any non-ESP32 target): defaults-only stub ----------

#include <Arduino.h>       // Arduino String, size types
#include <string.h>

/**
 * @brief LittleFS-backed, Arduino-`Preferences`-compatible key/value store.
 *
 * Each key is stored as its own small file under a namespace directory in the
 * nRF52's internal flash (InternalFS), so settings persist across reboots just
 * like the ESP32's NVS. One file per key keeps writes independent — a value
 * updated from one context never has to read-modify-write another's. Defined in
 * prefs_nrf52.cpp.
 */
class Preferences {
 public:
  /**
   * @brief Open (and mount) a namespace directory.
   * @param[in] name       namespace name; becomes the flash directory "/name".
   * @param[in] read_only  if true, writes are rejected.
   * @return true once InternalFS is mounted and the directory exists.
   */
  bool begin(const char* name, bool read_only = false);

  /** @brief Close the namespace (flushes are immediate, so a no-op here). */
  void end();

  /**
   * @brief Erase every key in the namespace directory.
   * @return true on success.
   */
  bool clear();

  /**
   * @brief Whether a key exists.
   * @param[in] key  key name.
   * @return true if the backing file is present.
   */
  bool isKey(const char* key);

  /**
   * @brief Read an 8-bit value.
   * @param[in] key  key name.
   * @param[in] def  value to return when the key is absent.
   * @return the stored value, or def.
   */
  uint8_t getUChar(const char* key, uint8_t def = 0);
  /**
   * @brief Write an 8-bit value.
   * @param[in] key  key name. @param[in] val  value.
   * @return bytes written (1), or 0 on failure.
   */
  size_t putUChar(const char* key, uint8_t val);

  /**
   * @brief Read a 16-bit value.
   * @param[in] key  key name.
   * @param[in] def  value to return when the key is absent.
   * @return the stored value, or def.
   */
  uint16_t getUShort(const char* key, uint16_t def = 0);
  /**
   * @brief Write a 16-bit value.
   * @param[in] key  key name. @param[in] val  value.
   * @return bytes written (2), or 0 on failure.
   */
  size_t putUShort(const char* key, uint16_t val);

  /**
   * @brief Read a 32-bit value.
   * @param[in] key  key name.
   * @param[in] def  value to return when the key is absent.
   * @return the stored value, or def.
   */
  uint32_t getUInt(const char* key, uint32_t def = 0);
  /**
   * @brief Write a 32-bit value.
   * @param[in] key  key name. @param[in] val  value.
   * @return bytes written (4), or 0 on failure.
   */
  size_t putUInt(const char* key, uint32_t val);

  /**
   * @brief Read a 64-bit value.
   * @param[in] key  key name.
   * @param[in] def  value to return when the key is absent.
   * @return the stored value, or def.
   */
  uint64_t getULong64(const char* key, uint64_t def = 0);
  /**
   * @brief Write a 64-bit value.
   * @param[in] key  key name. @param[in] val  value.
   * @return bytes written (8), or 0 on failure.
   */
  size_t putULong64(const char* key, uint64_t val);

  /**
   * @brief Read a float value.
   * @param[in] key  key name.
   * @param[in] def  value to return when the key is absent.
   * @return the stored value, or def.
   */
  float getFloat(const char* key, float def = 0.0f);
  /**
   * @brief Write a float value.
   * @param[in] key  key name. @param[in] val  value.
   * @return bytes written (sizeof(float)), or 0 on failure.
   */
  size_t putFloat(const char* key, float val);

  /**
   * @brief Read a string value.
   * @param[in] key  key name.
   * @param[in] def  value to return when the key is absent.
   * @return the stored string, or def.
   */
  String getString(const char* key, const String& def = String());
  /**
   * @brief Write a string value (stored without a trailing NUL).
   * @param[in] key  key name. @param[in] val  NUL-terminated source.
   * @return bytes written (the string length), or 0 on failure.
   */
  size_t putString(const char* key, const char* val);

  /**
   * @brief Read a blob into a caller buffer.
   * @param[in]  key  key name.
   * @param[out] buf  destination buffer.
   * @param[in]  len  buffer capacity.
   * @return bytes copied (0 if the key is absent).
   */
  size_t getBytes(const char* key, void* buf, size_t len);
  /**
   * @brief Write a blob, replacing any prior value.
   * @param[in] key  key name. @param[in] buf  source. @param[in] len  length.
   * @return bytes written, or 0 on failure.
   */
  size_t putBytes(const char* key, const void* buf, size_t len);
  /**
   * @brief Length of a stored blob.
   * @param[in] key  key name.
   * @return the file size in bytes, or 0 if absent.
   */
  size_t getBytesLength(const char* key);

 private:
  char ns_[24] = {0};   ///< namespace directory path, e.g. "/loramodem"
  bool ro_ = false;     ///< opened read-only (writes rejected)
};

#endif  // ARDUINO_ARCH_ESP32

#endif  // LORA_SERIAL_PLATFORM_PREFS_H_
