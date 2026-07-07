/**
 * @file fw_display.cpp
 * @brief Display class implementation (see fw_display.h). Built only on boards
 *        that declare HAS_DISPLAY (excluded from the ESP32 envs by the
 *        build_src_filter in platformio.ini).
 *
 * The OLED runs on a dedicated low-priority FreeRTOS task so its blocking
 * I2C never stalls the half-duplex turn loop (which runs at TASK_PRIO_LOW). The
 * link loop only ever enqueues received bytes into a lock-free SPSC ring
 * (FeedThunk, the g_host_out_hook tap); the task drains the ring into the
 * teletype, reads live link stats for the status bar, and pushes frames.
 *
 * Layout on the 128x64 SH1106: a full-width white status bar (inverted, black
 * content) shows the carrier frequency, the mode, a 5-bar RSSI meter, TX/RX
 * activity arrows and a heartbeat that pulses on each link RX; below it, a
 * 5-line teletype renders the received serial stream (wrap, scroll, control
 * bytes as '.') over a 64-line history the trackball scrolls back through. A
 * white border frames the whole screen.
 */
#include "fw_display.h"

#include <stdio.h>
#include <string.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Arduino.h>
#include <Wire.h>

#include "fw_config.h"       // cfg, FEAT_*, g_host_out_hook, g_link (stats)
#include "fw_device.h"       // g_device.last_rx_ms — the heartbeat edge
#include "fw_host.h"         // g_host: host_out, ApplyLinkConfig, SaveSettings
#include "fw_radio.h"        // g_radio: RSSI/SNR, mode table, SetFrequency
#include "platform/prefs.h"  // Preferences — persist brightness/region

// The global settings store (defined in main.cpp); used to persist the
// brightness and region choices the menu owns.
extern Preferences prefs;

// The single display-front-end instance (static singleton; no heap — rule 5).
Display g_display;

// ---- OLED driver (file-static so the header needs no display includes) ------
static const uint8_t kOledW     = 128;
static const uint8_t kOledH     = 64;
// Height of the inverted top bar, shared by every screen so the connection
// cluster (which reaches down to row 8) keeps the same 2-px margin everywhere —
// otherwise a shorter bar clips it at the bottom.
static const uint8_t kBarH      = 11;
static const uint8_t kOledAddrA = 0x3C;   ///< common SH1106 address
static const uint8_t kOledAddrB = 0x3D;   ///< the L1's panel answers here
static const int8_t  kOledRst   = -1;     ///< no dedicated reset line
static Adafruit_SH1106G g_oled(kOledW, kOledH, &Wire, kOledRst);

// ---- Brightness steps (SH1106 contrast, command 0x81). Pushed low so even MED
// is dim: FULL is daylight, MED is a dim indoor level, LOW is the contrast
// floor for night use. LOW also shortens the pre-charge period (SetBrightness)
// to dim below the floor. VCOM is deliberately left alone — it's threshold-
// sensitive on this panel and a wrong value blanks the glass; pre-charge only.
static const uint8_t kBright[]  = {0xFF, 0x18, 0x00};   ///< FULL, MED, LOW
static const uint8_t kNumBright = sizeof(kBright) / sizeof(kBright[0]);

// Pre-charge period (command 0xD9). The SH1106G driver init programs 0x1F; the
// night step shortens phase-1 to its minimum (0x11) so the darkest level dims
// below what contrast 0x00 reaches. Both are valid values — unlike VCOM, this
// won't blank the panel.
static const uint8_t kCmdPrecharge   = 0xD9;
static const uint8_t kPrechargeNorm  = 0x1F;  ///< SH1106G driver default
static const uint8_t kPrechargeNight = 0x11;  ///< shortest phase-1 -> dimmest

// User button (cycles brightness). D13 on the L1 — from the board variant.
static const int      kBtnPin      = CANCEL_BUTTON_PIN;
static const uint32_t kBtnDebounce = 30;   ///< ms stable to accept a press

// Task cadence + how long the heart / arrows stay lit after an event.
static const uint32_t kFrameMs = 50;    ///< ~20 fps redraw (on the task)
static const uint32_t kBeatMs  = 160;   ///< heart shown filled this long
static const uint32_t kFlickMs = 120;   ///< an arrow shown filled this long
static const uint32_t kStaleMs = 4000;  ///< no RX this long -> "disconnected"
static const uint16_t kTaskStack = 2048;   ///< task stack depth (words)

// 7x7 filled-heart bitmap (MSB-first rows) for the heartbeat.
static const uint8_t kHeart[] = {0x6C, 0xFE, 0xFE, 0xFE, 0x7C, 0x38, 0x10};

// 7x7 padlock bitmap (MSB-first rows): a shackle over a body with a keyhole.
// Shown right of the frequency; crossed out when encryption is off.
static const uint8_t kLock[] = {0x38, 0x44, 0x44, 0xFE, 0xFE, 0xEE, 0xFE};

// ---- SPSC byte ring: link loop produces (FeedThunk), task consumes ----------
// Single-producer/single-consumer with a power-of-two-free modulo; volatile
// head/tail are read/written atomically on the 32-bit core, so no lock is
// needed. On overflow we drop for the DISPLAY only — the USB data path already
// has the bytes.
static const uint16_t   kRingSz = 512;
static volatile uint8_t s_ring[kRingSz];
static volatile uint16_t s_head = 0;   ///< consumer index (task)
static volatile uint16_t s_tail = 0;   ///< producer index (link loop)

// ---- Trackball edge counters (ISR-incremented, task-drained) ----------------
// The trackball's four direction pins pulse (falling edge) as the ball rolls;
// a GPIO interrupt per pin bumps a counter, and the task drains them. Plain
// 16-bit counters: single-writer (the ISR), single-reader (the task), and a
// dropped count now and then only means one fewer scroll/menu step — harmless.
static volatile uint16_t s_tb_up = 0;     ///< TB_UP falling edges
static volatile uint16_t s_tb_down = 0;   ///< TB_DOWN falling edges
static volatile uint16_t s_tb_left = 0;   ///< TB_LEFT falling edges
static volatile uint16_t s_tb_right = 0;  ///< TB_RIGHT falling edges
static volatile uint16_t s_tb_press = 0;  ///< TB_PRESS (click) falling edges

/** @brief TB_UP interrupt: count one "rolled up" step. */
static void OnTbUp() { s_tb_up++; }

/** @brief TB_DOWN interrupt: count one "rolled down" step. */
static void OnTbDown() { s_tb_down++; }

/** @brief TB_LEFT interrupt: count one "rolled left" step. */
static void OnTbLeft() { s_tb_left++; }

/** @brief TB_RIGHT interrupt: count one "rolled right" step. */
static void OnTbRight() { s_tb_right++; }

/** @brief TB_PRESS interrupt: count one click (drained + debounced by task). */
static void OnTbPress() { s_tb_press++; }

// Hold-to-auto-scroll (main screen): after kAutoDelayMs of a direction held
// down, scroll repeats each frame — one line, then kAutoFast lines once held a
// while, so a long buffer is quick to traverse without spamming the ball.
static const uint32_t kAutoDelayMs = 350;   ///< hold this long before repeating
static const uint32_t kAutoFastMs  = 1200;  ///< then speed up after this long
static const uint16_t kAutoFast    = 4;     ///< fast-repeat lines per frame
static const uint32_t kPressGuardMs = 180;  ///< ignore re-presses within this

// ---- Frequency regions: expand the tunable range and clamp the freq item. ---
// Selecting a region sets the min/max the frequency item steps within (and
// snaps an out-of-band frequency to the region default). All sit inside the
// SX1262's 150-960 MHz range. Both ends must share one frequency; the region is
// just a local convenience for choosing a legal one.
struct Region {
  const char* name;   ///< short label shown in the menu
  float min_mhz;      ///< low edge of the band
  float max_mhz;      ///< high edge of the band
  float def_mhz;      ///< sensible default within the band
};
static const Region kRegions[] = {
  {"TW", 920.0f, 925.0f, 921.0f},   ///< Taiwan 920-925 MHz
  {"US", 902.0f, 928.0f, 915.0f},   ///< US915 ISM 902-928 MHz
  {"EU", 863.0f, 870.0f, 868.0f},   ///< EU868 SRD 863-870 MHz
};
static const uint8_t kNumRegions = sizeof(kRegions) / sizeof(kRegions[0]);
static const float   kFreqStepMhz = 0.1f;   ///< frequency edit step per click

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
  for (size_t i = 0; i < len; i++) {
    uint16_t nt = (uint16_t)((s_tail + 1) % kRingSz);
    if (nt == s_head) break;   // ring full: drop (USB path still has the bytes)
    s_ring[s_tail] = data[i];
    s_tail = nt;
  }
}

void Display::SetBrightness(uint8_t level) {
  g_oled.setContrast(kBright[level]);
  // Shorten the pre-charge period on the darkest (night) step for extra dimming
  // below the contrast floor; restore the driver default otherwise. Raw command
  // stream: control byte 0x00 => the following bytes are commands + their args.
  bool night = (level == kNumBright - 1);
  Wire.beginTransmission(oled_addr_);
  Wire.write(0x00);
  Wire.write(kCmdPrecharge);
  Wire.write(night ? kPrechargeNight : kPrechargeNorm);
  Wire.endTransmission();
}

void Display::NewLine() {
  head_ = (uint16_t)((head_ + 1) % kScroll);   // advance the ring one slot
  lines_[head_][0] = 0;                         // fresh, empty current line
  if (count_ < kScroll) count_++;               // history grows until it fills
  // If the reader is scrolled back, follow the tail so their view stays put as
  // new lines push in — until history is full, then the oldest scrolls off.
  if (scroll_ > 0) {
    uint16_t smax = (count_ > kRows) ? (uint16_t)(count_ - kRows) : 0;
    if (scroll_ < smax) scroll_++;
    else scroll_ = smax;
  }
  col_ = 0;
}

void Display::Ingest(uint8_t c) {
  if (c == '\r') return;              // ignore CR; newlines drive the feed
  // Defer the scroll on '\n': keep the just-finished line on the bottom row and
  // only scroll when the NEXT character arrives, so the newest completed line
  // is always visible instead of an empty cursor row.
  if (c == '\n') {
    pending_nl_ = true;
    return;
  }
  if (c == 0) return;                 // NUL would terminate the line's C string
  // Everything else is a printable CP437 glyph under cp437(true), including the
  // low-range symbols like 0x1E/0x1F (up/down triangles) used as markers; only
  // CR and LF (handled above) are treated as control.
  if (pending_nl_) {                  // first char after '\n' -> scroll now
    NewLine();
    pending_nl_ = false;
  }
  if (col_ >= kCols) NewLine();
  lines_[head_][col_++] = (char)c;
  lines_[head_][col_] = 0;
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
      // "Back/escape": cancel an in-progress edit, else advance the screen.
      if (screen_ == kScreenConfig && editing_) {
        CancelEdit();
      } else {
        screen_ = (Screen)((screen_ + 1) % 3);
        editing_ = false;
      }
    }
  }
}

void Display::PollTrackball() {
  // Drain the ISR edge counters (clear-after-read; a pulse racing the reset
  // lands on the next poll), then act on them per screen.
  uint16_t up = s_tb_up;    s_tb_up = 0;
  uint16_t dn = s_tb_down;  s_tb_down = 0;
  uint16_t lf = s_tb_left;  s_tb_left = 0;
  uint16_t rt = s_tb_right; s_tb_right = 0;

  if (screen_ == kScreenMain) {
    // Scrollback: up walks into older history, down returns toward live. Edge
    // pulses give fine control; holding a direction auto-repeats (accelerating)
    // so a long buffer is quick to traverse. Positive = older.
    uint32_t now = millis();
    int older = (int)up - (int)dn;
    older += HoldRepeat(TB_UP, &up_since_, now);
    older -= HoldRepeat(TB_DOWN, &dn_since_, now);
    uint16_t smax = (count_ > kRows) ? (uint16_t)(count_ - kRows) : 0;
    int s = (int)scroll_ + older;
    if (s < 0) s = 0;
    if (s > (int)smax) s = smax;
    scroll_ = (uint16_t)s;
    return;
  }
  if (screen_ != kScreenConfig) return;

  // On CONFIG we step one notch per poll (direction only), so discrete choices
  // don't race past under a burst of pulses.
  if (editing_) {
    if (rt > lf) EditStep(+1);
    else if (lf > rt) EditStep(-1);
  } else {
    int dir = (dn > up) ? +1 : (up > dn) ? -1 : 0;
    if (dir)
      sel_ = (uint8_t)(((int)sel_ + dir + kItemCount) % kItemCount);
  }
}

int Display::HoldRepeat(int pin, uint32_t* since, uint32_t now) {
  if (digitalRead(pin) != LOW) { *since = 0; return 0; }   // released
  if (*since == 0) { *since = now; return 0; }             // just pressed
  uint32_t held = now - *since;
  if (held < kAutoDelayMs) return 0;                       // pre-repeat delay
  return (held > kAutoFastMs) ? kAutoFast : 1;             // repeat rate
}

void Display::PollPress() {
  // The click is captured by an ISR (a quick press is too short for a 50 ms
  // poll to catch reliably); here we just drain it and reject bounce/repeat.
  if (s_tb_press == 0) return;
  s_tb_press = 0;
  uint32_t now = millis();
  if (now - prs_last_ms_ < kPressGuardMs) return;
  prs_last_ms_ = now;
  if (screen_ == kScreenConfig) {
    if (editing_) ConfirmEdit();
    else EnterEdit();
  }
}

void Display::EnterEdit() {
  switch (sel_) {
    case kItemBright: draft_i_ = bright_; break;
    case kItemRegion: draft_i_ = region_; break;
    case kItemFreq:   draft_f_ = cfg.freq_mhz; break;
    case kItemMode: {
      int m = g_radio.CurrentModeIndex();
      draft_i_ = (m >= 0) ? m : 0;
      break;
    }
    case kItemEnc:  draft_i_ = (cfg.feat & FEAT_ENC) ? 1 : 0; break;
    case kItemComp: draft_i_ = (cfg.feat & FEAT_COMP) ? 1 : 0; break;
  }
  editing_ = true;
}

void Display::EditStep(int dir) {
  switch (sel_) {
    case kItemBright: {
      int v = draft_i_ + dir;
      if (v < 0) v = 0;
      if (v >= kNumBright) v = kNumBright - 1;
      draft_i_ = v;
      SetBrightness((uint8_t)draft_i_);   // preview the level live
      break;
    }
    case kItemRegion:
      draft_i_ = (draft_i_ + dir + kNumRegions) % kNumRegions;
      break;
    case kItemFreq: {
      float f = draft_f_ + dir * kFreqStepMhz;
      float lo = kRegions[region_].min_mhz, hi = kRegions[region_].max_mhz;
      if (f < lo) f = lo;
      if (f > hi) f = hi;
      draft_f_ = f;
      break;
    }
    case kItemMode: {
      int n = g_radio.RfModeCount();
      draft_i_ = (draft_i_ + dir + n) % n;
      break;
    }
    case kItemEnc:
    case kItemComp:
      draft_i_ = !draft_i_;   // a step either way just toggles the boolean
      break;
  }
}

void Display::CancelEdit() {
  // The only item that changed anything before confirm is brightness (live
  // preview); revert it to the saved level. The rest were draft-only.
  if (sel_ == kItemBright) SetBrightness(bright_);
  editing_ = false;
}

void Display::ConfirmEdit() {
  // Update display-owned state now, but DEFER anything that touches the radio,
  // link, or flash to the main loop (ServiceConfig). Those subsystems (g_radio,
  // g_link, prefs/flash) are driven by the main loop and the radio-RX task and
  // are NOT safe to poke from this display task — doing so raced them and
  // deadlocked the node. Brightness is display-only (I2C we own), so it applies
  // live here; only its persistence defers.
  apply_item_ = sel_;
  switch (sel_) {
    case kItemBright:
      bright_ = (uint8_t)draft_i_;
      SetBrightness(bright_);          // live preview commit (our own I2C)
      apply_ival_ = bright_;
      break;
    case kItemRegion:
      region_ = (uint8_t)draft_i_;     // display state; freq re-clamp defers
      apply_ival_ = region_;
      break;
    case kItemFreq:
      apply_fval_ = draft_f_;
      break;
    case kItemMode:
    case kItemEnc:
    case kItemComp:
      apply_ival_ = draft_i_;
      break;
  }
  apply_pending_ = true;
  editing_ = false;
}

void Display::ServiceConfig() {
  if (!apply_pending_) return;
  apply_pending_ = false;
  // Runs in the MAIN loop — the same single-threaded context the AT commands
  // use — so these radio/link/flash calls can't race the RX task.
  switch (apply_item_) {
    case kItemBright:
      prefs.putUChar("bri", (uint8_t)apply_ival_);
      break;
    case kItemRegion: {
      prefs.putUChar("rgn", (uint8_t)apply_ival_);
      // Snap the carrier into the new band if it now falls outside it.
      float lo = kRegions[apply_ival_].min_mhz;
      float hi = kRegions[apply_ival_].max_mhz;
      if (cfg.freq_mhz < lo || cfg.freq_mhz > hi) {
        cfg.freq_mhz = kRegions[apply_ival_].def_mhz;
        g_radio.SetFrequency(cfg.freq_mhz);
      }
      g_host.SaveSettings();
      break;
    }
    case kItemFreq:
      cfg.freq_mhz = apply_fval_;
      g_radio.SetFrequency(cfg.freq_mhz);
      g_host.SaveSettings();
      break;
    case kItemMode:
      g_radio.ApplyModeByIndex(apply_ival_);
      g_host.SaveSettings();
      break;
    case kItemEnc:
      if (apply_ival_) cfg.feat |= FEAT_ENC;
      else             cfg.feat &= ~FEAT_ENC;
      g_host.ApplyLinkConfig();     // reinit the link with the new feature set
      g_host.SaveSettings();
      break;
    case kItemComp:
      if (apply_ival_) cfg.feat |= FEAT_COMP;
      else             cfg.feat &= ~FEAT_COMP;
      g_host.ApplyLinkConfig();
      g_host.SaveSettings();
      break;
  }
}

const char* Display::ItemLabel(uint8_t i) {
  switch (i) {
    case kItemBright: return "Bright";
    case kItemRegion: return "Region";
    case kItemFreq:   return "Freq";
    case kItemMode:   return "Mode";
    case kItemEnc:    return "Encrypt";
    case kItemComp:   return "Compress";
  }
  return "";
}

void Display::ItemValue(uint8_t i, char* out, size_t n) {
  static const char* kBrightName[] = {"FULL", "MED", "LOW"};
  bool ed = editing_ && (i == sel_);   // show the draft while editing this row
  switch (i) {
    case kItemBright:
      snprintf(out, n, "%s", kBrightName[ed ? draft_i_ : bright_]);
      break;
    case kItemRegion:
      snprintf(out, n, "%s", kRegions[ed ? draft_i_ : region_].name);
      break;
    case kItemFreq:
      snprintf(out, n, "%.1f", (double)(ed ? draft_f_ : cfg.freq_mhz));
      break;
    case kItemMode:
      snprintf(out, n, "%s",
               g_radio.ModeNameByIndex(ed ? draft_i_
                                          : g_radio.CurrentModeIndex()));
      break;
    case kItemEnc:
      snprintf(out, n, "%s",
               (ed ? draft_i_ : (cfg.feat & FEAT_ENC) != 0) ? "ON" : "OFF");
      break;
    case kItemComp:
      snprintf(out, n, "%s",
               (ed ? draft_i_ : (cfg.feat & FEAT_COMP) != 0) ? "ON" : "OFF");
      break;
    default:
      if (n) out[0] = 0;
  }
}

void Display::Draw() {
  g_oled.clearDisplay();
  // The config menu fills the full height (its rows would clip a border), so it
  // draws borderless; MAIN and INFO keep the framed look.
  if (screen_ != kScreenConfig)
    g_oled.drawRect(0, 0, kOledW, kOledH, SH110X_WHITE);
  switch (screen_) {
    case kScreenInfo:   DrawInfo();   break;
    case kScreenConfig: DrawConfig(); break;
    default:            DrawMain();   break;
  }
  g_oled.display();
}

void Display::DrawStatusCluster() {
  // The top-right connection cluster, drawn black on whatever inverted bar the
  // caller already laid down, so it's present on every screen. Signal meter is
  // RSSI-driven but forced to zero once the link goes quiet (no RX within
  // kStaleMs) — it reads "no connection" alongside the heart, which pulses on
  // each frame received from the peer. The arrows flash on TX/RX activity.
  uint32_t now = millis();
  bool linked = (uint32_t)(now - g_device.last_rx_ms()) < kStaleMs;
  int bars = (linked && g_radio.have_rssi()) ? RssiBars(g_radio.rssi()) : 0;
  for (int i = 0; i < 5; i++) {
    int h = 3 + i;                    // ascending: 3,4,5,6,7 px
    int x = 86 + i * 3, y = 9 - h;
    if (i < bars)
      g_oled.fillRect(x, y, 2, h, SH110X_BLACK);
    else
      g_oled.drawRect(x, y, 2, h, SH110X_BLACK);
  }
  bool tx_on = (int32_t)(tx_until_ms_ - now) > 0;
  bool rx_on = (int32_t)(rx_until_ms_ - now) > 0;
  if (tx_on)
    g_oled.fillTriangle(104, 8, 110, 8, 107, 2, SH110X_BLACK);
  else
    g_oled.drawTriangle(104, 8, 110, 8, 107, 2, SH110X_BLACK);
  if (rx_on)
    g_oled.fillTriangle(111, 2, 117, 2, 114, 8, SH110X_BLACK);
  else
    g_oled.drawTriangle(111, 2, 117, 2, 114, 8, SH110X_BLACK);
  if ((int32_t)(beat_until_ms_ - now) > 0)
    g_oled.drawBitmap(120, 2, kHeart, 7, 7, SH110X_BLACK);
}

void Display::DrawMain() {
  // --- inverted status bar (white fill, black content) ---
  g_oled.fillRect(0, 0, kOledW, kBarH, SH110X_WHITE);
  g_oled.setTextSize(1);
  g_oled.setTextColor(SH110X_BLACK);

  char freq[8];
  snprintf(freq, sizeof freq, "%.1f", (double)cfg.freq_mhz);
  g_oled.setCursor(2, 2);
  g_oled.print(freq);

  // Encryption padlock, right of the frequency: locked when AEAD is on, a
  // crossed-out lock when it's off.
  g_oled.drawBitmap(34, 2, kLock, 7, 7, SH110X_BLACK);
  if (!(cfg.feat & FEAT_ENC))
    g_oled.drawLine(33, 9, 41, 1, SH110X_BLACK);

  // Mode name, centered in the gap between the lock and the signal meter.
  const char* mode = ModeShort(g_radio.CurrentModeName());
  int mw = (int)strlen(mode) * 6;
  int mx = 44 + (84 - 44 - mw) / 2;
  if (mx < 44) mx = 44;
  g_oled.setCursor(mx, 2);
  g_oled.print(mode);

  DrawStatusCluster();   // signal meter, TX/RX arrows, heartbeat (top-right)

  // --- teletype (white on black, below the bar) ---
  // Row r shows the history line (scroll_ + rows-below) slots behind head_. At
  // scroll_ == 0 the bottom row is the live line; rolling the trackball up
  // walks the whole window back through the ring.
  g_oled.setTextColor(SH110X_WHITE);
  for (uint8_t r = 0; r < kRows; r++) {
    int dist = scroll_ + (kRows - 1 - r);   // lines this row sits behind head_
    if (dist > count_ - 1) continue;        // before start of history: blank
    int slot = ((int)head_ - dist + kScroll) % kScroll;
    g_oled.setCursor(1, 13 + r * 10);
    g_oled.print(lines_[slot]);
  }
  // Scrollbar on the right edge: the thumb's size is the visible fraction of
  // the buffer and its position is where the window sits — at the bottom means
  // the live tail (following input), higher means scrolled back into history.
  if (count_ > kRows) {
    const int ty0 = 12, th0 = 50;                    // track top / height
    int top = (int)count_ - kRows - (int)scroll_;    // oldest visible from top
    if (top < 0) top = 0;
    int th = th0 * kRows / (int)count_;              // thumb height
    if (th < 4) th = 4;
    int ty = ty0 + th0 * top / (int)count_;          // thumb top
    if (ty + th > ty0 + th0) ty = ty0 + th0 - th;
    g_oled.drawFastVLine(kOledW - 3, ty0, th0, SH110X_WHITE);   // track
    g_oled.fillRect(kOledW - 4, ty, 3, th, SH110X_WHITE);       // thumb
  }
}

void Display::DrawInfo() {
  // Inverted title (same bar height as the other screens), then a column of
  // read-only link/radio diagnostics.
  g_oled.setTextSize(1);
  g_oled.fillRect(0, 0, kOledW, kBarH, SH110X_WHITE);
  g_oled.setTextColor(SH110X_BLACK);
  g_oled.setCursor(2, 2);
  g_oled.print("INFO");
  DrawStatusCluster();   // connection cluster stays put across screens
  g_oled.setTextColor(SH110X_WHITE);

  uint32_t now = millis();
  bool linked = (uint32_t)(now - g_device.last_rx_ms()) < kStaleMs;
  char ln[24];
  int y = 13;
  snprintf(ln, sizeof ln, "Freq  %.1f MHz", (double)cfg.freq_mhz);
  g_oled.setCursor(3, y); g_oled.print(ln); y += 9;
  snprintf(ln, sizeof ln, "Mode  %s", g_radio.CurrentModeName());
  g_oled.setCursor(3, y); g_oled.print(ln); y += 9;
  if (g_radio.have_rssi())
    snprintf(ln, sizeof ln, "Sig   %d dBm %d dB", (int)g_radio.rssi(),
             (int)g_radio.snr());
  else
    snprintf(ln, sizeof ln, "Sig   --");
  g_oled.setCursor(3, y); g_oled.print(ln); y += 9;
  snprintf(ln, sizeof ln, "TX/reTX  %lu/%lu", (unsigned long)g_link.DbgStatTx(),
           (unsigned long)g_link.DbgStatRetx());
  g_oled.setCursor(3, y); g_oled.print(ln); y += 9;
  snprintf(ln, sizeof ln, "Link  %s   Up %lus", linked ? "UP" : "down",
           (unsigned long)(now / 1000));
  g_oled.setCursor(3, y); g_oled.print(ln);
}

void Display::DrawConfig() {
  g_oled.setTextSize(1);
  // Title: normally "CONFIG"; while editing a setting that only works if the
  // peer matches (freq/mode/enc/comp — local-apply), it warns instead.
  bool warn = editing_ && (sel_ == kItemFreq || sel_ == kItemMode ||
                           sel_ == kItemEnc || sel_ == kItemComp);
  // Borderless (see Draw): full-width title (same bar height as the other
  // screens), then six rows filling the remaining height. The title text is
  // kept short so it clears the top-right status cluster.
  g_oled.fillRect(0, 0, kOledW, kBarH, SH110X_WHITE);
  g_oled.setTextColor(SH110X_BLACK);
  g_oled.setCursor(2, 2);
  g_oled.print(warn ? "match peer" : "CONFIG");
  DrawStatusCluster();   // connection cluster stays put across screens

  for (uint8_t i = 0; i < kItemCount; i++) {
    int y = 11 + i * 9;
    bool selrow = (i == sel_);
    if (selrow) {   // highlight the selected row (inverted, full width)
      g_oled.fillRect(0, y - 1, kOledW, 9, SH110X_WHITE);
      g_oled.setTextColor(SH110X_BLACK);
    } else {
      g_oled.setTextColor(SH110X_WHITE);
    }
    g_oled.setCursor(2, y);
    g_oled.print(ItemLabel(i));

    // Value, right-aligned. While editing this row, wrap it in CP437 arrows
    // (◄ ►) to show left/right will change it.
    char val[12];
    ItemValue(i, val, sizeof val);
    char disp[20];
    if (selrow && editing_)
      snprintf(disp, sizeof disp, "\x11%s\x10", val);
    else
      snprintf(disp, sizeof disp, "%s", val);
    int vx = kOledW - 2 - (int)strlen(disp) * 6;
    g_oled.setCursor(vx, y);
    g_oled.print(disp);
  }
  g_oled.setTextColor(SH110X_WHITE);
}

void Display::Task(void*) {
  for (;;) {
    // Drain received bytes from the ring into the teletype.
    while (s_head != s_tail) {
      g_display.Ingest(s_ring[s_head]);
      s_head = (uint16_t)((s_head + 1) % kRingSz);
    }
    g_display.PollButton();
    g_display.PollTrackball();
    g_display.PollPress();

    // Latch the status-bar activity edges. last_rx_ms advances on every valid
    // frame (heart); the link TX-frame count feeds the up arrow; host-out bytes
    // (received data delivered) feed the down arrow.
    uint32_t now = millis();
    uint32_t rx = g_device.last_rx_ms();
    if (rx != g_display.prev_rx_ms_) {
      g_display.prev_rx_ms_ = rx;
      g_display.beat_until_ms_ = now + kBeatMs;
    }
    uint32_t txs = g_link.DbgStatTx();
    if (txs != g_display.prev_tx_) {
      g_display.prev_tx_ = txs;
      g_display.tx_until_ms_ = now + kFlickMs;
    }
    uint32_t ho = g_host.host_out();
    if (ho != g_display.prev_hout_) {
      g_display.prev_hout_ = ho;
      g_display.rx_until_ms_ = now + kFlickMs;
    }

    g_display.Draw();
    vTaskDelay(pdMS_TO_TICKS(kFrameMs));
  }
}

void Display::Init() {
  Wire.begin();   // MUST precede any I2C probe, or the bus transaction hangs
  Wire.setClock(400000);   // fast I2C so a frame push is short on the task

  // Restore the persisted display preferences (prefs is already open — Host::
  // LoadSettings ran first in setup()). Clamp in case flash holds a bad index.
  bright_ = prefs.getUChar("bri", bright_);
  if (bright_ >= kNumBright) bright_ = 1;
  region_ = prefs.getUChar("rgn", region_);
  if (region_ >= kNumRegions) region_ = 0;
  uint8_t addr = I2cPresent(kOledAddrA)   ? kOledAddrA
                 : I2cPresent(kOledAddrB) ? kOledAddrB
                                          : 0;
  oled_addr_ = addr;
  if (addr && g_oled.begin(addr, true)) {
    ok_ = true;
    g_oled.setTextSize(1);
    g_oled.setTextWrap(false);           // the teletype does its own wrapping
    g_oled.cp437(true);                  // full CP437 glyphs (bytes 0x80-0xFF)
    SetBrightness(bright_);
  }
  pinMode(kBtnPin, INPUT_PULLUP);
  // Trackball: the four direction pins pulse active-low (count falling edges
  // via ISRs); the press pin is a click we debounce-poll. Up/down scroll on
  // MAIN and move the selection on CONFIG; left/right edit; press enters/saves.
  pinMode(TB_UP, INPUT_PULLUP);
  pinMode(TB_DOWN, INPUT_PULLUP);
  pinMode(TB_LEFT, INPUT_PULLUP);
  pinMode(TB_RIGHT, INPUT_PULLUP);
  pinMode(TB_PRESS, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TB_UP), OnTbUp, FALLING);
  attachInterrupt(digitalPinToInterrupt(TB_DOWN), OnTbDown, FALLING);
  attachInterrupt(digitalPinToInterrupt(TB_LEFT), OnTbLeft, FALLING);
  attachInterrupt(digitalPinToInterrupt(TB_RIGHT), OnTbRight, FALLING);
  attachInterrupt(digitalPinToInterrupt(TB_PRESS), OnTbPress, FALLING);
  g_host_out_hook = &Display::FeedThunk;   // received bytes -> ring -> teletype
  if (ok_) {
    // Own low-priority task (priority 1 = TASK_PRIO_LOW, same as the Arduino
    // loop): its blocking I2C time-slices with the link loop instead of
    // stalling it, and sleeps kFrameMs between frames so the link keeps CPU.
    xTaskCreate(Display::Task, "disp", kTaskStack, nullptr, 1, nullptr);
  }
}
