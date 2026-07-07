/**
 * @file prefs_nrf52.cpp
 * @brief nRF52 flash-backed implementation of the Preferences store declared in
 *        prefs.h. Built only on the nRF52 (excluded from the ESP32 envs by the
 *        build_src_filter in platformio.ini); the ESP32 uses the real NVS class.
 *
 * Storage model: each key is a separate file under the namespace directory in
 * the Adafruit nRF52 core's internal LittleFS (InternalFS). One file per key
 * makes every write self-contained — no read-modify-write of a shared blob — so
 * a value written from the link context (e.g. the TX counter) never races a
 * value written from the display/menu context. Values are stored as raw
 * little-endian bytes; a typed get reads the file back into the same-width type.
 */
#include "prefs.h"

#ifndef ARDUINO_ARCH_ESP32

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

using namespace Adafruit_LittleFS_Namespace;

namespace {

// Longest path we build: "/<namespace>/<key>" — both are short, 64 is ample.
const size_t kPathMax = 64;

/**
 * @brief Build the flash path "/<namespace>/<key>" for a key.
 * @param[in]  ns   namespace directory path (e.g. "/loramodem").
 * @param[in]  key  key name.
 * @param[out] out  destination buffer.
 * @param[in]  n    destination capacity.
 */
void KeyPath(const char* ns, const char* key, char* out, size_t n) {
  snprintf(out, n, "%s/%s", ns, key);
}

}  // namespace

bool Preferences::begin(const char* name, bool read_only) {
  ro_ = read_only;
  snprintf(ns_, sizeof ns_, "/%s", name ? name : "prefs");
  InternalFS.begin();      // idempotent: mounts (and formats first time) once
  InternalFS.mkdir(ns_);   // ensure the namespace directory exists (idempotent)
  return true;
}

void Preferences::end() {}   // writes are flushed on close(); nothing to do here

bool Preferences::clear() {
  if (ro_) return false;
  File dir = InternalFS.open(ns_, FILE_O_READ);
  if (!dir) return true;   // no directory yet => nothing to clear
  File e = dir.openNextFile();
  while (e) {
    // name() is the entry's basename; rebuild the full path to remove it. (Guard
    // against a core that returns an absolute path by checking the leading '/'.)
    const char* nm = e.name();
    char path[kPathMax];
    if (nm[0] == '/')
      snprintf(path, sizeof path, "%s", nm);
    else
      snprintf(path, sizeof path, "%s/%s", ns_, nm);
    e.close();
    InternalFS.remove(path);
    e = dir.openNextFile();
  }
  dir.close();
  return true;
}

bool Preferences::isKey(const char* key) {
  char path[kPathMax];
  KeyPath(ns_, key, path, sizeof path);
  return InternalFS.exists(path);
}

size_t Preferences::putBytes(const char* key, const void* buf, size_t len) {
  if (ro_) return 0;
  char path[kPathMax];
  KeyPath(ns_, key, path, sizeof path);
  InternalFS.remove(path);   // replace any prior value with a fresh file
  File f = InternalFS.open(path, FILE_O_WRITE);
  if (!f) return 0;
  size_t w = f.write((const uint8_t*)buf, len);
  f.close();
  return w;
}

size_t Preferences::getBytes(const char* key, void* buf, size_t len) {
  char path[kPathMax];
  KeyPath(ns_, key, path, sizeof path);
  File f = InternalFS.open(path, FILE_O_READ);
  if (!f) return 0;
  int r = f.read(buf, (uint16_t)len);
  f.close();
  return r < 0 ? 0 : (size_t)r;
}

size_t Preferences::getBytesLength(const char* key) {
  char path[kPathMax];
  KeyPath(ns_, key, path, sizeof path);
  File f = InternalFS.open(path, FILE_O_READ);
  if (!f) return 0;
  size_t s = f.size();
  f.close();
  return s;
}

// ---- Fixed-width typed accessors: store/read the raw bytes of the value ------

uint8_t Preferences::getUChar(const char* key, uint8_t def) {
  uint8_t v;
  return getBytes(key, &v, sizeof v) == sizeof v ? v : def;
}
size_t Preferences::putUChar(const char* key, uint8_t val) {
  return putBytes(key, &val, sizeof val);
}

uint16_t Preferences::getUShort(const char* key, uint16_t def) {
  uint16_t v;
  return getBytes(key, &v, sizeof v) == sizeof v ? v : def;
}
size_t Preferences::putUShort(const char* key, uint16_t val) {
  return putBytes(key, &val, sizeof val);
}

uint32_t Preferences::getUInt(const char* key, uint32_t def) {
  uint32_t v;
  return getBytes(key, &v, sizeof v) == sizeof v ? v : def;
}
size_t Preferences::putUInt(const char* key, uint32_t val) {
  return putBytes(key, &val, sizeof val);
}

uint64_t Preferences::getULong64(const char* key, uint64_t def) {
  uint64_t v;
  return getBytes(key, &v, sizeof v) == sizeof v ? v : def;
}
size_t Preferences::putULong64(const char* key, uint64_t val) {
  return putBytes(key, &val, sizeof val);
}

float Preferences::getFloat(const char* key, float def) {
  float v;
  return getBytes(key, &v, sizeof v) == sizeof v ? v : def;
}
size_t Preferences::putFloat(const char* key, float val) {
  return putBytes(key, &val, sizeof val);
}

String Preferences::getString(const char* key, const String& def) {
  char path[kPathMax];
  KeyPath(ns_, key, path, sizeof path);
  File f = InternalFS.open(path, FILE_O_READ);
  if (!f) return def;
  char buf[64];
  int n = f.read(buf, sizeof buf - 1);
  f.close();
  if (n < 0) n = 0;
  buf[n] = 0;
  return String(buf);
}
size_t Preferences::putString(const char* key, const char* val) {
  return putBytes(key, val, val ? strlen(val) : 0);
}

#endif  // !ARDUINO_ARCH_ESP32
