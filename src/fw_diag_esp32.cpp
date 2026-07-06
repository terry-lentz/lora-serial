/**
 * @file fw_diag.cpp
 * @brief Diag class implementation (see fw_diag.h).
 *
 * The diagnostics layer is the `Diag` class, instantiated ONCE as the static
 * singleton `g_diag` (no heap — CLAUDE.md rule 5). The class owns the captured
 * boot state (reset reason, boot count, pre-crash uptime) and the
 * software-watchdog state (the esp_timer handle and the pet/miss counters) as
 * private members. The crash breadcrumb is the lone exception: it lives in RTC
 * memory and must stay a file-local static (see the long comment below).
 *
 * What this gives you to debug a dead board:
 *   - AT+DIAG reports: boot count, WHY the board last reset (power-on / panic /
 *     brownout / our software watchdog / clean), the uptime it reached just
 *     before that reset, current uptime, free + min-ever-free heap, and whether
 *     a core dump is stored in flash.
 *   - A software watchdog: if the main loop stops running for kWdtSecs (a
 *     hang — e.g. a radio driver stalling on the BUSY line), the board reboots
 *     itself and records it, instead of going silent.
 *   - Core dumps: the build already enables esp-coredump-to-flash, so a panic
 *     writes a full backtrace to the coredump partition; AT+DIAG flags it and
 *     tools/coredump.sh reads + decodes it. (A panic also prints a backtrace on
 *     UART0 — see docs/DIAGNOSTICS.md.)
 */
#include "fw_diag.h"

#include <Arduino.h>
#include <Preferences.h>

#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

/// NVS handle, opened ("loramodem") in Host::LoadSettings(); for boot count
extern Preferences prefs;

// The single diagnostics-layer instance (static singleton; no heap — rule 5).
Diag g_diag;

// The software-watchdog periodic timer (1 Hz). File-static rather than a class
// member because its type is ESP-IDF-specific — the portable Diag header can't
// name it (see fw_diag.h). Created/started in Init(), fires WdtTick().
static esp_timer_handle_t s_wdt_timer = nullptr;

// --------------------------------------------------------------------------
// Crash breadcrumb in RTC memory — WHY THIS IS NOT A CLASS MEMBER.
//
// RTC_NOINIT_ATTR places a variable in the RTC-memory segment, which survives a
// reset / panic / watchdog reboot (but NOT a power cycle). We use it to carry a
// breadcrumb across a crash so the next boot can report what happened.
//
// That attribute can ONLY apply to a variable with static storage duration in
// the RTC segment — it CANNOT be applied to a non-static class instance member.
// The Diag object (g_diag) lives in normal RAM, not the RTC segment, so its
// members would not survive a reboot even if we tried. So these four words and
// the magic that validates them stay file-local statics; the Diag methods below
// reference them directly. Do not try to fold them into the class.
// --------------------------------------------------------------------------
/**
 * @brief Sentinel proving the RTC words are ours (survived a reset, not garbage
 *        from a cold power-on): the ASCII-ish constant "LoRa SEED".
 */
static const uint32_t kRtcMagic = 0x10DA5EED;
/// RTC sentinel slot; equals kRtcMagic once the breadcrumb has been validated
RTC_NOINIT_ATTR static uint32_t s_rtc_magic;
RTC_NOINIT_ATTR static uint32_t s_rtc_uptime_ms;   ///< last-seen uptime (crumb)
RTC_NOINIT_ATTR static uint32_t s_rtc_wdt_fired;   ///< kRtcMagic if WDT reboot
RTC_NOINIT_ATTR static uint32_t s_rtc_noprog;      ///< kRtcMagic if no-progress
/// Radio-op breadcrumb (global; see fw_config.h). RTC_NOINIT so the last radio
/// op the loop entered (stamped by RMARK in fw_radio.cpp) survives a watchdog/
/// panic reboot for AT+DIAG to report — pinpoints a wedge's hung op.
RTC_NOINIT_ATTR volatile uint8_t g_dbg_stage;
/// RX-task breadcrumb (global; see fw_config.h). RTC_NOINIT, like g_dbg_stage.
RTC_NOINIT_ATTR volatile uint8_t g_dbg_rx;
/// Captured breadcrumbs at the moment of a wedge reboot. The watchdog paths
/// snapshot the live g_dbg_stage / g_dbg_rx into these slots BEFORE the reboot,
/// so the wedge ops survive intact — the post-reboot idle loop re-stamps the
/// live bytes, so reading those straight after a reboot would be meaningless.
/// AT+DIAG reports these as "wedgeop" (loop) and "rxop" (RX task).
RTC_NOINIT_ATTR static uint8_t s_rtc_wedge_op;
RTC_NOINIT_ATTR static uint8_t s_rtc_wedge_rx;

// IDF Task-Watchdog user hook: called from the TWDT ISR just before the panic
// reboot (CONFIG_ESP_TASK_WDT_PANIC=y here). We snapshot the live radio-op
// breadcrumb so a HARD loop-hang's location (the op the loop is stuck in)
// survives the reboot for AT+DIAG. Overrides the weak default declared in
// esp_task_wdt.h; keep it ISR-minimal (a single byte store, no logging/SPI).
extern "C" void esp_task_wdt_isr_user_handler(void) {
    s_rtc_wedge_op = g_dbg_stage;
    s_rtc_wedge_rx = g_dbg_rx;
}

void Diag::Init() {
    reason_ = (uint32_t)esp_reset_reason();
    bool rtc_valid = (s_rtc_magic == kRtcMagic);
    // Our software watchdog reboots via esp_restart() -> shows as ESP_RST_SW;
    // the RTC flag disambiguates it from a normal reflash/restart.
    was_wdt_ = rtc_valid && (s_rtc_wdt_fired == kRtcMagic);
    was_noprog_ = rtc_valid && (s_rtc_noprog == kRtcMagic);
    // The pre-crash uptime crumb is only meaningful if RTC survived (not a
    // power cycle) and it wasn't a clean power-on.
    prev_uptime_ms_ =
        (rtc_valid && reason_ != ESP_RST_POWERON) ? s_rtc_uptime_ms : 0;
    s_rtc_magic = kRtcMagic;
    s_rtc_wdt_fired = 0;
    s_rtc_noprog = 0;
    s_rtc_uptime_ms = 0;

    boots_ = prefs.getUInt("boots", 0) + 1;
    prefs.putUInt("boots", boots_);

    // Start the software watchdog (1 Hz tick counting missed pets).
    const esp_timer_create_args_t args = {
        .callback = &Diag::WdtTick, .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK, .name = "lora_wdt",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_wdt_timer) == ESP_OK)
        esp_timer_start_periodic(s_wdt_timer, 1000000ULL);   // 1 s

    // HARDWARE backstop: put the Arduino loop task under the IDF Task Watchdog
    // (CONFIG_ESP_TASK_WDT_PANIC=y here -> reboots on a hang; a panic reboots
    // too). This catches a deep hang that defeats the esp_timer watchdog
    // above (a continuous-RX wedge did exactly that). The 5 s TWDT is
    // fed every loop iteration by the framework, and during long radio waits by
    // feedLoopWDT() in Pet() (called from the rx idle hook), so legitimate
    // multi-second waits never trip it — only a true hang does.
    enableLoopWDT();
}

void Diag::Pet() {
    petted_ = true;
    s_rtc_uptime_ms = millis();   // refresh the pre-crash breadcrumb
    feedLoopWDT();                 // keep the hardware TWDT happy during waits
}

// Reboot now: the link wedged (no valid RX despite re-init + rendezvous).
void Diag::RebootNoProgress() {
    s_rtc_noprog = kRtcMagic;       // breadcrumb: blame a no-progress wedge
    s_rtc_wedge_op = g_dbg_stage;   // snapshot the loop + RX ops for AT+DIAG
    s_rtc_wedge_rx = g_dbg_rx;
    s_rtc_uptime_ms = millis();
    esp_restart();
}

size_t Diag::Report(char* buf, size_t cap) {
    // Internal SRAM is the scarce, crash-relevant pool (~512 KB); PSRAM (~6 MB,
    // holds the ingest ring) rarely matters. Report internal free + min-ever.
    const uint32_t cap_int = MALLOC_CAP_INTERNAL;
    uint32_t freek = heap_caps_get_free_size(cap_int) / 1024;
    uint32_t mink  = heap_caps_get_minimum_free_size(cap_int) / 1024;
    uint32_t cd_addr = 0, cd_size = 0;
    bool coredump = (esp_core_dump_image_get(&cd_addr, &cd_size) == ESP_OK)
                    && cd_size > 0;
    return (size_t)snprintf(buf, cap,
        "boots=%lu lastreset=%s ranbefore=%lus uptime=%lus "
        "iram=%luK miniram=%luK coredump=%s wedgeop=%c rxop=%c\r\n",
        (unsigned long)boots_, ReasonStr(),
        (unsigned long)(prev_uptime_ms_ / 1000),
        (unsigned long)(millis() / 1000),
        (unsigned long)freek, (unsigned long)mink,
        coredump ? "YES (tools/coredump.sh)" : "no",
        (s_rtc_wedge_op >= 32 && s_rtc_wedge_op < 127)
            ? (char)s_rtc_wedge_op : '?',
        (s_rtc_wedge_rx >= 32 && s_rtc_wedge_rx < 127)
            ? (char)s_rtc_wedge_rx : '?');
}

// Software-watchdog tick: a 1 Hz esp_timer counts ticks with no pet and reboots
// the board once kWdtSecs go by un-petted (a hung loop). Static member: reaches
// the singleton through g_diag to touch its privates; writes the RTC breadcrumb
// directly (it isn't a class member — see above).
void Diag::WdtTick(void*) {
    if (g_diag.petted_) {
        g_diag.petted_ = false;
        g_diag.misses_ = 0;
        return;
    }
    if (++g_diag.misses_ >= kWdtSecs) {
        s_rtc_wdt_fired = kRtcMagic;   // breadcrumb: blame the watchdog
        s_rtc_uptime_ms = millis();
        esp_restart();                 // hung loop -> reboot
    }
}

// Human-readable name of the last reset cause (for the AT+DIAG report). Our two
// software reboots (watchdog / no-progress) both surface as ESP_RST_SW, so the
// RTC breadcrumb flags disambiguate them and take precedence here.
const char* Diag::ReasonStr() const {
    if (was_noprog_) return "NO-PROGRESS (radio wedge reboot)";
    if (was_wdt_) return "SW-WATCHDOG (loop hang)";
    switch (reason_) {
        case ESP_RST_POWERON:  return "power-on";
        case ESP_RST_SW:       return "software (reflash/restart)";
        case ESP_RST_PANIC:    return "PANIC (crash)";
        case ESP_RST_INT_WDT:  return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT:      return "other watchdog";
        case ESP_RST_BROWNOUT: return "BROWNOUT (power dip)";
        case ESP_RST_DEEPSLEEP:return "deep-sleep wake";
        case ESP_RST_EXT:      return "external reset";
        default:               return "unknown";
    }
}
