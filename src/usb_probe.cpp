// usb_probe.cpp — DIAGNOSTIC-ONLY build (env:usbprobe, -D USB_PHONE_PROBE).
//
// Purpose: find out what a USB host (esp. Termius on Android) does to our CDC
// port when device->host output "shows nothing". The board is on the phone, so
// we can't watch it live — instead we LOG a per-second timeline to NVS (which
// survives the unplug) and dump it when the board is brought back to a PC.
//
// The decisive signal is whether the host DRAINS our IN endpoint (i.e. reads):
//   wav   = tud_cdc_write_available() — free space in the 64-byte CDC IN FIFO.
//   wfail = seconds a write couldn't fully complete (FIFO stayed full).
// If the host reads, wav stays ~64 and wfail stays 0. If it never reads, the
// FIFO fills after one line and wfail climbs every second — proof the host
// isn't issuing IN transfers. We also log DTR/RTS/baud (what the host set),
// host->device byte count (did it send anything), and connect/disconnect count.
//
// Flow: flash on PC -> move to phone, open Termius ~30-60 s -> back to PC. On
// the PC, this build re-dumps the PREVIOUS (phone) session every few seconds,
// so whichever terminal you open catches it. Compiles to nothing without
// USB_PHONE_PROBE; never affects node_raw. Not held to the teaching-grade bar.
#ifdef USB_PHONE_PROBE

#include <Arduino.h>
#include <Preferences.h>

#include <cstdio>
#include <cstring>

#include "USBCDC.h"             // enableReboot(), onEvent(), CDC event enums
#include "esp32-hal-tinyusb.h"  // tud_cdc_write/_flush/_available/_connected

namespace {

// ---- Host control-signal state, captured from USBCDC events -----------------
volatile int32_t g_dtr = -1;    // -1 = never set by a host
volatile int32_t g_rts = -1;
volatile uint32_t g_baud = 0;
volatile uint32_t g_ls_n = 0;   // SET_CONTROL_LINE_STATE count (DTR/RTS)
volatile uint32_t g_lc_n = 0;   // SET_LINE_CODING count (baud/parity)
volatile uint32_t g_cn_n = 0;   // CONNECTED events
volatile uint32_t g_dc_n = 0;   // DISCONNECTED events
volatile uint32_t g_rx = 0;     // host->device bytes seen

void UsbCdcEvent(void*, esp_event_base_t, int32_t id, void* data) {
  auto* d = static_cast<arduino_usb_cdc_event_data_t*>(data);
  switch (id) {
    case ARDUINO_USB_CDC_LINE_STATE_EVENT:
      g_dtr = d->line_state.dtr; g_rts = d->line_state.rts; g_ls_n++; break;
    case ARDUINO_USB_CDC_LINE_CODING_EVENT:
      g_baud = d->line_coding.bit_rate; g_lc_n++; break;
    case ARDUINO_USB_CDC_CONNECTED_EVENT:    g_cn_n++; break;
    case ARDUINO_USB_CDC_DISCONNECTED_EVENT: g_dc_n++; break;
    case ARDUINO_USB_CDC_RX_EVENT:           g_rx += d->rx.len; break;
    default: break;
  }
}

// ---- Persistent per-second timeline (survives the unplug via NVS) -----------
struct Rec {
  uint16_t t;            // seconds into the session
  uint8_t conn, dtr, rts;
  uint16_t wav;          // IN-FIFO free space this second
  uint16_t wfail;        // cumulative seconds a write couldn't complete
  uint32_t rx;           // cumulative host->device bytes
  uint32_t baud;
  uint16_t ls, lc, cn, dc;
};
const int kRecMax = 60;  // ~60 s of timeline
Rec g_ring[kRecMax];
int g_ring_n = 0;
uint16_t g_wfail = 0;

Rec g_prev[kRecMax];     // previous session, loaded from NVS at boot
int g_prev_n = 0;

Preferences g_prefs;
const char* kNs = "probelog";
const char* kKey = "ring";

// Save the current timeline so it survives the next power cycle (unplug).
void PersistRing() {
  g_prefs.begin(kNs, false);
  g_prefs.putUShort("n", (uint16_t)g_ring_n);
  g_prefs.putBytes(kKey, g_ring, sizeof(Rec) * g_ring_n);
  g_prefs.end();
}

// Load the previous session's timeline into g_prev at boot.
void LoadPrev() {
  g_prefs.begin(kNs, true);
  uint16_t n = g_prefs.getUShort("n", 0);
  if (n > kRecMax) n = kRecMax;
  size_t got = g_prefs.getBytes(kKey, g_prev, sizeof(Rec) * n);
  g_prev_n = (int)(got / sizeof(Rec));
  g_prefs.end();
}

// Emit one line via TinyUSB direct (no DTR gate). Returns bytes accepted.
uint32_t Put(const char* s, int n) {
  uint32_t w = tud_cdc_write(s, (uint32_t)n);
  tud_cdc_write_flush();
  return w;
}

// Dump the previous (phone/Termius) session: the timeline + a verdict summary.
void DumpPrev() {
  char b[128];
  int n = snprintf(b, sizeof(b),
                   "\r\n=== PREV SESSION (%d s) ===\r\n", g_prev_n);
  Put(b, n);
  uint16_t min_wav = 0xFFFF, max_wfail = 0;
  uint32_t max_rx = 0;
  for (int i = 0; i < g_prev_n; i++) {
    Rec& r = g_prev[i];
    if (r.wav < min_wav) min_wav = r.wav;
    if (r.wfail > max_wfail) max_wfail = r.wfail;
    if (r.rx > max_rx) max_rx = r.rx;
    n = snprintf(b, sizeof(b),
                 "t=%u c=%u d=%u r=%u wav=%u wf=%u rx=%lu bd=%lu "
                 "ls=%u lc=%u cn=%u dc=%u\r\n",
                 r.t, r.conn, r.dtr, r.rts, r.wav, r.wfail,
                 (unsigned long)r.rx, (unsigned long)r.baud, r.ls, r.lc,
                 r.cn, r.dc);
    Put(b, n);
    delay(3);
  }
  const char* verdict = (g_prev_n == 0) ? "no data"
      : (max_wfail > 2 && min_wav == 0) ? "host did NOT read (IN FIFO filled)"
      : "host WAS reading (FIFO drained) -> display-side issue";
  n = snprintf(b, sizeof(b),
               "SUMMARY min_wav=%u max_wfail=%u max_rx=%lu -> %s\r\n"
               "=== END PREV ===\r\n",
               (min_wav == 0xFFFF ? 0 : min_wav), max_wfail,
               (unsigned long)max_rx, verdict);
  Put(b, n);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  // Reboot LEFT ENABLED (default): normal terminals don't walk the 4-step
  // DTR/RTS sequence that fires the bootloader, so it's safe — and it lets
  // tools/upload_flash.sh reset this board into the bootloader button-free.
  Serial.onEvent(UsbCdcEvent);  // capture what the host sets (DTR/RTS/baud)
  LoadPrev();                   // pull the last session's log out of NVS
}

void loop() {
  static uint32_t sec = 0;
  static uint32_t last_persist = 0;
  static uint32_t last_dump = 0;

  // Drain host->device so its queue never stalls (bytes are counted via event).
  while (Serial.available()) Serial.read();

  // Live heartbeat (kept < 64 B so a full FIFO is the ONLY reason a write
  // can't complete — that makes wfail a clean "host isn't reading" signal).
  uint16_t wav = (uint16_t)tud_cdc_write_available();
  char hb[64];
  int n = snprintf(hb, sizeof(hb),
                   "[HB] t=%lu c%d d%ld r%ld wav%u rx%lu wf%u\r\n",
                   (unsigned long)sec, tud_cdc_connected() ? 1 : 0,
                   (long)g_dtr, (long)g_rts, wav, (unsigned long)g_rx, g_wfail);
  // #2572 FIX UNDER TEST: only write when the terminal is open
  // (tud_cdc_connected == DTR). Writing into the IN endpoint before a host
  // reads can leave the CDC IN pipe "stuck busy/claimed" so the host then
  // reads nothing — the exact Termius symptom. Gating avoids pre-saturating.
  if (tud_cdc_connected() && Put(hb, n) < (uint32_t)n) g_wfail++;

  // Record this second into the persistent timeline.
  if (g_ring_n < kRecMax) {
    Rec& r = g_ring[g_ring_n++];
    r.t = (uint16_t)sec;
    r.conn = tud_cdc_connected() ? 1 : 0;
    r.dtr = g_dtr > 0 ? 1 : 0;
    r.rts = g_rts > 0 ? 1 : 0;
    r.wav = wav;
    r.wfail = g_wfail;
    r.rx = g_rx;
    r.baud = g_baud;
    r.ls = (uint16_t)g_ls_n; r.lc = (uint16_t)g_lc_n;
    r.cn = (uint16_t)g_cn_n; r.dc = (uint16_t)g_dc_n;
  }

  if (sec - last_persist >= 2) { PersistRing(); last_persist = sec; }

  // Re-dump the previous (phone) session periodically so a terminal opened on
  // the PC after boot still catches it.
  if (g_prev_n && tud_cdc_connected() && (sec == 0 || sec - last_dump >= 6)) {
    DumpPrev();
    last_dump = sec;
  }

  sec++;
  delay(1000);
}

#endif  // USB_PHONE_PROBE
