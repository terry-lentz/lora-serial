/**
 * @file fw_host.cpp
 * @brief Host class implementation (see fw_host.h).
 *
 * The host layer is the `Host` class, instantiated ONCE as the static singleton
 * `g_host` (no heap in the hot path — CLAUDE.md rule 5; the single PSRAM ingest
 * ring is claimed once at boot in IngestInit() and never freed). The class owns
 * the host I/O rings, the AT command-mode state machine, the on-device
 * speed-test state, and the host byte-flow counters as private members; the
 * rest of the firmware reaches them through the public methods. The file-static
 * tuning constants below are used only by this translation unit.
 */
#include "fw_host.h"

#include <cstring>

#include "esp_heap_caps.h"   // internal-SRAM heap metric for AT+LINK?

#include "autopower.h"  // link_layer::AutoPowerStep — peer-SNR power loop
#include "fw_device.h"  // g_device: role, pairing, TX gap, recovery counters
#include "fw_diag.h"    // g_diag.Pet (watchdog) + g_diag.Report (AT+DIAG)
#include "fw_radio.h"   // g_radio: Tx/Rx, ApplyMode, mode helpers
#include "fw_session.h" // SessionReset (re-handshake the per-session key)

// The single host-layer instance (static singleton; no heap — rule 5).
Host g_host;

// --------------------------------------------------------------------------
// Buffer-size / timing tuning (file-local: only this translation unit uses it).
// All times are milliseconds, all sizes are bytes, unless noted.
// kHostTxRingBytes and kAtLineMax are member constants (they bound member
// arrays) and live in fw_host.h; the rest stay file-local here.
// --------------------------------------------------------------------------
/**
 * @brief PSRAM host->link ingest ring size: 2 MB when PSRAM is present, so any
 *        realistic `cat bigfile` fits and nothing is dropped while the LoRa
 *        link drains it.
 */
static constexpr size_t   kIngestWantBytes  = 2u * 1024 * 1024;
/// graceful fallback ingest ring in internal RAM when no PSRAM is available
static constexpr size_t   kIngestFallBytes  = 48u * 1024;
/**
 * @brief Stop pulling from the ingest ring once free space drops below this,
 *        leaving a little slack so a bulk read isn't split awkwardly.
 */
static constexpr size_t   kIngestLowFree    = 8;
/**
 * @brief Bulk USB read chunk: read up to this many waiting bytes per pass (bulk
 *        reads dodge per-byte USB lock contention).
 */
static constexpr size_t   kBulkReadBytes    = 512;
/**
 * @brief AT reply formatting scratch (chars) used by snprintf for status
 *        strings. Must fit the longest reply — AT+LINK? (rssi..retx +
 *        reinit/rendz + heap).
 */
static constexpr size_t   kAtScratchMax     = 160;
/// PumpLinkOut() per-pass read buffer (bytes) feeding the host ring
static constexpr size_t   kPumpChunkBytes   = 240;
/**
 * @brief Keep at least this much host-ring free space (bytes) as recv headroom;
 *        below it, stop reading from the link so the peer holds + retransmits.
 */
static constexpr size_t   kHostRingHeadroom = 8;
/**
 * @brief Extra ring-headroom margin (bytes) subtracted from a read request so a
 *        Read never exactly fills the ring.
 */
static constexpr size_t   kPumpReadMargin   = 4;
// "+++" Hayes escape: guard time (ms) of host silence required before AND after
// the plus run, and the number of '+' that make the escape.
static constexpr uint32_t kEscapeGuardMs    = 1000;  ///< pre/post guard silence
static constexpr uint8_t  kEscapePlusCount  = 3;     ///< '+' count for escape
/// MaybeStatus()/auto-power cadence (ms): min gap between TX-power re-evals
static constexpr uint32_t kStatusPeriodMs   = 1500;
/// lowest TX power (dBm) the peer-SNR auto-power loop will drop to
static const int8_t       kApPwrFloorDbm    = -9;

// ---- Non-blocking host output ring ----
// A slow terminal must NEVER block the radio loop: a blocking Serial.write
// would freeze turn-taking and wedge the whole link. So every host-bound byte
// goes through host_tx_, drained only as fast as the port's TX FIFO has room.
// When it backs up, PumpLinkOut() stops pulling from the link -> recv-ring
// backpressure -> the peer holds + retransmits. No blocking, no drops.
#define HOSTPORT Serial   ///< the host serial port (USB-CDC) used for I/O

/**
 * @brief Auto-exit AT command mode after this much idle, so a forgotten session
 *        (or a tool that entered AT mode and died) can't strand the board out
 *        of its data pipe. Reset on every AT-mode keystroke; only applies while
 *        in AT mode. (60 s.)
 */
static constexpr uint32_t kAtIdleExitMs = 60000;

// ---- On-device link test: throughput self-test + sink ----------------------
// AT+SPEEDTEST=<kb> (initiator) generates incompressible data INTERNALLY and
// measures how fast the link drains it (peer receives + ACKs) — no host USB
// streaming, so the measurement isn't skewed by host-side USB/EMI (which was
// raising the initiator's noise floor and confounding bench numbers). The peer
// must absorb the data: AT+SINK=1 makes a node drain+discard received bytes so
// no host is needed at the far end. Reports KB/s + retx + snr/rssi/pwr.
// Per-test deadline = base + a per-byte budget that SCALES WITH THE MODE'S
// airtime, so a test on a slow mode isn't falsely TIMED OUT while it's still
// legitimately draining. A payload byte costs ~ToA/MAXPAY of raw airtime;
// kSpeedTestToaFactor multiplies that for ARQ round-trips, acks, and the odd
// retransmit. Fast modes (tiny ToA) floor at kSpeedTestMsPerByteMin so a short
// test keeps a sane minimum budget. Without this scaling, far/SF12 (~9 s/frame)
// blew past the old fixed 4 ms/byte and reported a false TIMEOUT (got=0).
static const uint32_t kSpeedTestBaseMs       = 20000; ///< base test deadline
static const uint32_t kSpeedTestMsPerByteMin = 4;    ///< floor for fast modes
static const uint32_t kSpeedTestToaFactor    = 4;    ///< RTT/ack/retx headroom

// ---- AT+TRAIN secure pairing: X25519 ECDH over the air ----------------------
/**
 * @brief AT+TRAIN pairing magic: marks an over-the-air X25519 key-exchange
 *        frame. Both ends run AT+TRAIN (in the SAME mode); each broadcasts its
 *        PUBLIC key in the clear and derives the SAME link key = KDF(
 *        ECDH(my_private, peer_public) ). The secret never crosses the air. The
 *        returned fingerprint MUST match on both ends (the human MITM check);
 *        the key is stored in NVS, so you pair once. NOTE: *unauthenticated*
 *        ECDH — pair in physical proximity and compare the fingerprint
 *        (SECURITY.md).
 */
static const uint8_t kPairMagic[4] = { 0xAA, 0x55, 'T', 'R' };

// ---- AT+TRAIN pairing wire format + timing ----
// Pairing frame layout: [4-byte magic][32-byte X25519 public key] = 36 bytes.
static constexpr size_t   kPairMagicLen   = 4;     ///< bytes of kPairMagic
static constexpr size_t   kPairPubLen     = 32;    ///< X25519 public-key length
/// pairing frame length: magic + pubkey = 36 bytes
static constexpr size_t   kPairFrameLen   = kPairMagicLen + kPairPubLen;
/// total time (ms) to keep searching for a peer before giving up
static constexpr uint32_t kPairWindowMs   = 20000;
// Randomized per-cycle listen window = base + rand(0..jitter) ms. The random
// part drifts two symmetric ends out of lockstep so one listens while the other
// transmits (like CSMA backoff).
static constexpr uint32_t kPairListenBase = 250;   ///< listen-window base (ms)
static constexpr uint32_t kPairListenJit  = 750;   ///< listen-window jitter(ms)
// After hearing the peer, re-send my key this many times (each with a short
// confirm-listen window) so the still-listening peer surely gets mine too.
static constexpr int      kPairResends    = 5;     ///< post-hear key re-sends
static constexpr uint32_t kPairConfirmMs  = 250;   ///< confirm-listen win (ms)

void Host::Poll() {
    g_diag.Pet();               // loop is alive (covers long radio waits too)
    CheckHostReconnect();
    HostTxDrain();              // keep host output flowing first (non-blocking)
    AtTick();                   // +++ escape -> AT command-mode handling
    if (ServiceSpeedTest()) return;   // a speed test owns the link this pass
    // Bulk-drain ALL waiting USB bytes into the big PSRAM ingest ring so the
    // core's small queue never overflows; then feed the link from the ring at
    // link rate.
    while (Serial.available() && IngestFree() > kIngestLowFree) {
        feedLoopWDT();   // a host flood can keep this reading a while; stay fed
        uint8_t raw[kBulkReadBytes];
        size_t want = Serial.available();
        if (want > sizeof(raw)) want = sizeof(raw);
        // bulk read (avoids per-byte lock contention)
        int rd = Serial.read(raw, want);
        if (rd <= 0) break;
        uint8_t filt[sizeof(raw) + kEscapePlusCount];
        size_t fn = AtFilter(raw, (size_t)rd, filt); // strip +++/AT bytes
        size_t took = IngestPush(filt, fn);
        // ring full (stream > ring) -> counted, not silent
        if (took < fn) ingest_drop_ += (fn - took);
    }
    IngestToLink();
}

size_t Host::PumpLinkOut() {
    uint8_t b[kPumpChunkBytes]; size_t total = 0;
    if (sink_) {                  // speed-test sink: drain + discard (keep ACK)
        size_t n;
        while ((n = g_link.Read(b, sizeof(b))) > 0) {
            total += n; host_out_ += n;
        }
        return total;
    }
    while (true) {
        size_t room = HtFree();
        // leave headroom; ring full -> recv backpressure
        if (room <= kHostRingHeadroom) break;
        size_t want = room - kPumpReadMargin;   // small ring-headroom margin
        if (want > sizeof(b)) want = sizeof(b);
        size_t n = g_link.Read(b, want);
        if (n == 0) break;
        HostDeliver(b, (uint16_t)n);
        total += n; host_out_ += n;
    }
    HostTxDrain();
    return total;
}

void Host::MaybeStatus() {
    static uint32_t t = 0;
    if (millis() - t < kStatusPeriodMs) return;
    t = millis();
    AdjustTxPower();
}

void Host::ApplyLinkConfig() {
    link_layer::Config lc;
    lc.addr = cfg.addr;
    lc.peer = cfg.peer;
    // BDP window, auto-sized per mode in Radio::ApplyMode()
    lc.window = g_radio.window();
    // derived per-mode from ToA (Radio::ApplyMode)
    lc.retransmit_ms = g_radio.retransmit_ms();
    lc.compress = (cfg.feat & FEAT_COMP) != 0;
    lc.encrypt  = (cfg.feat & FEAT_ENC) != 0;
    // paired key (from NVS via AT+TRAIN) or the built-in fallback
    lc.key = g_link_key;
    lc.tag_len = LINK_TAGLEN;
    // Resume the monotonic frame counter past the persisted floor, then
    // re-persist the new floor immediately so a crash before the next save
    // still can't reuse it.
    uint64_t saved_ctr = prefs.getULong64("txctr", 0);
    lc.start_ctr = saved_ctr + LINK_CTR_STRIDE;
    prefs.putULong64("txctr", lc.start_ctr);
    lc.persist_stride = LINK_CTR_STRIDE;
    lc.persist_cb = Host::PersistTxCtr;
    // new boot id -> peer auto-resyncs on reconnect
    lc.epoch = (uint8_t)(esp_random() & 0xFF);
    if (!lc.epoch) lc.epoch = 1;
    g_link.Init(lc);
}

void Host::ApplyLinkTiming() {
    // Per-mode timing only — no link reset. The window / retransmit_ms were
    // re-derived from the new mode's time-on-air in DeriveTiming().
    g_link.SetTiming(g_radio.window(), g_radio.retransmit_ms());
}

void Host::LoadSettings() {
    prefs.begin("loramodem", false);
    cfg.addr = prefs.getUChar("addr", NODE_ADDR);
    cfg.peer = prefs.getUChar("peer", PEER_ADDR);
    cfg.feat = prefs.getUChar("feat", FEAT_DEFAULT);
    String nm = prefs.isKey("name") ? prefs.getString("name", NODE_NAME)
                                    : String(NODE_NAME);
    memset(cfg.name, 0, sizeof(cfg.name));
    strncpy(cfg.name, nm.c_str(), sizeof(cfg.name) - 1);
    cfg.sf     = prefs.getUChar("sf",  CFG_SF);    // radio mode (range/speed)
    cfg.bw_code = prefs.getUChar("bw",  CFG_BWCODE);
    cfg.cr     = prefs.getUChar("cr",  CFG_CR);
    // carrier frequency (MHz)
    cfg.freq_mhz = prefs.getFloat("freq", kFreqMhz);
    // private-link sync word (both ends must match)
    cfg.sync    = prefs.getUChar("sync", kSyncWord);
    // Static link key: the AT+TRAIN-derived per-pair key if we've paired, else
    // the built-in fallback. NVS survives reflash, so you pair once. The active
    // key (g_link_key) starts as a copy; the session handshake later replaces
    // it with a forward-secret per-session key (see fw_session).
    if (prefs.isKey("pkey") && prefs.getBytesLength("pkey") == 16)
        prefs.getBytes("pkey", g_static_key, 16);
    else
        memcpy(g_static_key, kLinkKey, 16);
    memcpy(g_link_key, g_static_key, 16);
}

void Host::SaveSettings() {
    prefs.putUChar("addr", cfg.addr);
    prefs.putUChar("peer", cfg.peer);
    prefs.putUChar("feat", cfg.feat);
    prefs.putString("name", cfg.name);
    prefs.putUChar("sf", cfg.sf);
    prefs.putUChar("bw", cfg.bw_code);
    prefs.putUChar("cr", cfg.cr);
    prefs.putFloat("freq", cfg.freq_mhz);
    prefs.putUChar("sync", cfg.sync);
}

// Paired = a per-pair key is persisted in NVS. prefs is opened in
// LoadSettings() (called early in setup()), so "pkey" is queryable here.
bool Host::IsPaired() {
    return prefs.isKey("pkey");
}

// Run the X25519 pairing exchange now and persist the derived key. The same
// handshake AT+TRAIN runs, driven directly so setup()/AT+PAIR can call it. On
// success it persists the per-pair key, turns encryption on, and adopts it.
bool Host::RunPairing(char* fp /* >= 5 bytes */) {
    // Disable the radio-wait host-I/O hook so the blocking handshake below
    // doesn't re-enter Poll()/the AT parser while we drive the radio
    // directly.
    void (*saved)() = g_rx_idle_hook; g_rx_idle_hook = nullptr;

    uint8_t priv[32], pub[32], peer[32];
    for (int i = 0; i < 32; i++) priv[i] = (uint8_t)(esp_random() & 0xFF);
    x25519::scalarmult_base(pub, priv);

    uint8_t frame[kPairFrameLen];
    memcpy(frame, kPairMagic, kPairMagicLen);
    memcpy(frame + kPairMagicLen, pub, kPairPubLen);

    uint8_t rx[64]; size_t rl; bool got = false;
    uint32_t start = millis();
    while (millis() - start < kPairWindowMs) {    // search for a peer
        g_radio.Tx(frame, sizeof frame);      // broadcast my public key (raw)
        // RANDOMIZED listen window: both ends are symmetric and may start in
        // lockstep (transmitting together, then listening together -> they'd
        // never hear each other). A random window per cycle drifts their phases
        // so one is listening while the other transmits. (Same idea as CSMA
        // backoff.)
        uint32_t win = kPairListenBase + (esp_random() % kPairListenJit);
        if (g_radio.Rx(rx, sizeof rx, rl, win) == RADIOLIB_ERR_NONE
            && rl == sizeof frame
            && memcmp(rx, kPairMagic, kPairMagicLen) == 0) {
            memcpy(peer, rx + kPairMagicLen, kPairPubLen);
            got = true;
            // Re-send a few times so the still-listening peer surely gets mine
            // too.
            for (int k = 0; k < kPairResends; k++) {
                g_radio.Tx(frame, sizeof frame);
                g_radio.Rx(rx, sizeof rx, rl, kPairConfirmMs);
            }
            break;
        }
    }
    g_rx_idle_hook = saved;                  // restore host-I/O servicing
    if (!got) return false;

    uint8_t shared[32], key[16];
    x25519::scalarmult(shared, priv, peer);        // ECDH -> shared secret
    link_layer::AsconKdf16(shared, 32, key);     // KDF -> 16-byte static key
    prefs.putBytes("pkey", key, 16);               // persist (survives reflash)
    memcpy(g_static_key, key, 16);                 // new long-term key
    cfg.feat |= FEAT_ENC;                       // pairing implies encryption on
    ApplyLinkConfig();                             // adopt the new key now
    SessionReset();      // re-handshake a session key from the new static key
    snprintf(fp, 5, "%02X%02X", key[0], key[1]);   // fingerprint: compare ends
    return true;
}

// Persist the elected role so a paired board reuses it (no re-discovery).
void Host::PersistRole() {
    prefs.putUChar("addr", cfg.addr);
    prefs.putUChar("peer", cfg.peer);
}

// Best-effort status line to the host serial (non-blocking; drops if the ring
// is full). Used to surface PAIRING status during the blocking proximity
// exchange, where the AT parser isn't the active path.
void Host::Emit(const char* s) { AtReply(s); }

void Host::IngestInit() {
    const size_t WANT = kIngestWantBytes;     // 2 MB if PSRAM is present
    ingest_ = (uint8_t*)ps_malloc(WANT);
    if (ingest_) { ingest_cap_ = WANT; return; }
    ingest_cap_ = kIngestFallBytes;            // fallback: internal RAM
    ingest_ = (uint8_t*)malloc(ingest_cap_);
    if (!ingest_) ingest_cap_ = 0;
}

// Radio-wait idle-hook thunk: forwards to the singleton so host I/O is serviced
// during blocking radio waits. Static to match g_rx_idle_hook's void(*)()
// signature; wired in setup() as `g_rx_idle_hook = Host::IdleHook;`.
void Host::IdleHook() { g_host.Poll(); }

// AEAD frame-counter persistence: the link layer calls this every
// persist_stride frames so a reboot can resume past the last-saved counter (no
// nonce reuse). Static so it matches the link's void(*)(void*,uint64_t)
// callback signature; it only touches the prefs global, so it needs no
// singleton state.
void Host::PersistTxCtr(void* /*arg*/, uint64_t ctr) {
    prefs.putULong64("txctr", ctr);
}

size_t Host::HtCount() {
    return (ht_head_ - ht_tail_ + sizeof(host_tx_)) % sizeof(host_tx_);
}
size_t Host::HtFree() { return sizeof(host_tx_) - 1 - HtCount(); }
void   Host::HtPush(const uint8_t* b, size_t n) {  // caller: HtFree() >= n
    for (size_t i = 0; i < n; i++) {
        host_tx_[ht_head_] = b[i];
        ht_head_ = (ht_head_ + 1) % sizeof(host_tx_);
    }
}
void Host::HostTxDrain() {
    int room = HOSTPORT.availableForWrite();
    while (room > 0 && HtCount() > 0) {
        size_t contig = (ht_head_ >= ht_tail_) ? (ht_head_ - ht_tail_)
                                          : (sizeof(host_tx_) - ht_tail_);
        size_t chunk  = contig < (size_t)room ? contig : (size_t)room;
        // bounded by room -> won't block
        size_t w = HOSTPORT.write(host_tx_ + ht_tail_, chunk);
        ht_tail_ = (ht_tail_ + w) % sizeof(host_tx_);
        if (w < chunk) break;
        room -= (int)w;
    }
}

// Discard any buffered host-bound output when a host (re)connects, so a
// reconnect never gets the previous session's stale backlog dumped at it.
// TinyUSB reports connection via (bool)Serial (DTR asserted = a terminal
// opened the port).
void Host::CheckHostReconnect() {
    static bool was_conn = false;
    bool conn = (bool)Serial;
    if (conn && !was_conn) {         // host just (re)connected -> fresh session
        ht_head_ = ht_tail_ = 0;         // drop stale host-bound output
        g_link.NewSession();        // reset link + bump epoch (peer resyncs)
        SessionReset();             // negotiate a fresh per-session key too
    }
    was_conn = conn;
}

// ---- Large host->link INGEST ring (PSRAM-backed) ----------------------------
// The ESP32 Arduino USB-CDC core drains the USB FIFO into a small FreeRTOS
// queue and SILENTLY DROPS when that queue is full (no USB NAK — a known
// arduino-esp32 limitation, issues #10836/#5727). A fast `cat bigfile` (USB
// runs ~1 MB/s, the "115200 baud" is cosmetic) overruns the ~2 KB/s LoRa link
// and bytes vanish.
// Fix: each poll, BULK-read everything waiting (bulk reads dodge the per-byte
// lock contention) into a big PSRAM ring, then feed the link from it at link
// rate. With a multi-MB ring every realistic transfer fits -> lossless, no
// client-side flow control. Only a stream LARGER than the ring can still drop
// (counted in ingest_drop_).
size_t Host::IngestCount() {
    return (in_head_ - in_tail_ + ingest_cap_) % ingest_cap_;
}
size_t Host::IngestFree() {
    return ingest_cap_ ? ingest_cap_ - 1 - IngestCount() : 0;
}
// Push up to n bytes; returns accepted count (rest = overrun beyond the ring).
size_t Host::IngestPush(const uint8_t* s, size_t n) {
    size_t fr = IngestFree(); if (n > fr) n = fr;
    size_t acc = 0;
    while (acc < n) {
        size_t contig = ingest_cap_ - in_head_;
        size_t c = (n - acc) < contig ? (n - acc) : contig;
        memcpy(ingest_ + in_head_, s + acc, c);
        in_head_ = (in_head_ + c) % ingest_cap_; acc += c;
    }
    return acc;
}
// Feed the link from the ingest ring at whatever rate it accepts
// (backpressure-safe).
void Host::IngestToLink() {
    while (IngestCount() > 0) {
        feedLoopWDT();   // draining a large backlog to the link; stay fed
        size_t contig = (in_head_ >= in_tail_) ? (in_head_ - in_tail_)
                                             : (ingest_cap_ - in_tail_);
        size_t w = g_link.Write(ingest_ + in_tail_, contig);
        in_tail_ = (in_tail_ + w) % ingest_cap_; host_in_ += w;
        if (w < contig) break;            // link ring full -> stop (buffered)
    }
}

void Host::AtReply(const char* s) {
    while (*s) {
        if (HtFree() < 2) break;
        uint8_t c = (uint8_t)*s++; HtPush(&c, 1);
    }
}

void Host::StartSpeedTest(uint32_t bytes) {
    if (!g_device.initiator()) {        // only the sender drives a test
        AtReply("speedtest: run this on the INITIATOR\r\nOK\r\n");
        return;
    }
    st_total_ = bytes; st_sent_ = 0; st_start_ = millis();
    // Per-byte budget tracks the current mode's airtime (see the constants).
    uint32_t ms_per_byte =
        (g_radio.toa_ms() * kSpeedTestToaFactor) / link_layer::MAXPAY;
    if (ms_per_byte < kSpeedTestMsPerByteMin)
        ms_per_byte = kSpeedTestMsPerByteMin;
    st_deadline_ = st_start_ + kSpeedTestBaseMs + bytes * ms_per_byte;
    st_tx0_ = g_link.DbgStatTx(); st_retx0_ = g_link.DbgStatRetx();
    st_gen_.Seed(st_start_);     // vary per run (data is discarded anyway)
    AtReply("speedtest: running (set AT+SINK=1 on the peer)...\r\n");
}

// Serviced from Poll() each pass. Feeds the link at link rate and, when the
// whole payload is delivered+ACKed, reports the result. Returns true while
// a test is active so Poll() can skip normal host ingest (the test owns the
// link). Does NOT drive the radio — the usual turn engine transmits the data.
bool Host::ServiceSpeedTest() {
    if (!st_total_) return false;
    while (st_sent_ < st_total_) {             // feed until the ring backs up
        uint8_t buf[256];
        size_t want = st_total_ - st_sent_;
        if (want > sizeof(buf)) want = sizeof(buf);
        for (size_t i = 0; i < want; i++)      // incompressible -> honest floor
            buf[i] = st_gen_.NextByte();
        size_t w = g_link.Write(buf, want);
        st_sent_ += w;
        if (w < want) break;                   // link ingest full; resume later
    }
    if (st_sent_ >= st_total_ && !g_link.HasWork()) {     // delivered + ACKed
        uint32_t ms = millis() - st_start_; if (!ms) ms = 1;
        uint32_t txd = g_link.DbgStatTx() - st_tx0_;
        uint32_t rtx = g_link.DbgStatRetx() - st_retx0_;
        int retx_pct = txd ? (int)(100 * rtx / txd) : 0;
        double kbps = (double)st_total_ / ((double)ms / 1000.0) / 1024.0;
        char s[160];
        snprintf(s, sizeof s,
                 "speedtest: %luB in %lums = %.2f KB/s | retx=%d%% "
                 "snr=%.1f rssi=%.0f pwr=%d\r\nOK\r\n",
                 (unsigned long)st_total_, (unsigned long)ms, kbps, retx_pct,
                 g_radio.snr(), (double)g_radio.rssi(), g_radio.tx_power());
        AtReply(s);
        st_total_ = 0;
        return false;
    }
    if (millis() > st_deadline_) {             // link not draining -> give up
        AtReply("speedtest: TIMEOUT (link not draining)\r\nOK\r\n");
        st_total_ = 0;
        return false;
    }
    return true;
}

void Host::AtExec(char* line) {
    // upper-case cmd head
    for (char* p = line; *p && *p != '='; p++)
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    int v; float fv; char s[kAtScratchMax];
    // AT — bare attention command; the modem liveness ping. Replies "OK". No
    // config or on-air effect.
    if      (!strcmp(line, "AT"))       AtReply("OK\r\n");
    // ATI — identify: print this node's name, address, peer, role (initiator),
    // and the encryption/tag/compression feature state. Read-only.
    else if (!strcmp(line, "ATI")) {
        snprintf(s, sizeof s,
                 "id=%s addr=%d peer=%d initiator=%d enc=%d tag=%d comp=%d "
                 "state=%s fw=%s\r\nOK\r\n",
                 cfg.name, cfg.addr, cfg.peer, g_device.initiator(),
                 (cfg.feat & FEAT_ENC) ? 1 : 0, LINK_TAGLEN,
                 (cfg.feat & FEAT_COMP) ? 1 : 0,
                 g_device.pairing() ? "pairing" : "ready", FW_VERSION);
        AtReply(s);
    }
    // AT+VER / AT+VER? — report the firmware version. It is stamped in at build
    // time from the git tag (tools/version.py), so it matches the release the
    // board was flashed from. Read-only.
    else if (!strcmp(line, "AT+VER") || !strcmp(line, "AT+VER?")) {
        snprintf(s, sizeof s, "fw=%s\r\nOK\r\n", FW_VERSION);
        AtReply(s);
    }
    // AT+LINK? — live link diagnostics: smoothed RSSI/SNR, current TX power, TX
    // queue depth, host bytes in/out (per-hop loss check), ingest-ring size and
    // drop count. Read-only.
    else if (!strcmp(line, "AT+LINK?")) {
        snprintf(s, sizeof s,
                 "rssi=%.0f snr=%.1f pwr=%d txq=%u hin=%lu hout=%lu ibuf=%uK "
                 "idrop=%lu tx=%lu retx=%lu reinit=%lu rendz=%lu heap=%luK\r\n"
                 "OK\r\n",
                 (double)g_radio.rssi(), g_radio.snr(), g_radio.tx_power(),
                 (unsigned)g_link.TxPending(), (unsigned long)host_in_,
                 (unsigned long)host_out_, (unsigned)(ingest_cap_/1024),
                 (unsigned long)ingest_drop_,
                 (unsigned long)g_link.DbgStatTx(),
                 (unsigned long)g_link.DbgStatRetx(),
                 (unsigned long)g_device.reinit_count(),
                 (unsigned long)g_device.rendezvous_count(),
                 (unsigned long)(
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        AtReply(s);
    }
    // AT+FMODE=<name> — FORCE this mode locally (no handshake, no peer
    // coordination). Disables ADR. Use to (a) test a PHY in isolation by
    // forcing BOTH ends to the same mode, and (b) RECOVER a mismatched pair
    // (set both the same). The normal AT+MODE coordinates the peer; this won't.
    else if (!strncmp(line, "AT+FMODE=", 9)) {
        if (g_radio.ApplyModeByName(line + 9)) {
            cfg.feat &= ~FEAT_ADR;
            g_modesw.Begin(g_radio.CurrentModeIndex());
            AtReply("OK (forced local; set the SAME on the other end)\r\n");
        } else {
            AtReply("ERROR (modes: turbo fast medium slow far ludicrous)\r\n");
        }
    }
    // AT+SESSION? — forward-secrecy status: whether a per-session key is in use
    // and a 2-byte fingerprint of both the static and the active key (they
    // DIFFER once a session handshake has completed). Read-only.
    else if (!strcmp(line, "AT+SESSION?")) {
        snprintf(s, sizeof s,
                 "session=%d static=%02X%02X active=%02X%02X\r\nOK\r\n",
                 SessionActive() ? 1 : 0, g_static_key[0], g_static_key[1],
                 g_link_key[0], g_link_key[1]);
        AtReply(s);
    }
    // AT+TXGAP=<ms> / AT+TXGAP? — experimental inter-frame TX pacing, to probe
    // whether a mode's loss is receiver re-arm timing (gap helps) or deeper.
    else if (!strncmp(line, "AT+TXGAP=", 9)) {
        g_device.SetTxGap((uint32_t)atoi(line + 9));
        snprintf(s, sizeof s, "txgap=%lu ms\r\nOK\r\n",
                 (unsigned long)g_device.tx_gap());
        AtReply(s);
    }
    else if (!strcmp(line, "AT+TXGAP?")) {
        snprintf(s, sizeof s, "txgap=%lu ms\r\nOK\r\n",
                 (unsigned long)g_device.tx_gap());
        AtReply(s);
    }
    // AT+DIAG — crash & health report: boot count, why it last reset (panic /
    // brownout / watchdog / clean), uptime reached before that reset, current
    // uptime, free + min-ever heap, and whether a core dump exists. Read-only.
    else if (!strcmp(line, "AT+DIAG")) {
        char d[192];
        g_diag.Report(d, sizeof d);
        AtReply(d); AtReply("OK\r\n");
    }
    // AT+CRASH=<panic|hang> — DELIBERATELY crash THIS board, to verify the
    // diagnostics actually capture a fault. Recoverable (NVS survives):
    //   panic -> illegal write -> ESP panic: writes a flash core dump, reboots;
    //            afterwards AT+DIAG shows lastreset=PANIC + coredump=YES.
    //   hang  -> stop returning to loop() so the watchdog stops being petted;
    //            ~25 s later the software watchdog reboots -> lastreset=SW-WDT.
    else if (!strncmp(line, "AT+CRASH", 8)) {
        const char* arg = (line[8] == '=') ? line + 9 : line + 8;
        AtReply("crashing now (reconnect after reboot, then AT+DIAG)\r\n");
        HostTxDrain();                       // flush the notice first
        delay(50);
        if (!strcmp(arg, "hang")) {
            for (;;) { delay(1); }           // never returns -> SW watchdog
        } else {
            volatile uint32_t* p = (volatile uint32_t*)0;
            *p = 0xDEAD;                      // illegal write -> panic + dump
        }
    }
    // AT+ADDR=n — set this node's link address. Recomputes the initiator role
    // (lower address initiates turns) and reinitializes the link immediately.
    // Not persisted until AT&W; both ends must have distinct addresses.
    else if (sscanf(line, "AT+ADDR=%d", &v) == 1) {
        cfg.addr = (uint8_t)v; ApplyLinkConfig();
        g_device.RecomputeRole();
        AtReply("OK\r\n");
    }
    // AT+PEER=n — set the expected peer address (0 = accept any). Recomputes
    // the initiator role and reinitializes the link. Not persisted until AT&W.
    else if (sscanf(line, "AT+PEER=%d", &v) == 1) {
        cfg.peer = (uint8_t)v; ApplyLinkConfig();
        g_device.RecomputeRole();
        AtReply("OK\r\n");
    }
    // AT+PWR=dBm — force the radio TX power now (effective on the next frame).
    // Auto power control (AdjustTxPower) may move it again afterward. Not saved
    // to NVS. VERIFY the value is legal for your band before field use.
    else if (sscanf(line, "AT+PWR=%d",  &v) == 1) {
        g_radio.SetTxPower((int8_t)v);
        AtReply("OK\r\n");
    }
    // AT+MODE? — show the current range/speed mode, whether 'auto' (ADR) is on,
    // and list the presets. GFSK ('ludicrous') has no SF/BW/CR. Read-only.
    else if (!strcmp(line, "AT+MODE?")) {
        const char* adr = !(cfg.feat & FEAT_ADR) ? ""
                          : (cfg.feat & FEAT_GFSK) ? " (auto+gfsk)" : " (auto)";
        if (g_radio.phy_fsk())
            snprintf(s, sizeof s,
                     "mode=%s%s phy=GFSK br=%.0fkbps\r\n"
                     "modes: turbo fast medium slow far ludicrous | auto"
                     "\r\nOK\r\n",
                     g_radio.CurrentModeName(), adr, (double)kFskBitrate);
        else
            snprintf(s, sizeof s,
                     "mode=%s%s sf=%d bw=%.0f cr=4/%d\r\n"
                     "modes: turbo fast medium slow far ludicrous | auto"
                     "\r\nOK\r\n",
                     g_radio.CurrentModeName(), adr, cfg.sf,
                     (double)BwFromCode(cfg.bw_code), cfg.cr);
        AtReply(s);
    }
    // AT+MODE=auto — enable adaptive data rate: the INITIATOR measures link SNR
    // and coordinates both ends to the fastest mode the link can sustain (the
    // responder follows). Run it on the initiator (the lower address).
    else if (!strcmp(line, "AT+MODE=auto")) {
        cfg.feat |= FEAT_ADR;
        AtReply(g_device.initiator()
                    ? "OK auto (ADR on this initiator)\r\n"
                    : "OK auto set; ADR runs on the initiator\r\n");
    }
    // AT+MODE=<turbo|fast|medium|slow|far|ludicrous> — pin a fixed preset
    // (turns 'auto' off). Re-applies the radio, re-derives turn timing + window
    // immediately. 'ludicrous' switches to GFSK modulation (very short range).
    // BOTH ends must use the SAME mode. Not persisted until AT&W.
    else if (!strncmp(line, "AT+MODE=", 8)) {
        int idx = g_radio.ModeIndexByName(line + 8);
        if (idx < 0) {
            AtReply("ERROR (modes: turbo fast medium slow far ludicrous "
                    "| auto)\r\n");
        } else {
            cfg.feat &= ~FEAT_ADR;             // pinning a mode disables ADR
            if (g_device.initiator()) {
                // Coordinate BOTH ends through the make-before-break handshake
                // (with probation auto-revert if the new PHY goes silent), so
                // pinning a mode brings the responder along. Applying it only
                // locally would leave the responder on the old PHY and deafen
                // the link — the bug this replaces.
                if (idx == g_radio.CurrentModeIndex()) {
                    AtReply("OK (already there)\r\n");
                } else {
                    g_modesw.Request(idx);
                    AtReply("OK (coordinating peer)\r\n");
                }
            } else {
                // Responder is the follower; apply locally (manual resync).
                g_radio.ApplyModeByIndex(idx);
                g_modesw.Begin(g_radio.CurrentModeIndex());
                AtReply("OK (responder; set the mode on the initiator)\r\n");
            }
        }
    }
    // AT+ADRGFSK=0|1 (AT+ADRGFSK? to query) — opt 'auto' into the GFSK
    // 'ludicrous' rung. Off by default: when on, ADR may step turbo->ludicrous
    // on a very strong, low-loss link (and drops back on loss). Only meaningful
    // with auto on the initiator. Not persisted until AT&W.
    else if (!strcmp(line, "AT+ADRGFSK?")) {
        snprintf(s, sizeof s, "adrgfsk=%d\r\nOK\r\n",
                 (cfg.feat & FEAT_GFSK) ? 1 : 0);
        AtReply(s);
    }
    else if (sscanf(line, "AT+ADRGFSK=%d", &v) == 1) {
        if (v) cfg.feat |= FEAT_GFSK; else cfg.feat &= ~FEAT_GFSK;
        AtReply("OK\r\n");
    }
    // AT+FS=0|1 (AT+FS? to query) — forward secrecy: per-session ephemeral
    // X25519 handshake. Off by default (the static key already gives
    // authenticated, replay-safe encryption). BOTH ends must match. Not
    // persisted until AT&W.
    else if (!strcmp(line, "AT+FS?")) {
        snprintf(s, sizeof s, "fs=%d\r\nOK\r\n", (cfg.feat & FEAT_FS) ? 1 : 0);
        AtReply(s);
    }
    else if (sscanf(line, "AT+FS=%d", &v) == 1) {
        if (v) cfg.feat |= FEAT_FS; else cfg.feat &= ~FEAT_FS;
        SessionReset();   // (re)start or drop the session handshake
        AtReply("OK\r\n");
    }
    // AT+APWR=0|1 (AT+APWR? to query) — peer-SNR auto TX-power control: each
    // side holds its power so the SNR the peer reports for it stays above the
    // mode's demod floor (see FEAT_APWR / the sim). OFF by default pending
    // hardware validation; when off, power is fixed at AT+PWR. Not persisted
    // until AT&W.
    else if (!strcmp(line, "AT+APWR?")) {
        snprintf(s, sizeof s, "apwr=%d\r\nOK\r\n",
                 (cfg.feat & FEAT_APWR) ? 1 : 0);
        AtReply(s);
    }
    else if (sscanf(line, "AT+APWR=%d", &v) == 1) {
        if (v) cfg.feat |= FEAT_APWR; else cfg.feat &= ~FEAT_APWR;
        AtReply("OK\r\n");
    }
    // AT+SPEEDTEST=<kb> — on-device throughput test (run on the INITIATOR).
    // Generates <kb> KB internally, measures link drain rate; set AT+SINK=1 on
    // the peer so it absorbs the data. Reports KB/s + retx + snr/rssi/pwr.
    else if (sscanf(line, "AT+SPEEDTEST=%d", &v) == 1) {
        if (v < 1) v = 1; if (v > 4096) v = 4096;    // clamp 1..4096 KB (4 MB)
        StartSpeedTest((uint32_t)v * 1024);
    }
    // AT+SINK=0|1 — drain+discard received data (no host needed at this end).
    // Use on the peer during AT+SPEEDTEST. Not persisted.
    else if (!strcmp(line, "AT+SINK?")) {
        snprintf(s, sizeof s, "sink=%d\r\nOK\r\n", sink_ ? 1 : 0);
        AtReply(s);
    }
    else if (sscanf(line, "AT+SINK=%d", &v) == 1) {
        sink_ = (v != 0); AtReply("OK\r\n");
    }
    // AT+ENC=0|1 — toggle Ascon-128 AEAD (authenticated encryption + replay
    // protection). Reinitializes the link immediately; BOTH ends must match.
    // Not persisted until AT&W.
    else if (sscanf(line, "AT+ENC=%d",  &v) == 1) {
        if (v) cfg.feat |= FEAT_ENC; else cfg.feat &= ~FEAT_ENC;
        ApplyLinkConfig(); AtReply("OK\r\n");
    }
    // AT+COMP=0|1 — toggle per-frame compression. Reinitializes the link
    // immediately; BOTH ends must match. Not persisted until AT&W.
    else if (sscanf(line, "AT+COMP=%d", &v) == 1) {
        if (v) cfg.feat |= FEAT_COMP; else cfg.feat &= ~FEAT_COMP;
        ApplyLinkConfig(); AtReply("OK\r\n");
    }
    // AT+NAME=s — set this node's human-readable name (shown by ATI), truncated
    // to fit cfg.name. Cosmetic only, no on-air effect. Not saved until AT&W.
    else if (!strncmp(line, "AT+NAME=", 8)) {
        strncpy(cfg.name, line + 8, sizeof(cfg.name) - 1);
        cfg.name[sizeof(cfg.name) - 1] = 0; AtReply("OK\r\n");
    }
    // AT+FREQ? — show the current carrier frequency in MHz. Read-only.
    else if (!strcmp(line, "AT+FREQ?")) {
        snprintf(s, sizeof s, "freq=%.3f MHz\r\nOK\r\n", (double)cfg.freq_mhz);
        AtReply(s);
    }
    // AT+FREQ=mhz — set the carrier frequency now (applied to the radio
    // immediately). MUST be set identically on BOTH ends or they go deaf.
    // Rejected if outside the SX1262's tuning range (kFreqMin/MaxMhz). Not
    // persisted until AT&W. VERIFY the frequency is legal for your region.
    else if (sscanf(line, "AT+FREQ=%f", &fv) == 1) {
        if (fv < kFreqMinMhz || fv > kFreqMaxMhz) {
            snprintf(s, sizeof s,
                     "ERROR freq out of range (%.0f-%.0f MHz)\r\n",
                     (double)kFreqMinMhz, (double)kFreqMaxMhz);
            AtReply(s);
        } else {
            cfg.freq_mhz = fv; g_radio.SetFrequency(fv);
            AtReply("OK (set on BOTH ends)\r\n");
        }
    }
    // AT+SYNC? — show the current private-link sync word (hex). Read-only.
    else if (!strcmp(line, "AT+SYNC?")) {
        snprintf(s, sizeof s, "sync=0x%02X\r\nOK\r\n", cfg.sync);
        AtReply(s);
    }
    // AT+SYNC=0xNN — set the LoRa sync word now (applied at once). Acts as a
    // coarse network filter; MUST match on BOTH ends. Not persisted until AT&W.
    else if (sscanf(line, "AT+SYNC=%i", &v) == 1) {
        cfg.sync = (uint8_t)v; g_radio.SetSyncWord(cfg.sync);
        AtReply("OK (set on BOTH ends)\r\n");
    }
    // AT&W — write the current settings to NVS so they survive reboot/reflash.
    // Persists addr/peer/feat/name/mode/freq/sync (not the live TX power).
    else if (!strcmp(line, "AT&W")) {
        SaveSettings(); AtReply("OK\r\n");
    }
    // AT&F — factory reset: clear NVS, reload build defaults, then re-apply
    // radio mode / freq / sync / link config and recompute the role. Wipes a
    // paired key too (reverts to the built-in fallback key).
    else if (!strcmp(line, "AT&F")) {
        prefs.clear(); LoadSettings();
        g_radio.ApplyMode(cfg.sf, cfg.bw_code, cfg.cr);
        g_radio.SetFrequency(cfg.freq_mhz); g_radio.SetSyncWord(cfg.sync);
        ApplyLinkConfig();
        g_device.RecomputeRole();
        g_modesw.Begin(g_radio.CurrentModeIndex());    // resync switch engine
        AtReply("factory reset\r\nOK\r\n");
    }
    // AT+TRAIN — secure pairing over X25519 ECDH. BLOCKS up to ~20 s driving
    // the radio directly, exchanging public keys with the other end (run on
    // BOTH ends, same mode), derives a per-pair link key, stores it in NVS, and
    // turns encryption on. Prints a fingerprint that MUST match on both ends.
    else if (!strcmp(line, "AT+TRAIN")) {
        AtReply("pairing 20s - run AT+TRAIN on the OTHER end now...\r\n");
        HostTxDrain();        // flush the prompt before the blocking handshake
        char fp[5];
        if (RunPairing(fp)) {
            snprintf(s, sizeof s,
                     "paired key=%s (MUST match peer)\r\nOK\r\n", fp);
            AtReply(s);
        } else {
            AtReply("ERROR no peer (run AT+TRAIN on both, same mode)\r\n");
        }
    }
    // AT+PAIR — re-run PROXIMITY pairing on demand: drop to low power,
    // re-discover an ADJACENT peer, re-elect the role, train a fresh key, and
    // persist role+key. Run on BOTH ends with the boards close together.
    // BLOCKS while pairing. (AT&F also forces a re-pair on next boot by wiping
    // NVS; AT+PAIR keeps your other settings.)
    else if (!strcmp(line, "AT+PAIR")) {
        AtReply("proximity pairing - bring the boards close, "
                "AT+PAIR on BOTH...\r\n");
        HostTxDrain();        // flush the prompt before the blocking exchange
        if (g_device.ProximityPair())
            AtReply("paired (role+key persisted)\r\nOK\r\n");
        else                 AtReply("ERROR no adjacent peer\r\n");
    }
    // AT? / AT$ — print the built-in command help (the list of supported AT
    // commands). Read-only.
    else if (!strcmp(line, "AT?") || !strcmp(line, "AT$")) {
        AtReply("commands:\r\n ATI  AT+VER  AT+LINK?  AT+SESSION?  AT+DIAG  "
                "AT+CRASH=<panic|hang>\r\n AT+MODE? "
                "AT+MODE=<turbo|fast|medium|slow|far|ludicrous|auto>\r\n");
        AtReply(" AT+FMODE=<name>(force local, no peer)\r\n"
                " AT+ADDR=n AT+PEER=n AT+NAME=s\r\n AT+FREQ=mhz AT+FREQ? "
                "AT+SYNC=0xNN AT+SYNC?\r\n");
        AtReply(" AT+PWR=dBm AT+APWR=0|1 AT+ENC=0|1 AT+COMP=0|1 "
                "AT+ADRGFSK=0|1 AT+FS=0|1\r\n");
        AtReply(" AT+SPEEDTEST=<kb>(initiator) AT+SINK=0|1(peer) "
                "AT+TRAIN(pair) AT+PAIR(proximity)\r\n");
        AtReply(" AT&W(save) AT&F(factory-reset) ATO(exit)\r\nOK\r\n");
    }
    // ATO — exit AT command mode and return to transparent data pass-through.
    else if (!strcmp(line, "ATO")) {
        at_mode_ = false; AtReply("OK\r\n");
    }
    // Unrecognized command.
    else                                AtReply("ERROR (AT? for list)\r\n");
}

// Strip the +++ escape / command-mode bytes out of the host->link stream.
// Returns the count of pass-through (data) bytes written to out[] (sized
// >= n + kEscapePlusCount). In AT mode, lines are accumulated/echoed and run
// via AtExec(); outside it, a guarded "+++" run is swallowed (the escape) and
// any non-escape '+' run is flushed back into the data stream.
size_t Host::AtFilter(const uint8_t* in, size_t n, uint8_t* out) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = in[i]; uint32_t now = millis();
        if (at_mode_) {
            at_active_at_ = now;                   // keep the idle timer alive
            if (b == '\r' || b == '\n') {
                if (at_n_) {
                    at_line_[at_n_] = 0; AtReply("\r\n");
                    AtExec(at_line_); at_n_ = 0;
                }
            }
            else if (at_n_ < sizeof(at_line_) - 1) {     // echo
                at_line_[at_n_++] = (char)b;
                uint8_t e = b; if (HtFree() > 2) HtPush(&e, 1);
            }
            continue;
        }
        // Guard time of host silence required before the escape run begins.
        if (b == '+'
            && (at_plus_ > 0 || now - at_last_data_ >= kEscapeGuardMs)) {
            if (at_plus_ < kEscapePlusCount) {
                at_plus_++; at_plus_at_ = now;
            }
            continue;                              // buffer '+', not data yet
        }
        if (at_plus_ > 0) {   // escape failed -> were data
            for (uint8_t j = 0; j < at_plus_; j++) out[k++] = '+';
            at_plus_ = 0;
        }
        out[k++] = b; at_last_data_ = now;
    }
    return k;
}

// Promote to AT command mode once a full "+++" run is followed by the trailing
// guard-time of silence (the Hayes escape's "+ 1s after" half). Also auto-exit
// AT mode after a long idle, so a forgotten/abandoned session returns the board
// to its transparent data pipe on its own.
void Host::AtTick() {
    if (!at_mode_ && at_plus_ >= kEscapePlusCount
        && millis() - at_plus_at_ >= kEscapeGuardMs) {
        at_plus_ = 0; at_mode_ = true; at_n_ = 0; at_active_at_ = millis();
        AtReply("\r\nOK\r\n");
    } else if (at_mode_ && millis() - at_active_at_ >= kAtIdleExitMs) {
        at_mode_ = false; at_n_ = 0;               // idle -> back to data mode
        AtReply("\r\n(AT idle timeout)\r\nOK\r\n");
    }
}

void Host::HostDeliver(const uint8_t* buf, uint16_t len) {
    // transparent byte stream (PumpLinkOut reserved room)
    HtPush(buf, len);
}

// Peer-SNR auto power control: hold OUR transmit power so the SNR the PEER
// reports for our signal (carried back in the link's authenticated aux byte)
// stays a safe margin above the current mode's demod floor — boost when the
// peer is marginal, ease off when it has headroom. Closing the loop on how the
// PEER hears us (not on our own received RSSI) is the fix for the
// asymmetric-link starvation bug behind FEAT_APWR: the old own-RSSI loop
// floored one side's power and deafened the other on a path with unequal loss
// each way (reproduced in test/test_sim test_autopower_own_rssi_starves_asym).
// The control law lives in lib/linklayer/autopower.h, shared verbatim with the
// sim (one source of truth).
void Host::AdjustTxPower() {
    // Gated on FEAT_APWR (ON by default; AT+APWR toggles). With it off, power
    // stays fixed at g_radio.tx_power() (AT+PWR sets it).
    if (!(cfg.feat & FEAT_APWR)) return;
    // (Auto-power once fought ADR — it floored power for a slow mode, then ADR
    // climbed to a power-hungrier one and the link went deaf on the switch, so
    // it was disabled under ADR. That "deaf" was largely the RX-throttle
    // regression (entry 29); auto-power now runs under ADR too and adapts power
    // as ADR adapts mode. GFSK still holds fixed power — see below.)
    // GFSK ('ludicrous') has no spreading gain, so it's far more sensitive to a
    // power change than LoRa — a step could push it below its demod margin (or,
    // at very close range, into saturation) and trip a loss storm. Leave GFSK's
    // power fixed; it's a short-range, max-speed mode.
    if (g_radio.phy_fsk()) return;
    // The peer's reported SNR for our signal (signed dB in the aux byte). It's
    // 0 until the peer's first report arrives, which makes the margin ~= -floor
    // (negative) -> the loop holds power, which is harmless.
    int peer_snr = (int8_t)g_link.AuxRx();
    // Which floor to hold our margin above: under ADR ('auto') aim a rung ahead
    // so the mode controller can climb (otherwise we minimize power for the
    // current mode and strand ADR a rung low); a pinned mode minimizes for
    // itself. The policy is one source of truth in autopower.h.
    int next_floor = 0;
    bool has_faster = g_radio.next_faster_snr_floor(next_floor);
    int floor = link_layer::AutoPowerTargetFloor(
        (cfg.feat & FEAT_ADR) != 0, g_radio.snr_floor(), next_floor,
        has_faster);
    int8_t p = link_layer::AutoPowerStep(g_radio.tx_power(), peer_snr,
                                         floor, kApPwrFloorDbm, kTxPowerDbm);
    if (p != g_radio.tx_power()) g_radio.SetTxPower(p);
}
