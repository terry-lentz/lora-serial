/**
 * @file fw_display.cpp
 * @brief Display class implementation (see fw_display.h). Built only on boards
 *        that declare HAS_DISPLAY (excluded from the ESP32 envs by the
 *        build_src_filter in platformio.ini).
 *
 * Layout on the 128x64 SH1106: a full-width white status bar (inverted, black
 * text) shows the carrier frequency, the mode, a 5-bar RSSI meter and a
 * heartbeat that pulses on each link RX; below it, a 4-line teletype shows the
 * received serial stream in real time (wrap, scroll, control bytes shown as
 * '.'). A white border frames the whole screen.
 */
#include "fw_display.h"

#include <string.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Arduino.h>
#include <Wire.h>

#include "fw_config.h"   // cfg (freq), g_host_out_hook
#include "fw_device.h"   // g_device.last_rx_ms — drives the heartbeat
#include "fw_radio.h"    // g_radio: RSSI + current mode name

// The single display-front-end instance (static singleton; no heap — rule 5).
Display g_display;

// ---- OLED driver (file-static so the header needs no display includes) ------
static const uint8_t kOledW     = 128;
static const uint8_t kOledH     = 64;
static const uint8_t kOledAddrA = 0x3C;   ///< common SH1106 address
static const uint8_t kOledAddrB = 0x3D;   ///< the L1's panel answers here
static const int8_t  kOledRst   = -1;     ///< no dedicated reset line
static Adafruit_SH1106G g_oled(kOledW, kOledH, &Wire, kOledRst);

// ---- Brightness steps (SH1106 contrast). MED/LOW are deliberately dark. -----
static const uint8_t kBright[]  = {0xFF, 0x20, 0x01};   ///< FULL, MED, LOW
static const uint8_t kNumBright = sizeof(kBright) / sizeof(kBright[0]);

// User button (cycles brightness). D13 on the L1 — from the board variant.
static const int      kBtnPin      = CANCEL_BUTTON_PIN;
static const uint32_t kBtnDebounce = 30;   ///< ms stable to accept a press

// Redraw pacing. A full SH1106 frame is ~1 KB over I2C and BLOCKS the loop as
// it ships; on a responder that loop also services the half-duplex turn, so an
// over-eager redraw starves the link. We run I2C fast (400 kHz, ~25 ms/frame)
// and rate-limit: redraw at most every kDrawMinMs when content changed, with a
// slower background refresh so the status bar / heartbeat still stay live.
static const uint32_t kDrawMinMs     = 500;
static const uint32_t kRefreshMs     = 1500;
static const uint32_t kBeatMs        = 160;   ///< heart shown filled this long

// 7x7 filled-heart bitmap (MSB-first rows) for the heartbeat.
static const uint8_t kHeart[] = {0x6C, 0xFE, 0xFE, 0xFE, 0x7C, 0x38, 0x10};

/**
 * @brief Probe an I2C address for an ACK.
 * @param[in] addr  7-bit address. @return true if a device answered.
 */
static bool I2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

/**
 * @brief Short display name for a mode (only 'ludicrous' needs shortening).
 * @param[in] name  full mode name. @return a <=6-char label.
 */
static const char* ModeShort(const char* name) {
  return strcmp(name, "ludicrous") == 0 ? "lud" : name;
}

/**
 * @brief Map a smoothed RSSI (dBm) to a 0..5 signal-bar count.
 * @param[in] rssi  smoothed RSSI in dBm. @return bars to fill (0..5).
 */
static int RssiBars(float rssi) {
  if (rssi >= -55) return 5;
  if (rssi >= -65) return 4;
  if (rssi >= -75) return 3;
  if (rssi >= -85) return 2;
  if (rssi >= -95) return 1;
  return 0;
}

void Display::FeedThunk(const uint8_t* data, size_t len) {
  g_display.Feed(data, len);
}

void Display::NewLine() {
  for (uint8_t r = 0; r < kRows - 1; r++)
    memcpy(rows_[r], rows_[r + 1], kCols + 1);
  rows_[kRows - 1][0] = 0;
  col_ = 0;
}

void Display::Feed(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    uint8_t c = data[i];
    if (c == '\n') {
      NewLine();
    } else if (c == '\r') {
      // carriage return: ignored (newlines drive the teletype)
    } else {
      if (c < 0x20 || c > 0x7E) c = '.';   // show control/binary as a dot
      if (col_ >= kCols) NewLine();
      rows_[kRows - 1][col_++] = (char)c;
      rows_[kRows - 1][col_] = 0;
    }
  }
  dirty_ = true;
}

void Display::PollButton() {
  bool raw = digitalRead(kBtnPin);
  if (raw != btn_last_) {
    btn_last_ = raw;
    btn_changed_ms_ = millis();
  }
  if (millis() - btn_changed_ms_ > kBtnDebounce && raw != btn_stable_) {
    btn_stable_ = raw;
    if (btn_stable_ == LOW) {   // active-low: pressed
      bright_ = (bright_ + 1) % kNumBright;
      g_oled.setContrast(kBright[bright_]);
    }
  }
}

void Display::Draw() {
  g_oled.clearDisplay();
  g_oled.drawRect(0, 0, kOledW, kOledH, SH110X_WHITE);   // screen border

  // --- inverted status bar (white fill, black content) ---
  g_oled.fillRect(0, 0, kOledW, 11, SH110X_WHITE);
  g_oled.setTextSize(1);
  g_oled.setTextColor(SH110X_BLACK);

  char freq[8];
  snprintf(freq, sizeof freq, "%.1f", (double)cfg.freq_mhz);
  g_oled.setCursor(2, 2);
  g_oled.print(freq);

  g_oled.setCursor(40, 2);
  g_oled.print(ModeShort(g_radio.CurrentModeName()));

  int bars = g_radio.have_rssi() ? RssiBars(g_radio.rssi()) : 0;
  for (int i = 0; i < 5; i++) {
    int h = 3 + i;                 // ascending: 3,4,5,6,7 px
    int x = 87 + i * 4, y = 9 - h;
    if (i < bars)
      g_oled.fillRect(x, y, 3, h, SH110X_BLACK);
    else
      g_oled.drawRect(x, y, 3, h, SH110X_BLACK);
  }

  if ((int32_t)(beat_until_ms_ - millis()) > 0)
    g_oled.drawBitmap(118, 2, kHeart, 7, 7, SH110X_BLACK);

  // --- teletype (white on black, below the bar) ---
  g_oled.setTextColor(SH110X_WHITE);
  for (uint8_t r = 0; r < kRows; r++) {
    g_oled.setCursor(1, 14 + r * 12);
    g_oled.print(rows_[r]);
  }
  g_oled.display();
}

void Display::Init() {
  Wire.begin();   // MUST precede any I2C probe, or the bus transaction hangs
  Wire.setClock(400000);   // fast I2C so a full-frame push won't stall the link
  uint8_t addr = I2cPresent(kOledAddrA)   ? kOledAddrA
                 : I2cPresent(kOledAddrB) ? kOledAddrB
                                          : 0;
  if (addr && g_oled.begin(addr, true)) {
    ok_ = true;
    g_oled.setTextSize(1);
    g_oled.setTextWrap(false);           // the teletype does its own wrapping
    g_oled.setContrast(kBright[bright_]);
  }
  pinMode(kBtnPin, INPUT_PULLUP);
  g_host_out_hook = &Display::FeedThunk;   // received bytes -> teletype
  if (ok_) Draw();
}

void Display::Tick() {
  if (!ok_) return;
  PollButton();
  uint32_t now = millis();

  // Heartbeat: the link's last-RX timestamp advances on every valid frame from
  // the peer (data or keepalive), so pulse the heart on each change — no extra
  // airtime, and naturally slow on 'far', fast on 'turbo'.
  uint32_t rx = g_device.last_rx_ms();
  if (rx != prev_rx_ms_) {
    prev_rx_ms_ = rx;
    beat_until_ms_ = now + kBeatMs;
    dirty_ = true;
  }

  bool due = (now - last_draw_ms_ >= kDrawMinMs) && dirty_;
  bool refresh = (now - last_draw_ms_ >= kRefreshMs);
  if (due || refresh) {
    last_draw_ms_ = now;
    dirty_ = false;
    Draw();
  }
}
