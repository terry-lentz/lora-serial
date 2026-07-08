/**
 * @file fw_host.h
 * @brief Host (USB) <-> link glue: the transparent data plane, the byte rings,
 *        AT command mode + pairing, link telemetry, and NVS settings.
 *
 * The host layer is a class (`Host`), instantiated ONCE as the static global
 * `g_host` (no heap in the hot path — see CLAUDE.md rule 5; the one allocation
 * is the PSRAM ingest ring, claimed once at boot via IngestInit()). It owns the
 * two byte rings (host-bound output + host->link ingest), the AT command-mode
 * state machine, the on-device speed-test state, and the host byte-flow
 * counters — all private members, reached through the public methods below.
 * Implementation in fw_host.cpp.
 */
#ifndef LORA_SERIAL_FW_HOST_H_
#define LORA_SERIAL_FW_HOST_H_

#include <stddef.h>
#include <stdint.h>

#include "byte_ring.h"  // util::ByteRing — the per-device outbound send queue
#include "fw_config.h"
#include "prng.h"   // link_layer::Lcg — deterministic speed-test PRNG (member)

// The host layer. ONE instance exists, the static global `g_host` (defined in
// fw_host.cpp) — never heap-allocated, so it is deterministic and
// fragmentation-free on a long-running radio (CLAUDE.md rule 5). It owns the
// host I/O rings, the AT command-mode state, the speed-test state, and the host
// byte-flow counters as private members, and exposes the host<->link service,
// settings, pairing, and telemetry helpers the rest of the firmware calls.
class Host {
 public:
  /**
   * @brief Service host<->link I/O for one pass.
   *
   * Called every loop iteration AND as the radio-wait idle hook
   * (g_rx_idle_hook, via the IdleHook() thunk), so USB keeps flowing during
   * multi-second radio ops: it pets the watchdog, drains host output, runs the
   * +++/AT command machine, services any speed test, and bulk-ingests waiting
   * USB bytes into the PSRAM ring, feeding the link at link rate.
   */
  void Poll();

  /**
   * @brief Drain decoded received bytes from the link to the host.
   *
   * Reads as much as the host-output ring has room for (so the slow terminal
   * can't block the radio loop) and hands it to the host; when the ring backs
   * up it stops, which becomes receive backpressure on the peer. In sink mode
   * (AT+SINK) it drains and discards instead, so no host is needed at that end.
   *
   * @return the number of bytes pulled from the link this pass.
   */
  size_t PumpLinkOut();

  /**
   * @brief Periodic housekeeping (auto TX-power control on a ~1.5 s cadence).
   *
   * Rate-limited internally; safe to call every turn. Only does work when the
   * auto-power feature (FEAT_APWR) is enabled.
   */
  void MaybeStatus();

  /**
   * @brief Push the current cfg/key/timing into the link layer (re-init it).
   *
   * RESETS the link (ARQ state, epoch, frame counter) and resumes the
   * persisted nonce counter past its saved floor. Use only at boot, on re-pair,
   * on address/encryption/compression changes, and on host reconnect.
   */
  void ApplyLinkConfig();

  /**
   * @brief Apply cfg.bufmode / cfg.bufkeep to the outbound send queue.
   *
   * Maps the persisted policy (BUFMODE_KEEPALL / BUFMODE_KEEPLATEST) and window
   * onto the ingest ring. Called from IngestInit() at boot and whenever
   * AT+BUFMODE / AT+BUFKEEP or the L1 CONFIG menu change the policy at runtime;
   * the change affects only future pushes, never bytes already queued.
   */
  void ApplyBufPolicy();

  /**
   * @brief Push ONLY the per-mode timing (BDP window + retransmit_ms) into the
   *        link, WITHOUT resetting it.
   *
   * Use for a runtime mode/PHY change so the switch doesn't bump the epoch
   * (which would re-run the session handshake and revert the switch) or drop
   * in-flight data.
   */
  void ApplyLinkTiming();

  /**
   * @brief Load the runtime settings (address, mode, freq, key, ...) from NVS.
   *
   * Opens the "loramodem" preferences namespace (so it must run early in
   * setup()), then fills cfg and the static/active keys from NVS, falling back
   * to the build-time defaults / built-in key when a value is absent.
   */
  void LoadSettings();

  /**
   * @brief Save the runtime settings (address, mode, freq, sync, ...) to NVS.
   *
   * Persists addr/peer/feat/name/mode/freq/sync/power so they survive reboot
   * and reflash.
   */
  void SaveSettings();

  /**
   * @brief Write a short one-way fingerprint of the paired static key.
   *
   * The AsconKdf16 of the static key, first 4 bytes as 8 hex chars — the same
   * on two units iff their keys match, without exposing the key. Backs
   * `AT+KEY?` and the INFO screen's key line.
   *
   * @param[out] out  destination buffer (>= 9 bytes for 8 hex + NUL).
   * @param[in]  n    capacity of out.
   */
  void KeyFingerprint(char* out, size_t n);

  /**
   * @brief Whether this board is already PAIRED.
   *
   * @return true if a per-pair key from AT+TRAIN/pairing is persisted in NVS
   *         ("pkey"). An unpaired board runs first-boot proximity pairing.
   */
  bool IsPaired();

  /**
   * @brief Run the X25519 pairing exchange now and persist the derived key.
   *
   * The same handshake as AT+TRAIN, driven directly so ProximityPair() can call
   * it from setup(). On success it derives a per-pair key over X25519 ECDH,
   * persists it to NVS, turns encryption on, and adopts the key live.
   *
   * @param[out] fp  buffer (>= 5 bytes) that receives a 4-hex fingerprint of
   *                 the derived key. MUST match on both ends (the human MITM
   *                 check). Must not be null.
   * @return false if no peer was heard within the pairing window; true on a
   *         completed exchange.
   */
  bool RunPairing(char* fp);

  /**
   * @brief Persist the elected role (addr/peer) to NVS.
   *
   * So a paired board reuses its role on later boots instead of re-discovering
   * it. Called by ProximityPair() after role election.
   */
  void PersistRole();

  /**
   * @brief Best-effort status line to the host serial (non-blocking).
   *
   * Queues the string in the host-output ring, dropping it if the ring is full
   * rather than blocking. Used to surface PAIRING status during the blocking
   * proximity exchange, where the AT parser isn't the active path.
   *
   * @param[in] s  the NUL-terminated string to emit. Must not be null.
   */
  void Emit(const char* s);

  /**
   * @brief Allocate the PSRAM host->link ingest ring.
   *
   * Claims a 2 MB PSRAM ring if PSRAM is present (so any realistic `cat
   * bigfile` fits losslessly), or a small internal-RAM ring as a fallback. Call
   * ONCE, before any host I/O. This is the layer's sole heap allocation, done
   * at boot and never freed (CLAUDE.md rule 5).
   */
  void IngestInit();

  /**
   * @brief Radio-wait idle-hook thunk for g_rx_idle_hook.
   *
   * Static so it matches the plain `void(*)()` signature g_rx_idle_hook
   * requires; it forwards to g_host.Poll() so the singleton services host I/O
   * during blocking radio waits. Wired in setup() as `g_rx_idle_hook =
   * Host::IdleHook;`.
   */
  static void IdleHook();

  /**
   * @brief Link TX-counter persist callback for link_layer::Config::persist_cb.
   *
   * The link layer calls this every persist_stride frames so a reboot can
   * resume the nonce counter past the last-saved value (no nonce reuse). Static
   * so it matches the `void(*)(void*, uint64_t)` callback signature; it only
   * touches the `prefs` global, so it needs no singleton state.
   *
   * @param[in] arg  unused callback context (the link passes nullptr).
   * @param[in] ctr  the frame counter value to persist.
   */
  static void PersistTxCtr(void* arg, uint64_t ctr);

  /**
   * @brief Running count of host->link bytes accepted, for AT+LINK? and the
   *        responder's interactive-piggyback heuristic (which watches this to
   *        tell a live interactive host from a one-way bulk transfer).
   * @return the host->link byte count so far this session.
   */
  uint32_t host_in() const { return host_in_; }
  /**
   * @brief Running count of link->host bytes delivered, for AT+LINK? (the
   *        symmetric partner of host_in(), pinpointing which hop loses bytes).
   * @return the link->host byte count so far this session.
   */
  uint32_t host_out() const { return host_out_; }

 private:
  // ---- Buffer sizes used as member-array bounds (declared first) ------------
  /**
   * @brief Host-bound output ring size (bytes): link->host bytes wait here
   *        until the slow terminal's TX FIFO has room. Sizes host_tx_.
   */
  static constexpr size_t kHostTxRingBytes = 2048;
  /**
   * @brief AT command-line scratch (chars, incl. NUL). Fits the longest AT
   *        command. Sizes at_line_.
   */
  static constexpr size_t kAtLineMax = 80;

  // ---- Non-blocking host output ring helpers --------------------------------
  /**
   * @brief Bytes currently queued in the host-output ring.
   * @return the queued byte count.
   */
  size_t HtCount();

  /**
   * @brief Free space in the host-output ring.
   * @return the free byte count (one slot is reserved as the full/empty
   *         discriminator).
   */
  size_t HtFree();

  /**
   * @brief Append bytes to the host-output ring. Caller must ensure room.
   * @param[in] b  source bytes. Must not be null.
   * @param[in] n  byte count; the caller must ensure HtFree() >= n.
   */
  void HtPush(const uint8_t* b, size_t n);

  /**
   * @brief Drain the host-output ring to the port without blocking.
   *
   * Writes only as much as availableForWrite() reports room for, so it never
   * blocks the radio loop on a slow terminal. When no terminal has the port
   * open (DTR deasserted) it DISCARDS the ring instead of holding it: buffered
   * output is dropped on the next reconnect anyway, and a full ring would
   * back-pressure PumpLinkOut and wedge the link — which on a display node also
   * freezes the OLED (its teletype is fed from this ring). The display still
   * gets every byte via the HtPush tap.
   */
  void HostTxDrain();

  /**
   * @brief On a host (re)connect, drop stale output and start a fresh session.
   *
   * So a reconnect never gets the previous session's host-bound backlog.
   */
  void CheckHostReconnect();

  // ---- Large host->link INGEST ring (the configurable send queue) -----------
  /**
   * @brief Feed the link from the ingest ring at whatever rate it accepts
   *        (backpressure-safe).
   */
  void IngestToLink();

  // ---- AT command mode + speed test -----------------------------------------
  /**
   * @brief Queue a NUL-terminated reply string into the host-output ring.
   *
   * Drops the reply on a full ring rather than blocking.
   *
   * @param[in] s  the NUL-terminated reply. Must not be null.
   */
  void AtReply(const char* s);

  /**
   * @brief Start an on-device throughput self-test (initiator only).
   * @param[in] bytes  total bytes to generate and push through the link.
   */
  void StartSpeedTest(uint32_t bytes);

  /**
   * @brief Service an in-progress speed test for one pass.
   * @return true while a test is active (so Poll() skips normal ingest — the
   *         test owns the link); false when idle or just finished.
   */
  bool ServiceSpeedTest();

  /**
   * @brief Execute one parsed AT command line (uppercases the command head).
   * @param[in,out] line  the NUL-terminated command line; the command head is
   *                      uppercased in place. Must not be null.
   */
  void AtExec(char* line);

  /**
   * @brief Strip the +++ escape / command-mode bytes from the host stream.
   * @param[in]  in   source bytes from the host. Must not be null.
   * @param[in]  n    byte count in `in`.
   * @param[out] out  pass-through (data) bytes; sized >= n + kEscapePlusCount.
   * @return the count of pass-through bytes written to out[].
   */
  size_t AtFilter(const uint8_t* in, size_t n, uint8_t* out);

  /**
   * @brief Promote into / auto-exit AT command mode.
   *
   * Driven by the +++ guard timing (to enter) and the AT-mode idle timeout (to
   * exit on its own).
   */
  void AtTick();

  // ---- Misc -----------------------------------------------------------------
  /**
   * @brief Hand decoded link bytes to the host-output ring.
   * @param[in] buf  the decoded bytes (caller reserved room). Must not be null.
   * @param[in] len  the byte count.
   */
  void HostDeliver(const uint8_t* buf, uint16_t len);

  /**
   * @brief Peer-SNR auto TX-power control (FEAT_APWR; ON by default).
   *
   * Holds our TX power so the SNR the peer reports for us (via the link's
   * authenticated aux byte) stays a margin above the mode's demod floor. A
   * no-op unless FEAT_APWR is set; AT+APWR toggles it at runtime.
   */
  void AdjustTxPower();

  // ---- Non-blocking host output ring ---------------------------------------
  // A slow terminal must NEVER block the radio loop: every host-bound byte goes
  // through this ring, drained only as fast as the port's TX FIFO has room.
  // When it backs up, PumpLinkOut() stops pulling from the link -> recv-ring
  // backpressure -> the peer holds + retransmits. No blocking, no drops.
  // (kHostTxRingBytes is declared at the top of the private section.)
  uint8_t host_tx_[kHostTxRingBytes];   ///< ring storage
  size_t  ht_head_ = 0;                  ///< producer index (HtPush)
  size_t  ht_tail_ = 0;                  ///< consumer index (HostTxDrain)

  // ---- Large host->link INGEST ring (PSRAM-backed) -------------------------
  // The ESP32 Arduino USB-CDC core drains the USB FIFO into a small FreeRTOS
  // queue and SILENTLY DROPS when that queue is full (arduino-esp32
  // #10836/#5727). A fast `cat bigfile` overruns the ~2 KB/s LoRa link and
  // bytes vanish. Fix: each poll, BULK-read everything waiting into this big
  // ring (PSRAM where present), then feed the link from it at link rate. With a
  // multi-MB ring every realistic transfer fits -> lossless. This is also the
  // configurable per-device send queue: its retention policy (AT+BUFMODE,
  // ApplyBufPolicy()) decides overflow behavior when the peer can't keep up —
  // byte-exact back-pressure (keepall) or drop-oldest (keeplatest). Storage is
  // claimed once in IngestInit() and never freed (CLAUDE.md rule 5).
  util::ByteRing ingest_ring_;    ///< the outbound send queue (storage below)
  uint32_t ingest_drop_ = 0;      ///< bytes dropped (overflow/evict); AT+LINK?

  // ---- AT command mode (Hayes "+++" escape) for the raw serial transport ---
  bool     at_mode_ = false;      ///< true while in AT command mode
  uint8_t  at_plus_ = 0;          ///< count of buffered '+' toward the escape
  uint32_t at_plus_at_ = 0;       ///< millis() of the last buffered '+'
  uint32_t at_last_data_ = 0;     ///< millis() of the last pass-through byte
  uint32_t at_active_at_ = 0;     ///< millis() of the last AT-mode keystroke
  char     at_line_[kAtLineMax];  ///< command-line accumulator (incl. NUL)
  uint8_t  at_n_ = 0;             ///< bytes accumulated in at_line_

  // ---- On-device link test: throughput self-test + sink --------------------
  // AT+SPEEDTEST=<kb> (initiator) generates incompressible data INTERNALLY and
  // measures how fast the link drains it; AT+SINK=1 makes the peer drain +
  // discard so no host is needed at the far end.
  bool     sink_ = false;         ///< drain+discard received data (peer)
  uint32_t st_total_ = 0;         ///< bytes this test (0 = inactive)
  uint32_t st_sent_ = 0;          ///< bytes handed to the link so far
  uint32_t st_start_ = 0;         ///< millis() at test start
  uint32_t st_deadline_ = 0;      ///< bail if the link won't drain
  uint32_t st_tx0_ = 0;           ///< TX frame-counter snapshot at start
  uint32_t st_retx0_ = 0;         ///< retx frame-counter snapshot at start
  /**
   * @brief Deterministic generator for the speed test's incompressible payload
   *        (see prng.h). Re-seeded per run; the data is discarded by the peer,
   *        so only its incompressibility matters, not the exact sequence.
   */
  link_layer::Lcg st_gen_{0};

  // ---- Host byte-flow counters (reported by AT+LINK?) -----------------------
  // Pinpoint which hop loses bytes during a transfer; also consulted by the
  // responder loop's interactive piggyback heuristic (see host_in()).
  uint32_t host_in_ = 0;          ///< host->link bytes accepted
  uint32_t host_out_ = 0;         ///< link->host bytes delivered
};

// The single host-layer instance (static singleton; no heap — CLAUDE.md
// rule 5). Defined in fw_host.cpp.
extern Host g_host;

#endif  // LORA_SERIAL_FW_HOST_H_
