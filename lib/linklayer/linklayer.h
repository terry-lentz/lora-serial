/**
 * @file linklayer.h
 * @brief Portable LoRa link layer (no Arduino/RadioLib deps).
 *
 * Compiles and runs both on the ESP32 and natively on a PC for unit testing.
 * It provides Selective-Repeat ARQ with a SACK bitmap (the receiver buffers
 * out-of-order frames and reports exactly which arrived, so the sender resends
 * ONLY the gaps; a window up to 16 keeps the pipe full over the
 * bandwidth-delay product, so no per-window stall), optional per-frame
 * compression (heatshrink) and Ascon-128 AEAD encryption, and addressing
 * (src/dst + peer filter). The transport (radio) is injected: the caller pumps
 * frames in via OnRx() and pulls frames to transmit via NextTx().
 * Turn-taking/cadence is the caller's job.
 *
 * Frame wire format:
 *   [0] src
 *   [1] dst
 *   [2] flags    bit0 F_DATA, bit1 F_COMP, bit2 F_ENC, bit3 F_MORE;
 *                bits4-7 = opaque firmware control nibble (mode-switch/ADR)
 *   [3] seq      ARQ data sequence (valid if F_DATA)
 *   [4] ack      cumulative ack: next seq expected from peer
 *   [5] epoch    boot id (changes on restart -> peer resyncs)
 *   [6] sackHi   SACK bitmap high byte: bit k set => seq (ack+1+k) buffered
 *   [7] sackLo   SACK bitmap low byte
 *   [8] len      payload length (plaintext == ciphertext length)
 *   [9] aux      opaque firmware telemetry byte (peer-SNR for auto-power);
 *                authenticated like the header, never interpreted by the link
 *   --- if F_ENC (AEAD): ---
 *   [10..17] ctr   64-bit monotonic frame counter (big-endian) -> nonce +
 *                  replay id
 *   [18..]   ciphertext (payload, compressed-then-encrypted), len bytes
 *   [end]    tag    AEAD auth tag (cfg.tag_len bytes; 8 or 16)
 *   The 10-byte header AND the 8 counter bytes are the AEAD associated data, so
 *   an attacker can't rewrite seq/ack/flags/aux/counter without failing the tag
 *   check.
 *   --- if not F_ENC: ---
 *   [10..]   payload (optionally compressed), len bytes
 */
#ifndef LINK_LAYER_LINKLAYER_H_
#define LINK_LAYER_LINKLAYER_H_
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ll_aead.h"        // Ascon-128 AEAD (portable)
#include "ll_transform.h"   // compress helper (portable)

namespace link_layer {

/**
 * @brief Raw payload bytes per frame.
 *
 * The LoRa PHY caps a packet at 255 bytes. With AEAD the worst case is
 * HDR(10)+CTRW(8)+MAXPAY+TAGMAX(16); MAXPAY=221 makes that exactly 255.
 */
static const size_t   MAXPAY   = 221;
static const uint8_t  F_DATA   = 0x01;  ///< Flag: frame carries ARQ data.
static const uint8_t  F_COMP   = 0x02;  ///< Flag: payload is compressed.
static const uint8_t  F_ENC    = 0x04;  ///< Flag: payload is AEAD-encrypted.
static const uint8_t  F_MORE   = 0x08;  ///< Flag: more frames follow this turn.
/**
 * @brief Mask of the 4 spare flag bits used as an opaque control channel.
 *
 * The four bits (0x10..0x80) are a 1-nibble control channel the firmware uses
 * for the mode-switch handshake / ADR. They ride in the header (so they're
 * authenticated as AEAD associated data) and the link layer never interprets
 * them — see SetCtrlTx()/CtrlRx(). Frames that don't set them are
 * byte-identical to before this feature.
 */
static const uint8_t  CTRL_MASK = 0xF0;
/// Header size: src,dst,flags,seq,ack,epoch,sackHi,sackLo,len,aux.
static const size_t   HDR      = 10;
static const size_t   CTRW     = 8;     ///< On-wire frame-counter width.
static const size_t   TAGMAX   = 16;    ///< Max AEAD tag bytes.
static const size_t   MAXFRAME = HDR + CTRW + MAXPAY + TAGMAX;  ///< Max == 255.
static const uint8_t  ADDR_BCAST = 0xFF;  ///< Broadcast destination address.
/// ARQ window cap == SACK bitmap bits; divides 256 (clean seq->slot mapping).
static const uint8_t  MAXWIN   = 16;
static const size_t   DEC_MAX  = MAXPAY + 64;  ///< Decoded-payload size bound.

/** @brief Per-link configuration: addressing, ARQ window, AEAD options. */
struct Config {
    uint8_t  addr = 1;            ///< This link's own address.
    uint8_t  peer = 0;           ///< Peer address to accept (0 = accept any).
    /// In-flight window (<= MAXWIN); size it to the bandwidth-delay product.
    uint8_t  window = 8;
    /// Resend an unacked frame after this many ms.
    uint32_t retransmit_ms = 1200;
    bool     compress = false;   ///< Compress payloads (heatshrink) if smaller.
    bool     encrypt = false;    ///< Seal frames with Ascon-128 AEAD.
    const uint8_t* key = nullptr;  ///< 16-byte Ascon-128 key (if encrypt).
    /// Boot id: changes on restart so the peer resyncs.
    uint8_t  epoch = 1;
    // --- AEAD (only used if encrypt) ---
    uint8_t  tag_len = 8;        ///< On-wire auth tag bytes: 8 (lean) or 16.
    /// Initial TX frame counter (loaded from NVS, pre-bumped past last use).
    uint64_t start_ctr = 0;
    /// Call persist_cb every N counters (trades flash wear vs reuse skip).
    uint32_t persist_stride = 256;
    void*    persist_arg = nullptr;  ///< Opaque arg passed to persist_cb.
    /// Save the TX counter (firmware writes it to NVS); null = no persist.
    void   (*persist_cb)(void*, uint64_t) = nullptr;
};

/** @brief Selective-Repeat ARQ link layer with optional AEAD + compression. */
template <size_t RING = 8192>
class LinkLayer {
public:
    /**
     * @brief (Re)initialize the link: reset all ARQ/replay/session state.
     *
     * Clamps the window and tag length, seeds the TX frame counter from
     * cfg.start_ctr, and clears the receive window. Call at boot, re-pair, or
     * host reconnect.
     *
     * @param[in] c the configuration to copy into this link.
     */
    void Init(const Config& c) {
        cfg_ = c;
        if (cfg_.window > MAXWIN) cfg_.window = MAXWIN;
        if (cfg_.window < 1) cfg_.window = 1;
        if (cfg_.tag_len == 0 || cfg_.tag_len > TAGMAX) cfg_.tag_len = 8;
        s_head_ = s_tail_ = r_head_ = r_tail_ = 0;
        inflight_ = 0; base_seq_ = next_seq_ = 0; rx_next_ = 0;
        ack_pending_ = false; ack_sent_turn_ = false; got_data_ = false;
        tx_ctr64_ = cfg_.start_ctr; last_persist_ = cfg_.start_ctr;
        rx_ctr_max_ = 0; rx_window_ = 0; rx_ctr_init_ = false; // anti-replay
        my_epoch_ = cfg_.epoch ? cfg_.epoch : 1; peer_epoch_ = 0;
        tx_ctrl_ = rx_ctrl_ = 0;
        tx_aux_ = rx_aux_ = 0;
        stat_tx_ = stat_retx_ = 0;
        memset(win_, 0, sizeof(win_));
        for (int i = 0; i < MAXWIN; i++) rx_[i].received = false;
    }

    /**
     * @brief Update ONLY the per-mode timing, live, without resetting state.
     *
     * Changes the BDP window and retransmit timer WITHOUT touching ARQ state,
     * the epoch, the frame counter, or the replay window. Use this for a
     * runtime PHY/mode change: a mode switch must NOT bump the epoch (that
     * makes the peer resync and re-run the session handshake, which can revert
     * the switch) and must NOT drop in-flight frames (they simply retransmit
     * on the new PHY). Only Init() resets the link.
     *
     * @param[in] window        in-flight window size (clamped to [1, MAXWIN]).
     * @param[in] retransmit_ms unacked-frame resend timeout in ms.
     */
    void SetTiming(uint8_t window, uint32_t retransmit_ms) {
        if (window < 1) window = 1;
        if (window > MAXWIN) window = MAXWIN;
        cfg_.window = window;
        cfg_.retransmit_ms = retransmit_ms;
    }

    // ---- app <-> link ----
    /**
     * @brief Queue app bytes for transmission (into the send ring).
     *
     * Copies as many bytes as the send ring has room for.
     *
     * @param[in] d source bytes from the application.
     * @param[in] n number of bytes offered.
     * @return how many bytes were actually queued (may be < n if full).
     */
    size_t Write(const uint8_t* d, size_t n) {
        size_t k = 0;
        while (k < n && SFree() > 0) SPush(d[k++]);
        return k;
    }

    /**
     * @brief Read delivered, in-order app bytes (from the receive ring).
     *
     * Freeing ring space here also drains any frames that backpressure was
     * holding.
     *
     * @param[out] out destination for delivered bytes.
     * @param[in]  max capacity of `out`.
     * @return how many bytes were copied out.
     */
    size_t Read(uint8_t* out, size_t max) {
        size_t k = 0;
        while (k < max && RCount() > 0) out[k++] = RPop();
        DrainInOrder();   // ring just freed -> deliver any frames held by
                          // backpressure
        return k;
    }

    /**
     * @brief Start a fresh session (call when the host (re)connects).
     *
     * Drops all queued / in-flight / received data and bumps our epoch so the
     * peer resyncs. Prevents a new session from inheriting the previous one's
     * stale bytes.
     */
    void NewSession() {
        s_head_ = s_tail_ = 0; r_head_ = r_tail_ = 0;
        inflight_ = 0; base_seq_ = next_seq_ = 0; rx_next_ = 0;
        ack_pending_ = false; ack_sent_turn_ = false; got_data_ = false;
        for (int i = 0; i < MAXWIN; i++) rx_[i].received = false;
        rx_ctrl_ = 0;                                    // drop stale control
        rx_aux_ = 0;                                     // drop stale telemetry
        my_epoch_++; if (my_epoch_ == 0) my_epoch_ = 1;  // epoch -> peer
                                                         // resyncs
    }

    /**
     * @brief Is there anything to send right now?
     * @return true if a frame is in flight, bytes are queued, or an ack is
     *         owed to the peer.
     */
    bool HasWork() const {
        return inflight_ > 0 || SCount() > 0 || ack_pending_;
    }

    /**
     * @brief Bytes still queued for transmission in the send ring.
     * @return the number of queued, not-yet-framed app bytes.
     */
    size_t TxPending() const { return SCount(); }

    /**
     * @brief Consume the "new in-order data was delivered" edge.
     *
     * Reads the edge and clears it, so it reports true once per delivery.
     *
     * @return true if in-order data was delivered since the last call.
     */
    bool TakeGotData() { bool g = got_data_; got_data_ = false; return g; }

    /**
     * @brief Consume the "peer rebooted" edge (clears it).
     *
     * True once after the peer's epoch changed (it rebooted / restarted its
     * session). The firmware uses this to re-run the session-key handshake, so
     * a reboot mid-session can't leave the ends on mismatched keys.
     *
     * @return true if the peer's epoch changed since the last call.
     */
    bool TakePeerReset() {
        bool r = peer_reset_; peer_reset_ = false; return r;
    }

    /**
     * @brief Consume the "a valid frame was received" edge (clears it).
     *
     * True once if a VALID (decoded, authenticated) frame arrived since the
     * last call — i.e. the peer is really reachable on this PHY. Used as the
     * "link alive" signal for dead-link rendezvous, so RF noise that
     * false-triggers the receiver (a strong nearby TX on a different PHY)
     * can't masquerade as a live link.
     *
     * @return true if an authenticated frame arrived since the last call.
     */
    bool TakeValidRx() {
        bool v = got_valid_rx_; got_valid_rx_ = false; return v;
    }

    // ---- Opaque control channel (mode-switch handshake / ADR) ----
    /**
     * @brief Set the control bits to stamp on every outgoing frame header.
     *
     * The 4 control bits (CTRL_MASK) are OR'd into EVERY outgoing frame until
     * changed; pass 0 to stop sending control. The link layer treats the
     * value as opaque — the firmware defines its meaning. Authenticated with
     * the header when encryption is on.
     *
     * @param[in] bits control nibble (masked to CTRL_MASK).
     */
    void SetCtrlTx(uint8_t bits) { tx_ctrl_ = (uint8_t)(bits & CTRL_MASK); }

    /**
     * @brief Control bits from the most recently accepted frame.
     *
     * The nibble is from the last VALID (authenticated, when encrypted) frame.
     *
     * @return the received control nibble (CTRL_MASK bits), or 0 if none.
     */
    uint8_t CtrlRx() const { return rx_ctrl_; }

    // ---- Opaque telemetry byte (firmware peer-SNR for auto-power) ----
    /**
     * @brief Stamp an opaque 1-byte telemetry value on every outgoing frame.
     *
     * The byte (e.g. a quantized peer-SNR for auto-power) rides in the
     * authenticated header at offset [9] on EVERY outgoing frame until changed.
     * The link layer never interprets it — the firmware defines its meaning.
     * Authenticated with the header when encryption is on.
     *
     * @param[in] aux the telemetry byte to stamp on TX frames.
     */
    void SetAuxTx(uint8_t aux) { tx_aux_ = aux; }

    /**
     * @brief Telemetry byte from the peer's most recently received frame.
     *
     * The byte is from the last VALID (authenticated, when encrypted) frame.
     *
     * @return the peer's aux byte (0 until one is received / after a reset).
     */
    uint8_t AuxRx() const { return rx_aux_; }

    /**
     * @brief Diagnostics: the current monotonic TX frame counter.
     * @return the 64-bit counter that seeds the AEAD nonce.
     */
    uint64_t DbgTxCtr() const { return tx_ctr64_; }

    /**
     * @brief Diagnostics: total data frames sent.
     * @return the count of data frames emitted (originals plus resends).
     */
    uint32_t DbgStatTx() const { return stat_tx_; }

    /**
     * @brief Diagnostics: how many sent data frames were resends.
     * @return the count of retransmitted data frames.
     */
    uint32_t DbgStatRetx() const { return stat_retx_; }

    /**
     * @brief Test hook: run a counter through the anti-replay window.
     *
     * Mutates the replay window exactly as a received frame would.
     *
     * @param[in] ctr the frame counter to test.
     * @return true if the counter is fresh (accepted); false if old/seen.
     */
    bool DbgReplayOk(uint64_t ctr) { return ReplayOk(ctr); }

    // ---- transport ----
    /**
     * @brief Begin a transmit turn: clear the per-turn "already sent" marks.
     *
     * Lets each in-flight frame be (re)transmitted at most once this turn and
     * resets the lone-ack guard.
     */
    void BeginTurn() {
        for (uint8_t i = 0; i < inflight_; i++) win_[i].tx_this_turn = false;
        ack_sent_turn_ = false;
    }

    /**
     * @brief Produce the next frame to transmit this turn.
     *
     * Fills the window with fresh data frames, then selective-repeat picks the
     * next un-acked frame not yet sent this turn that is new or timed out;
     * failing that, emits one lone ack if owed.
     *
     * @param[out] out     destination frame buffer.
     * @param[in]  cap     capacity of `out`.
     * @param[out] out_len bytes written to `out`.
     * @param[in]  now     current millis() (for retransmit timing).
     * @return true if a frame was produced; false when the turn is done.
     */
    bool NextTx(uint8_t* out, size_t cap, size_t& out_len, uint32_t now) {
        // Fill the window with fresh data frames.
        while (inflight_ < cfg_.window && SCount() > 0) {
            Frame& f = win_[inflight_];
            f.raw_len = 0;
            while (f.raw_len < MAXPAY && SCount() > 0)
                f.raw[f.raw_len++] = SPop();
            f.seq = next_seq_++;
            f.ever_sent = false; f.sent_at = 0; f.tx_this_turn = false;
            f.acked = false; f.fast_resend = false;
            inflight_++;
        }
        // Selective repeat: pick the next UN-ACKED frame not yet sent this
        // turn, that is new or has timed out.
        int pick = -1;
        for (uint8_t i = 0; i < inflight_; i++) {
            Frame& f = win_[i];
            if (f.acked || f.tx_this_turn) continue;
            if (!f.ever_sent || f.fast_resend ||
                (uint32_t)(now - f.sent_at) >= cfg_.retransmit_ms) {
                pick = i; break;
            }
        }
        if (pick >= 0) {
            Frame& f = win_[pick];
            if (f.ever_sent) stat_retx_++;   // resend (timed-out or fast)
            stat_tx_++;                       // data frames emitted
            f.tx_this_turn = true; f.ever_sent = true; f.sent_at = now;
            f.fast_resend = false;            // consumed
            bool more = MoreToSend(pick);
            out_len = Encode(out, cap, F_DATA | (more ? F_MORE : 0), f.seq,
                             f.raw, f.raw_len);
            ack_pending_ = false;        // piggybacked on this frame
            return out_len > 0;
        }
        // No data to send: emit a lone ack once if we owe one.
        if (!ack_sent_turn_ && ack_pending_) {
            ack_sent_turn_ = true; ack_pending_ = false;
            out_len = Encode(out, cap, 0, 0, nullptr, 0);
            return out_len > 0;
        }
        return false;
    }

    /**
     * @brief Build a bare poll/ack frame (no data) to hand the peer a turn.
     *
     * @param[out] out destination frame buffer.
     * @param[in]  cap capacity of `out`.
     * @return the encoded frame length, or 0 if it didn't fit.
     */
    size_t MakePoll(uint8_t* out, size_t cap) {
        return Encode(out, cap, 0, 0, nullptr, 0);
    }

    /**
     * @brief Feed a received frame into the link layer.
     *
     * Filters by address/peer, tracks the peer epoch, authenticates (if
     * encrypted), applies the ack + SACK bitmap, and buffers/delivers data.
     *
     * @param[in] in  the received frame bytes.
     * @param[in] len number of bytes in `in`.
     * @param[in] now current millis() (currently unused).
     * @return true if this frame ended the peer's burst (F_MORE clear).
     */
    bool OnRx(const uint8_t* in, size_t len, uint32_t now) {
        (void)now;
        if (len < HDR) return false;
        uint8_t src = in[0], dst = in[1], flags = in[2], seq = in[3],
                ack = in[4];
        uint8_t epoch = in[5];
        uint16_t sack_bits = ((uint16_t)in[6] << 8) | in[7];
        uint8_t plen = in[8];
        if (dst != cfg_.addr && dst != ADDR_BCAST) return false;
        if (cfg_.peer != 0 && src != cfg_.peer) return false;

        // Epoch tracking. First contact: record it. A *change* later = peer
        // rebooted.
        if (peer_epoch_ == 0) {
            peer_epoch_ = epoch;
        } else if (epoch != peer_epoch_) {
            rx_next_ = 0; next_seq_ = base_seq_ = 0; inflight_ = 0;
            ack_pending_ = false;
            for (int i = 0; i < MAXWIN; i++) rx_[i].received = false;
            rx_ctr_init_ = false;          // peer rebooted -> resync replay
                                           // window
            rx_aux_ = 0;                   // peer rebooted -> stale telemetry
            peer_epoch_ = epoch;
            peer_reset_ = true;            // signal firmware: re-handshake keys
        }

        // When encryption is on, reject any plaintext frame (no injection of
        // unauthenticated frames into an encrypted link).
        if (cfg_.encrypt && !(flags & F_ENC)) return false;

        size_t off = HDR;
        uint64_t ctr = 0;
        if (flags & F_ENC) {
            if (len < HDR + CTRW) return false;
            for (size_t i = 0; i < CTRW; i++) ctr = (ctr << 8) | in[HDR + i];
            off = HDR + CTRW;
            if (off + plen + cfg_.tag_len > len)
                return false;   // malformed -> drop
        } else if (off + plen > len) {
            plen = (uint8_t)(len - off);
        }

        // Authenticate FIRST when encrypted: do NOT trust ANY header field
        // (ack/sack/seq) until the AEAD tag over header+ctr+payload verifies
        // and the replay window accepts the counter. Tamper or replay -> drop
        // whole frame.
        uint8_t dec[DEC_MAX]; int dec_len = 0; bool have_dec = false;
        if (flags & F_ENC) {
            dec_len = AeadOpen(in, off, plen, flags, src, epoch, ctr, dec,
                               sizeof(dec));
            if (dec_len < 0) return false;
            have_dec = true;
        }

        // The frame is now trusted (authenticated if encrypted): expose its
        // opaque control nibble to the firmware (mode-switch handshake / ADR).
        rx_ctrl_ = (uint8_t)(flags & CTRL_MASK);
        rx_aux_ = in[9];        // peer telemetry byte (authenticated when
                                // encrypted)
        got_valid_rx_ = true;   // a real decoded frame (not RF noise)

        // Process cumulative ack + SACK bitmap against our send window (now
        // trusted).
        AckProcess(ack, sack_bits);

        if (flags & F_DATA) {
            uint8_t d = (uint8_t)(seq - rx_next_);   // distance ahead (mod 256)
            if (d < MAXWIN) {                        // within receive window
                uint8_t slot = seq & (MAXWIN - 1);
                if (!rx_[slot].received || rx_[slot].seq != seq) {
                    int dn = have_dec
                                 ? dec_len
                                 : DecodePlain(in + off, plen, flags, dec,
                                               sizeof(dec));
                    if (dn >= 0) {
                        memcpy(rx_[slot].buf, dec, dn);
                        rx_[slot].len = (uint16_t)dn;
                        rx_[slot].seq = seq;
                        rx_[slot].received = true;
                    }
                }
                DrainInOrder();        // deliver contiguous prefix (with
                                       // backpressure)
                ack_pending_ = true;
            } else {
                // duplicate (already delivered) or beyond window -> just re-ack
                ack_pending_ = true;
            }
        }
        return (flags & F_MORE) == 0;
    }

private:
    /** @brief One in-flight TX frame: its payload plus ARQ bookkeeping. */
    struct Frame {
        uint8_t  seq;                 ///< ARQ sequence number of this frame.
        uint8_t  raw_len;             ///< Payload length in `raw`.
        uint8_t  raw[MAXPAY];         ///< The (uncompressed) payload bytes.
        uint32_t sent_at;             ///< millis() of the last (re)transmit.
        bool     ever_sent;           ///< Has this frame been transmitted yet?
        bool     tx_this_turn;        ///< Already (re)sent in the current turn?
        bool     acked;               ///< Acked (cumulative or SACK) by peer?
        bool     fast_resend;         ///< Flagged for immediate retransmit.
    };
    /** @brief One receive-window slot: a buffered (possibly OoO) frame. */
    struct RxSlot {
        uint8_t  buf[DEC_MAX];        ///< Decoded payload bytes held here.
        uint16_t len;                 ///< Number of valid bytes in `buf`.
        uint8_t  seq;                 ///< ARQ sequence number of held frame.
        bool     received;            ///< Is a frame buffered in this slot?
    };
    Config  cfg_;                     ///< This link's active configuration.
    Frame   win_[MAXWIN];             ///< Send window: in-flight TX frames.
    RxSlot  rx_[MAXWIN];              ///< Receive window: buffered RX frames.
    uint8_t inflight_;                ///< Count of frames currently in `win_`.
    uint8_t base_seq_;                ///< Oldest unacked seq (window base).
    uint8_t next_seq_;                ///< Next seq to assign to a new frame.
    uint8_t rx_next_;                 ///< Next in-order seq to deliver.
    uint64_t tx_ctr64_;               ///< Monotonic TX counter (nonce src).
    uint64_t last_persist_;           ///< Counter value at the last persist_cb.
    uint64_t rx_ctr_max_;             ///< Anti-replay: highest counter seen.
    uint64_t rx_window_;              ///< Anti-replay: 64-bit seen-mask.
    bool    rx_ctr_init_;             ///< Has the replay window been seeded?
    bool    ack_pending_;             ///< We owe the peer an ack.
    bool    ack_sent_turn_;           ///< A lone ack was sent this turn.
    bool    got_data_;                ///< Edge: new in-order data delivered.
    bool    peer_reset_ = false;      ///< Edge: the peer's epoch changed.
    bool    got_valid_rx_ = false;    ///< Edge: a valid decoded frame arrived.
    uint8_t my_epoch_;                ///< Our boot id stamped on TX frames.
    uint8_t peer_epoch_;              ///< Peer's last-seen boot id (0=unknown).
    uint8_t tx_ctrl_;                 ///< Opaque control nibble stamped on TX.
    uint8_t rx_ctrl_;                 ///< Opaque control nibble from last RX.
    uint8_t tx_aux_;                  ///< Telemetry byte stamped on TX.
    uint8_t rx_aux_;                  ///< Telemetry byte from the last RX.
    uint32_t stat_tx_;                ///< Diagnostics: total data frames sent.
    uint32_t stat_retx_;              ///< Diagnostics: of those, resends.

    uint8_t s_buf_[RING];             ///< Send ring storage.
    size_t s_head_;                   ///< Send ring write index (producer).
    size_t s_tail_;                   ///< Send ring read index (consumer).
    uint8_t r_buf_[RING];             ///< Receive ring storage.
    size_t r_head_;                   ///< Receive ring write index.
    size_t r_tail_;                   ///< Receive ring read index.

    /**
     * @brief Bytes currently queued in the send ring.
     * @return the send ring's occupancy in bytes.
     */
    size_t SCount() const { return (s_head_ - s_tail_ + RING) % RING; }

    /**
     * @brief Free space in the send ring.
     *
     * One slot is reserved so a full ring is distinguishable from an empty one.
     *
     * @return the number of bytes that can still be pushed.
     */
    size_t SFree()  const { return RING - 1 - SCount(); }

    /**
     * @brief Push one byte into the send ring.
     * @param[in] b the byte to enqueue (caller ensures SFree() > 0).
     */
    void   SPush(uint8_t b){
        s_buf_[s_head_] = b; s_head_ = (s_head_ + 1) % RING;
    }

    /**
     * @brief Pop the oldest byte from the send ring.
     * @return the dequeued byte (caller ensures SCount() > 0).
     */
    uint8_t SPop(){
        uint8_t b = s_buf_[s_tail_]; s_tail_ = (s_tail_ + 1) % RING; return b;
    }

    /**
     * @brief Bytes currently waiting in the receive ring.
     * @return the receive ring's occupancy in bytes.
     */
    size_t RCount() const { return (r_head_ - r_tail_ + RING) % RING; }

    /**
     * @brief Free space in the receive ring.
     *
     * One slot is reserved so full is distinguishable from empty (as SFree()).
     *
     * @return the number of bytes that can still be pushed.
     */
    size_t RFree()  const { return RING - 1 - RCount(); }

    /**
     * @brief Push one byte into the receive ring.
     *
     * Silently drops the byte if the ring is full (backpressure keeps the
     * frame buffered upstream, so the peer holds and resends).
     *
     * @param[in] b the byte to enqueue.
     */
    void   RPush(uint8_t b){
        if ((r_head_ + 1) % RING != r_tail_){
            r_buf_[r_head_] = b; r_head_ = (r_head_ + 1) % RING;
        }
    }

    /**
     * @brief Pop the oldest byte from the receive ring.
     * @return the dequeued byte (caller ensures RCount() > 0).
     */
    uint8_t RPop(){
        uint8_t b = r_buf_[r_tail_]; r_tail_ = (r_tail_ + 1) % RING; return b;
    }

    /**
     * @brief Deliver the contiguous run of received frames from rx_next_.
     *
     * Copies as long as the recv ring has room; on backpressure (full) it
     * stops, leaving frames buffered so the peer holds and resends.
     */
    void DrainInOrder() {
        for (;;) {
            uint8_t slot = rx_next_ & (MAXWIN - 1);
            if (!(rx_[slot].received && rx_[slot].seq == rx_next_)) break;
            if (RFree() < rx_[slot].len) break;    // no room -> leave it
                                                   // buffered
            for (uint16_t i = 0; i < rx_[slot].len; i++)
                RPush(rx_[slot].buf[i]);
            rx_[slot].received = false;
            rx_next_++;
            got_data_ = true;
        }
    }

    /**
     * @brief Build our SACK bitmap for outgoing frames.
     *
     * Bit k set => seq (rx_next_+1+k) is already buffered at the receiver.
     *
     * @return the 16-bit SACK bitmap.
     */
    uint16_t SackBitmap() const {
        uint16_t b = 0;
        for (uint8_t k = 0; k < MAXWIN - 1; k++) {
            uint8_t s = (uint8_t)(rx_next_ + 1 + k);
            uint8_t slot = s & (MAXWIN - 1);
            if (rx_[slot].received && rx_[slot].seq == s)
                b |= (uint16_t)(1u << k);
        }
        return b;
    }

    /**
     * @brief Are there more un-acked frames to send this turn (for F_MORE)?
     *
     * @param[in] except window index to ignore (the frame being sent now).
     * @return true if another fresh, un-acked frame is waiting this turn.
     */
    bool MoreToSend(int except) const {
        for (uint8_t i = 0; i < inflight_; i++) {
            if ((int)i == except) continue;
            const Frame& f = win_[i];
            if (!f.acked && !f.tx_this_turn && !f.ever_sent) return true;
        }
        return false;
    }

    /**
     * @brief Apply a cumulative ack + SACK bitmap to the send window.
     *
     * Marks cumulatively-acked and SACK'd frames, slides the window past the
     * contiguously-acked front, then flags fast-retransmit on any frame a
     * later ack clearly skipped.
     *
     * @param[in] ack       cumulative ack: next seq the peer expects.
     * @param[in] sack_bits SACK bitmap: bit k => seq ack+1+k buffered at peer.
     */
    void AckProcess(uint8_t ack, uint16_t sack_bits) {
        // Mark cumulatively-acked frames (seq in [base_seq_, ack)).
        uint8_t cum = (uint8_t)(ack - base_seq_);
        if (cum > 0 && cum <= inflight_) {
            for (uint8_t i = 0; i < cum; i++) win_[i].acked = true;
        }
        // Mark SACK'd frames (seq == ack+1+k for set bits).
        for (uint8_t k = 0; k < MAXWIN - 1; k++) {
            if (!(sack_bits & (1u << k))) continue;
            uint8_t s = (uint8_t)(ack + 1 + k);
            for (uint8_t i = 0; i < inflight_; i++) {
                if (win_[i].seq == s) { win_[i].acked = true; break; }
            }
        }
        // Slide the window past the contiguously-acked front.
        uint8_t remove = 0;
        while (remove < inflight_ && win_[remove].acked) remove++;
        if (remove > 0) {
            for (uint8_t i = remove; i < inflight_; i++)
                win_[i - remove] = win_[i];
            inflight_ -= remove;
            base_seq_ = (uint8_t)(base_seq_ + remove);
        }
        // Fast retransmit: any still-unacked frame that the peer has clearly
        // skipped (a LATER frame is already acked) was lost — resend it on the
        // next turn immediately, without waiting out retransmit_ms. Huge win on
        // fast modes where that timeout dwarfs the frame airtime (e.g. GFSK).
        bool acked_after = false;
        for (int i = inflight_ - 1; i >= 0; i--) {
            if (win_[i].acked) { acked_after = true; continue; }
            if (acked_after && win_[i].ever_sent) win_[i].fast_resend = true;
        }
    }

    /**
     * @brief Encode one wire frame: header, optional compress, optional AEAD.
     *
     * Builds the 10-byte header (stamping the current SACK bitmap, control
     * nibble, and aux telemetry byte), optionally compresses the payload (only
     * if it shrinks), and when encryption is on seals the whole frame so the
     * header is authenticated.
     *
     * @param[out] out     destination frame buffer.
     * @param[in]  cap     capacity of `out`.
     * @param[in]  flags   base flag bits (F_DATA / F_MORE, etc.).
     * @param[in]  seq     ARQ sequence number for this frame.
     * @param[in]  raw     plaintext payload (may be null if raw_len == 0).
     * @param[in]  raw_len payload length in bytes.
     * @return the encoded frame length, or 0 if it would not fit in `cap`.
     */
    size_t Encode(uint8_t* out, size_t cap, uint8_t flags, uint8_t seq,
                  const uint8_t* raw, uint8_t raw_len) {
        // Optional compression (only if it actually shrinks).
        uint8_t cbuf[MAXPAY + 16];
        const uint8_t* p = raw; uint8_t plen = raw_len;
        if (raw_len > 0 && cfg_.compress) {
            int c = Compress(raw, raw_len, cbuf, sizeof(cbuf));
            if (c > 0 && c < raw_len) {
                p = cbuf; plen = (uint8_t)c; flags |= F_COMP;
            }
        }
        // When encryption is on, EVERY frame is sealed (even empty ACKs) so the
        // whole header is authenticated -> no forged acks/sacks.
        bool enc = cfg_.encrypt && cfg_.key;
        if (enc) flags |= F_ENC;
        uint8_t tlen = enc ? cfg_.tag_len : 0;
        size_t off = HDR + (enc ? CTRW : 0);
        size_t need = off + plen + tlen;
        if (need > cap) return 0;

        uint16_t sack = SackBitmap();
        out[0] = cfg_.addr;
        out[1] = cfg_.peer ? cfg_.peer : ADDR_BCAST;
        out[2] = (uint8_t)(flags | (tx_ctrl_ & CTRL_MASK));   // + control bits
        out[3] = seq;
        out[4] = rx_next_;
        out[5] = my_epoch_;
        out[6] = (uint8_t)(sack >> 8);
        out[7] = (uint8_t)(sack & 0xFF);
        out[8] = plen;
        out[9] = tx_aux_;   // firmware telemetry (peer-SNR), authenticated

        if (enc) {
            uint64_t ctr = ++tx_ctr64_;
            MaybePersist();
            for (size_t i = 0; i < CTRW; i++)
                out[HDR + i] = (uint8_t)(ctr >> (8 * (CTRW - 1 - i)));
            uint8_t nonce[16]; BuildNonce(nonce, cfg_.addr, my_epoch_, ctr);
            uint8_t tag[16];
            // AD = the 10-byte header + the 8 counter bytes (out[0..HDR+CTRW)).
            Ascon128Encrypt(cfg_.key, nonce, out, HDR + CTRW, p, plen,
                             out + off, tag);
            memcpy(out + off + plen, tag, tlen);
        } else {
            memcpy(out + off, p, plen);
        }
        return need;
    }

    /**
     * @brief Build the 16-byte AEAD nonce for a frame.
     *
     * Layout: src ‖ epoch ‖ ctr64 (big-endian) ‖ zeros — unique per
     * (key, src, ctr), which is what keeps the keystream from ever repeating.
     *
     * @param[out] n     16-byte nonce buffer to fill.
     * @param[in]  src   sender link address.
     * @param[in]  epoch sender boot id.
     * @param[in]  ctr   monotonic 64-bit frame counter.
     */
    static void BuildNonce(uint8_t n[16], uint8_t src, uint8_t epoch,
                           uint64_t ctr) {
        memset(n, 0, 16);
        n[0] = src; n[1] = epoch;
        for (int i = 0; i < 8; i++) n[2 + i] = (uint8_t)(ctr >> (8 * (7 - i)));
    }

    /**
     * @brief Persist the TX counter every persist_stride frames.
     *
     * Bounds counter reuse after a crash while limiting flash wear: the saved
     * value is pre-bumped so a reboot never reissues a used counter.
     */
    void MaybePersist() {
        if (cfg_.persist_cb &&
            tx_ctr64_ - last_persist_ >= cfg_.persist_stride) {
            last_persist_ = tx_ctr64_;
            cfg_.persist_cb(cfg_.persist_arg, tx_ctr64_);  // firmware -> NVS
        }
    }

    /**
     * @brief Sliding-window anti-replay check (IPsec/LoRaWAN style).
     *
     * Accepts a counter once; rejects anything already seen or older than the
     * 64-frame window, and advances the window for newer counters.
     *
     * @param[in] ctr the received frame counter.
     * @return true if the counter is fresh (accept); false if old/duplicate.
     */
    bool ReplayOk(uint64_t ctr) {
        if (!rx_ctr_init_) {
            rx_ctr_init_ = true; rx_ctr_max_ = ctr; rx_window_ = 1;
            return true;
        }
        if (ctr > rx_ctr_max_) {
            uint64_t d = ctr - rx_ctr_max_;
            rx_window_ = (d >= 64) ? 0 : (rx_window_ << d);
            rx_window_ |= 1;
            rx_ctr_max_ = ctr;
            return true;
        }
        uint64_t d = rx_ctr_max_ - ctr;
        if (d >= 64) return false;                       // too old
        uint64_t bit = 1ULL << d;
        if (rx_window_ & bit) return false;              // already seen
        rx_window_ |= bit;
        return true;
    }

    /**
     * @brief Decrypt + verify + replay-check a frame, then decompress.
     *
     * @param[in]  frame the full received frame.
     * @param[in]  off   offset of the ciphertext within `frame`.
     * @param[in]  plen  ciphertext length (== plaintext length).
     * @param[in]  flags frame flags (F_COMP decides decompression).
     * @param[in]  src   sender address (for the nonce).
     * @param[in]  epoch sender epoch (for the nonce).
     * @param[in]  ctr   frame counter (for the nonce + replay check).
     * @param[out] out   destination for the recovered app payload.
     * @param[in]  cap   capacity of `out`.
     * @return final app-payload length, or -1 on tamper/replay/decompress
     *         failure.
     */
    int AeadOpen(const uint8_t* frame, size_t off, uint8_t plen, uint8_t flags,
                 uint8_t src, uint8_t epoch, uint64_t ctr, uint8_t* out,
                 size_t cap) {
        if (!cfg_.key) return -1;
        uint8_t buf[MAXPAY + 16];
        uint8_t nonce[16]; BuildNonce(nonce, src, epoch, ctr);
        const uint8_t* ct  = frame + off;
        const uint8_t* tag = frame + off + plen;
        if (!Ascon128Decrypt(cfg_.key, nonce, frame, HDR + CTRW, ct, plen, tag,
                              cfg_.tag_len, buf))
            return -1;                                   // bad tag -> tampered
        if (!ReplayOk(ctr)) return -1;                   // replayed -> drop
        if (flags & F_COMP) return Decompress(buf, plen, out, cap);
        if (plen > cap) return -1;
        memcpy(out, buf, plen);
        return plen;
    }

    /**
     * @brief Decode an unencrypted payload (decompress if F_COMP set).
     *
     * @param[in]  in    the payload bytes.
     * @param[in]  plen  payload length.
     * @param[in]  flags frame flags (F_COMP decides decompression).
     * @param[out] out   destination for the recovered app payload.
     * @param[in]  cap   capacity of `out`.
     * @return the app-payload length, or -1 if it would not fit.
     */
    int DecodePlain(const uint8_t* in, uint8_t plen, uint8_t flags,
                    uint8_t* out, size_t cap) {
        if (flags & F_COMP) return Decompress(in, plen, out, cap);
        if (plen > cap) return -1;
        memcpy(out, in, plen);
        return plen;
    }
};

} // namespace link_layer

#endif  // LINK_LAYER_LINKLAYER_H_
