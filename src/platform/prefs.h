/**
 * @file prefs.h
 * @brief Platform-selected persistent key/value store, API-compatible with the
 *        Arduino ESP32 `Preferences` class.
 *
 * The firmware keeps its settings in a `Preferences` handle named `prefs`. On
 * the ESP32 that is the real IDF NVS-backed class. On the nRF52 (Wio Tracker
 * L1) there is no `Preferences`, so this provides a same-signature class. The
 * nRF52 version is currently a NO-OP stub: every read returns the caller's
 * default and every write is discarded, so the node runs on its compile-time
 * defaults (build flags). A flash/InternalFS-backed implementation replaces the
 * stub later; because the API matches, no call site changes when it does.
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
 * @brief Defaults-only, Arduino-`Preferences`-compatible key/value store.
 *
 * Reads return the supplied default; writes are discarded. Lets the node build
 * and run on its compile-time defaults on a target without NVS. Not persistent
 * (yet) — a flash-backed implementation will replace the bodies.
 */
class Preferences {
 public:
  /**
   * @brief Open a namespace (no-op).
   * @param[in] name       namespace name (ignored).
   * @param[in] read_only  open read-only (ignored).
   * @return always true (as if the namespace opened).
   */
  bool begin(const char* name, bool read_only = false) {
    (void)name;
    (void)read_only;
    return true;
  }

  /** @brief Close the namespace (no-op). */
  void end() {}

  /**
   * @brief Erase all keys (no-op).
   * @return always true.
   */
  bool clear() { return true; }

  /**
   * @brief Whether a key exists.
   * @param[in] key  key name (ignored).
   * @return always false (nothing is stored).
   */
  bool isKey(const char* key) {
    (void)key;
    return false;
  }

  /**
   * @brief Read an 8-bit value.
   * @param[in] key  key name (ignored).
   * @param[in] def  value to return.
   * @return def.
   */
  uint8_t getUChar(const char* key, uint8_t def = 0) {
    (void)key;
    return def;
  }
  /**
   * @brief Write an 8-bit value (discarded).
   * @param[in] key  key name (ignored).
   * @param[in] val  value (ignored).
   * @return 1 (bytes "written").
   */
  size_t putUChar(const char* key, uint8_t val) {
    (void)key;
    (void)val;
    return 1;
  }

  /**
   * @brief Read a 16-bit value.
   * @param[in] key  key name (ignored).
   * @param[in] def  value to return.
   * @return def.
   */
  uint16_t getUShort(const char* key, uint16_t def = 0) {
    (void)key;
    return def;
  }
  /**
   * @brief Write a 16-bit value (discarded).
   * @param[in] key  key name (ignored).
   * @param[in] val  value (ignored).
   * @return 2.
   */
  size_t putUShort(const char* key, uint16_t val) {
    (void)key;
    (void)val;
    return 2;
  }

  /**
   * @brief Read a 32-bit value.
   * @param[in] key  key name (ignored).
   * @param[in] def  value to return.
   * @return def.
   */
  uint32_t getUInt(const char* key, uint32_t def = 0) {
    (void)key;
    return def;
  }
  /**
   * @brief Write a 32-bit value (discarded).
   * @param[in] key  key name (ignored).
   * @param[in] val  value (ignored).
   * @return 4.
   */
  size_t putUInt(const char* key, uint32_t val) {
    (void)key;
    (void)val;
    return 4;
  }

  /**
   * @brief Read a 64-bit value.
   * @param[in] key  key name (ignored).
   * @param[in] def  value to return.
   * @return def.
   */
  uint64_t getULong64(const char* key, uint64_t def = 0) {
    (void)key;
    return def;
  }
  /**
   * @brief Write a 64-bit value (discarded).
   * @param[in] key  key name (ignored).
   * @param[in] val  value (ignored).
   * @return 8.
   */
  size_t putULong64(const char* key, uint64_t val) {
    (void)key;
    (void)val;
    return 8;
  }

  /**
   * @brief Read a float value.
   * @param[in] key  key name (ignored).
   * @param[in] def  value to return.
   * @return def.
   */
  float getFloat(const char* key, float def = 0.0f) {
    (void)key;
    return def;
  }
  /**
   * @brief Write a float value (discarded).
   * @param[in] key  key name (ignored).
   * @param[in] val  value (ignored).
   * @return sizeof(float).
   */
  size_t putFloat(const char* key, float val) {
    (void)key;
    (void)val;
    return sizeof(float);
  }

  /**
   * @brief Read a string value.
   * @param[in] key  key name (ignored).
   * @param[in] def  value to return.
   * @return def.
   */
  String getString(const char* key, const String& def = String()) {
    (void)key;
    return def;
  }
  /**
   * @brief Write a string value (discarded).
   * @param[in] key  key name (ignored).
   * @param[in] val  value (ignored).
   * @return the string length.
   */
  size_t putString(const char* key, const char* val) {
    (void)key;
    return val ? strlen(val) : 0;
  }

  /**
   * @brief Read a blob into a caller buffer (none stored).
   * @param[in]  key  key name (ignored).
   * @param[out] buf  destination (ignored).
   * @param[in]  len  capacity (ignored).
   * @return 0 (nothing copied).
   */
  size_t getBytes(const char* key, void* buf, size_t len) {
    (void)key;
    (void)buf;
    (void)len;
    return 0;
  }
  /**
   * @brief Write a blob (discarded).
   * @param[in] key  key name (ignored).
   * @param[in] buf  source (ignored).
   * @param[in] len  length.
   * @return len.
   */
  size_t putBytes(const char* key, const void* buf, size_t len) {
    (void)key;
    (void)buf;
    return len;
  }
  /**
   * @brief Length of a stored blob (none stored).
   * @param[in] key  key name (ignored).
   * @return 0.
   */
  size_t getBytesLength(const char* key) {
    (void)key;
    return 0;
  }
};

#endif  // ARDUINO_ARCH_ESP32

#endif  // LORA_SERIAL_PLATFORM_PREFS_H_
