/**
 * @file fw_display.h
 * @brief OLED front-end for the receive-and-display node (e.g. the Wio Tracker
 *        L1): an inverted status bar — frequency, mode, a 5-bar signal meter,
 *        and a heartbeat — above a 4-line teletype that renders the received
 *        serial stream in real time.
 *
 * One static instance, g_display (fw_display.cpp), built only on boards that
 * declare HAS_DISPLAY (see platformio.ini build_src_filter). Init() brings up
 * the SH1106 and wires the device->host byte tap (g_host_out_hook) so received
 * data flows into the teletype; Tick() redraws the panel and services the
 * brightness button. The OLED driver object itself is a file-static in the .cpp
 * so this header stays free of display-library includes.
 */
#ifndef LORA_SERIAL_FW_DISPLAY_H_
#define LORA_SERIAL_FW_DISPLAY_H_

#include <stddef.h>
#include <stdint.h>

/// Receive-and-display OLED front-end (see file comment). Static singleton.
class Display {
 public:
  /**
   * @brief Bring up the OLED and wire the received-byte tap.
   *
   * Probes the SH1106 (0x3C/0x3D), applies the default brightness, and sets
   * g_host_out_hook so host-bound bytes feed the teletype. Safe to call once
   * from setup() on a HAS_DISPLAY board.
   */
  void Init();

  /**
   * @brief Periodic tick: redraw the panel (throttled) and poll the button.
   *
   * Call from the main loop. Cheap when nothing changed; the actual I2C redraw
   * is rate-limited. The status bar reads live freq/mode/RSSI; the heart pulses
   * from the link's last-RX timestamp.
   */
  void Tick();

 private:
  /// Teletype geometry: 21 columns x 4 rows in the standard 6x8 font.
  static const uint8_t kCols = 21;
  static const uint8_t kRows = 4;

  /**
   * @brief Static thunk matching g_host_out_hook's signature; forwards the
   *        bytes to the singleton's Feed().
   * @param[in] data  received bytes. @param[in] len  count.
   */
  static void FeedThunk(const uint8_t* data, size_t len);

  /**
   * @brief Append received bytes to the teletype, char by char.
   * @param[in] data  bytes to render. @param[in] len  count.
   */
  void Feed(const uint8_t* data, size_t len);

  /** @brief Scroll the teletype up one line and clear the new bottom line. */
  void NewLine();

  /** @brief Redraw the status bar + teletype to the OLED. */
  void Draw();

  /** @brief Debounced brightness-button poll (cycles the contrast steps). */
  void PollButton();

  // ---- Teletype state ------------------------------------------------------
  char    rows_[kRows][kCols + 1] = {{0}};  ///< visible lines (NUL-terminated)
  uint8_t col_ = 0;                         ///< cursor column on the bottom row
  bool    ok_ = false;                      ///< OLED found + initialised
  bool    dirty_ = true;                    ///< content changed since last Draw

  // ---- Status / timing -----------------------------------------------------
  uint8_t  bright_ = 1;         ///< index into the contrast table (boot = MED)
  uint32_t last_draw_ms_ = 0;   ///< redraw rate-limit clock
  uint32_t prev_rx_ms_ = 0;     ///< last-seen g_device.last_rx_ms (heart edge)
  uint32_t beat_until_ms_ = 0;  ///< heart shown filled until this millis()

  // ---- Button debounce -----------------------------------------------------
  bool     btn_stable_ = true;  ///< last accepted (released) level
  bool     btn_last_ = true;    ///< last raw sample
  uint32_t btn_changed_ms_ = 0; ///< when the raw level last changed
};

/// The single display-front-end instance (static singleton; no heap — rule 5).
extern Display g_display;

#endif  // LORA_SERIAL_FW_DISPLAY_H_
