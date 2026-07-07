/**
 * @file fw_display.h
 * @brief OLED front-end for the receive-and-display node (e.g. the Wio Tracker
 *        L1): an inverted status bar — frequency, mode, a 5-bar signal meter,
 *        TX/RX activity arrows, and a heartbeat — above a 5-line teletype (with
 *        trackball scrollback) that renders the received serial stream live.
 *
 * Runs on its OWN low-priority FreeRTOS task so the OLED's blocking I2C never
 * stalls the half-duplex turn loop (the radio uses the same pattern). Received
 * bytes arrive on the link loop via the device->host tap (g_host_out_hook) and
 * are handed to the task through a lock-free SPSC ring; the task owns the
 * teletype/OLED and reads live link stats for the status bar. One static
 * instance, g_display, built only on boards that declare HAS_DISPLAY.
 */
#ifndef LORA_SERIAL_FW_DISPLAY_H_
#define LORA_SERIAL_FW_DISPLAY_H_

#include <stddef.h>
#include <stdint.h>

/// Receive-and-display OLED front-end (see file comment). Static singleton.
class Display {
 public:
  /**
   * @brief Bring up the OLED, wire the received-byte tap, and start the task.
   *
   * Probes the SH1106 (0x3C/0x3D), applies the default brightness, sets
   * g_host_out_hook so host-bound bytes feed the teletype, and spawns the
   * refresh task. Call once from setup() on a HAS_DISPLAY board.
   */
  void Init();

  /**
   * @brief Apply any config-menu change confirmed on the display task.
   *
   * Call once per iteration from the MAIN loop. A confirmed edit only stashes
   * its target value; this performs the actual radio/link/flash apply + persist
   * in the main-loop context so it can't race the RX task (see ConfirmEdit).
   * A no-op when nothing is pending, and on boards without a display.
   */
  void ServiceConfig();

 private:
  /// Teletype geometry: 21 columns x 5 visible rows in the standard 6x8 font
  /// (10-px line pitch below the 11-px status bar), backed by a kScroll-line
  /// history ring the trackball can scroll back through.
  static const uint8_t  kCols = 21;
  static const uint8_t  kRows = 5;
  static const uint16_t kScroll = 64;   ///< scrollback history depth (lines)

  /// The screens the main button cycles through: live teletype, read-only
  /// diagnostics, and the editable settings menu.
  enum Screen : uint8_t { kScreenMain, kScreenInfo, kScreenConfig };

  /// Editable rows on the config screen, in display order. Brightness previews
  /// live; the rest apply on confirm. Frequency/mode/encryption/compression all
  /// need the peer to match (local-apply — see the on-screen hint).
  enum ConfigItem : uint8_t {
    kItemBright,   ///< display brightness (FULL/MED/LOW)
    kItemRegion,   ///< frequency region (TW/US/EU) — sets the freq range
    kItemFreq,     ///< carrier frequency (MHz), clamped to the region
    kItemMode,     ///< speed preset (turbo..far/ludicrous)
    kItemPower,    ///< TX power (dBm)
    kItemAutoPwr,  ///< auto TX-power (FEAT_APWR) on/off — experimental
    kItemEnc,      ///< encryption on/off
    kItemComp,     ///< compression on/off
    kItemFS,       ///< forward secrecy (FEAT_FS) on/off
    kItemGfsk,     ///< ADR may use the GFSK rung (FEAT_GFSK) on/off
    kItemCount     ///< number of items (not an item)
  };

  /// What to re-apply after a boolean feature flag is toggled.
  enum FeatAction : uint8_t {
    kFeatApplyLink,     ///< reinit the link with the new feature set
    kFeatSessionReset,  ///< re-handshake the per-session key
    kFeatNone           ///< nothing beyond persisting the flag
  };

  /// A boolean feature-flag config row. The on/off items are identical bar the
  /// bit they flip and the post-change action, so they're driven by this table
  /// (see FeatFor) instead of repeating the same case in every menu switch.
  struct FeatToggle {
    uint8_t     item;    ///< the ConfigItem this row is
    const char* label;   ///< menu label
    uint8_t     bit;     ///< the FEAT_* bit it controls
    FeatAction  action;  ///< what to re-apply when it changes
  };

  /**
   * @brief The FeatToggle for a config item, or null if it isn't a bool flag.
   * @param[in] item  the ConfigItem index.
   * @return its FeatToggle, or nullptr for the non-boolean items.
   */
  static const FeatToggle* FeatFor(uint8_t item);

  /**
   * @brief The refresh task body: drain the byte ring into the teletype, poll
   *        the button, refresh the status bar, and push a frame — then sleep.
   * @param[in] arg  unused FreeRTOS task argument.
   */
  static void Task(void* arg);

  /**
   * @brief Static tap matching g_host_out_hook; enqueues bytes to the ring.
   * @param[in] data  received bytes. @param[in] len  count.
   */
  static void FeedThunk(const uint8_t* data, size_t len);

  /** @brief Append one received byte to the teletype (wrap/scroll/control). */
  void Ingest(uint8_t c);

  /** @brief Commit the current line; open a fresh one in the history ring. */
  void NewLine();

  /** @brief Redraw the current screen to the OLED (runs on the task). */
  void Draw();

  /**
   * @brief Draw the top-right connection cluster (signal, TX/RX, heartbeat).
   *
   * Black content over an inverted bar the caller already drew. Called by
   * DrawStatusBar, so it appears on every screen and stays put across screens.
   */
  void DrawStatusCluster();

  /**
   * @brief Draw the shared inverted status bar (identical on every screen).
   * @param[in] label  left-side title (INFO/CONFIG), or null for MAIN which
   *                   draws its own richer freq/lock/mode content.
   */
  void DrawStatusBar(const char* label);

  /**
   * @brief Draw the right-margin scrollbar thumb for a scrolling list.
   * @param[in] top    index of the first visible row. @param[in] total  rows.
   */
  void DrawScrollbar(uint8_t top, uint8_t total);

  /**
   * @brief Draw one CONFIG/INFO row: label left, value right-aligned.
   * @param[in] r      visible row (0..kRowsVisible-1). @param[in] label  left.
   * @param[in] value  right-aligned value. @param[in] sel  highlight this row.
   * @param[in] edit   wrap the value in edit arrows (CONFIG, while editing).
   */
  void DrawRow(uint8_t r, const char* label, const char* value, bool sel,
               bool edit);

  /**
   * @brief Live-link check: a peer frame heard within kStaleMs.
   * @return true if the link is currently live.
   */
  bool Linked();

  /**
   * @brief Battery charge as a percentage.
   * @return 0..100, or -1 when the board has no battery sense.
   */
  int BatteryPct();

  /** @brief Draw the main screen: status bar + scrollable teletype. */
  void DrawMain();

  /** @brief Draw the read-only INFO screen (link + radio diagnostics). */
  void DrawInfo();

  /**
   * @brief Static label for an INFO row.
   * @param[in] i  the INFO row index (0..kInfoCount-1). @return the label.
   */
  static const char* InfoLabel(uint8_t i);

  /**
   * @brief Format an INFO row's value (right-aligned column).
   * @param[in]  i    the INFO row index. @param[out] out  buffer.
   * @param[in]  n    buffer capacity.
   */
  void InfoValue(uint8_t i, char* out, size_t n);

  /** @brief Draw the CONFIG menu (the editable settings list). */
  void DrawConfig();

  /**
   * @brief Debounced main-button poll: cycle screens, or cancel an active edit.
   *
   * The one hardware button is "back/escape": while editing a config item it
   * cancels that edit; otherwise it advances to the next screen.
   */
  void PollButton();

  /** @brief Apply a brightness step (index into kBright) to the OLED. */
  void SetBrightness(uint8_t level);

  /**
   * @brief Consume trackball direction edges and dispatch them by screen.
   *
   * The trackball's direction pins pulse (falling edge) as it rolls; ISRs count
   * the pulses and this drains them. On MAIN, up/down scroll the teletype
   * history. On CONFIG, up/down move the selection and (while editing)
   * left/right change the selected item's value.
   */
  void PollTrackball();

  /**
   * @brief Debounced trackball-press poll (the click): enter/confirm an edit.
   *
   * On CONFIG: press enters edit mode on the selected item, or — if already
   * editing — confirms and applies the change. Ignored on the other screens.
   */
  void PollPress();

  /**
   * @brief Auto-repeat step count for a held scroll direction.
   * @param[in]     pin    the direction pin (TB_UP / TB_DOWN).
   * @param[in,out] since  hold-start timestamp; reset to 0 when released.
   * @param[in]     now    current millis().
   * @return lines to repeat this frame (0 until held past the initial delay).
   */
  int HoldRepeat(int pin, uint32_t* since, uint32_t now);

  /** @brief Begin editing the selected config item (seed the draft value). */
  void EnterEdit();

  /** @brief Discard the in-progress edit (revert a live brightness preview). */
  void CancelEdit();

  /** @brief Apply + persist the edited value, then leave edit mode. */
  void ConfirmEdit();

  /**
   * @brief Step the in-edit draft value.
   * @param[in] dir  -1 for left (previous/decrease), +1 for right (next).
   */
  void EditStep(int dir);

  /**
   * @brief Static label text for a config row.
   * @param[in] i  the ConfigItem index.
   * @return the row label.
   */
  static const char* ItemLabel(uint8_t i);

  /**
   * @brief Render a config row's value (the draft while editing, else current).
   * @param[in]  i    the ConfigItem index.
   * @param[out] out  destination buffer. @param[in] n  buffer capacity.
   */
  void ItemValue(uint8_t i, char* out, size_t n);

  // ---- Teletype + scrollback history (owned by the task) ------------------
  // The received stream is a ring of kScroll lines; kRows of them are shown at
  // once. head_ is the current (bottom, being-typed) line; scroll_ walks the
  // window back through history and snaps to 0 (live) at the newest line.
  char     lines_[kScroll][kCols + 1] = {{0}};  ///< history ring (NUL-term)
  uint16_t head_ = 0;            ///< ring slot of the current (bottom) line
  uint16_t count_ = 1;           ///< lines retained in the ring (1..kScroll)
  uint16_t scroll_ = 0;          ///< rows scrolled back from live (0 = newest)
  uint32_t up_since_ = 0;        ///< millis TB_UP held (0 = released)
  uint32_t dn_since_ = 0;        ///< millis TB_DOWN held (0 = released)
  uint8_t  col_ = 0;             ///< cursor column on the current line
  bool     pending_nl_ = false;  ///< '\n' seen; scroll on next char
  uint8_t  oled_addr_ = 0;       ///< probed panel I2C address (raw pre-charge)
  bool     ok_ = false;          ///< OLED found + initialised

  // ---- Status-bar activity edges (task-local) ------------------------------
  uint8_t  bright_ = 1;         ///< index into the contrast table (boot = MED)
  uint32_t prev_rx_ms_ = 0;     ///< last-seen g_device.last_rx_ms (heart edge)
  uint32_t beat_until_ms_ = 0;  ///< heart shown filled until this millis()
  uint32_t prev_tx_ = 0;        ///< last-seen g_link TX-frame count (up arrow)
  uint32_t tx_until_ms_ = 0;    ///< up arrow shown filled until this millis()
  uint32_t prev_hout_ = 0;      ///< last-seen host-out byte count (down arrow)
  uint32_t rx_until_ms_ = 0;    ///< down arrow shown filled until this millis()
  uint16_t batt_mv_ = 0;        ///< last battery read (mV); 0 = no batt sense
  uint32_t batt_read_ms_ = 0;   ///< millis of the last battery ADC sample

  // ---- Screen / config-menu state (owned by the task) ----------------------
  Screen  screen_ = kScreenMain;  ///< current screen (main button cycles it)
  uint8_t sel_ = 0;               ///< selected config row (a ConfigItem)
  uint8_t cfg_top_ = 0;           ///< first visible CONFIG row (scroll window)
  uint8_t info_top_ = 0;          ///< first visible INFO row (scroll window)
  bool    editing_ = false;       ///< editing the selected row's value
  int     draft_i_ = 0;           ///< in-edit draft for index/bool items
  float   draft_f_ = 0.0f;        ///< in-edit draft for the frequency item
  uint8_t region_ = 0;            ///< selected frequency region (kRegions idx)

  // Deferred config apply: ConfirmEdit (display task) stashes the target here
  // and raises apply_pending_ last; ServiceConfig (main loop) consumes it. All
  // volatile so the stash writes are ordered before the flag is seen set.
  volatile uint8_t apply_item_ = 0;      ///< which ConfigItem to apply
  volatile int     apply_ival_ = 0;      ///< pending integer/bool/index value
  volatile float   apply_fval_ = 0.0f;   ///< pending float value (frequency)
  volatile bool    apply_pending_ = false;  ///< a confirmed change awaits apply

  // A coordinated mode switch (initiator) settles asynchronously; persist it
  // only once the radio actually reaches the target (see ServiceConfig).
  bool mode_save_pending_ = false;  ///< a mode switch awaits its settle+save
  int  mode_save_target_ = 0;       ///< the mode index we're switching to

  // ---- Button + trackball-press debounce -----------------------------------
  bool     btn_stable_ = true;  ///< last accepted (released) level
  bool     btn_last_ = true;    ///< last raw sample
  uint32_t btn_changed_ms_ = 0; ///< when the raw level last changed
  uint32_t prs_last_ms_ = 0;    ///< millis of the last accepted press; debounce
};

/// The single display-front-end instance (static singleton; no heap — rule 5).
extern Display g_display;

#endif  // LORA_SERIAL_FW_DISPLAY_H_
