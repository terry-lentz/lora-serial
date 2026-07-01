/**
 * @file test_modem.cpp
 * @brief Threaded half-duplex modem sim driving the real Initiator/Responder
 *        step loops against a listen-gated channel.
 *
 * Two threads run the REAL InitiatorStep/ResponderStep against a shared
 * channel that delivers a frame only if the peer is currently listening (in
 * rx). This reproduces turn-taking / missed-frame bugs the deterministic
 * alternating sim can't. Run: pio test -e native -f test_modem
 */
#include <unity.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <vector>
#include <cstring>
#include "linklayer.h"
#include "modem.h"

/// Monotonic clock the sim measures elapsed milliseconds against.
using clk = std::chrono::steady_clock;
/// Start instant of the current run; nowms() is measured from here.
static clk::time_point g_start;

/**
 * @brief Milliseconds elapsed since g_start (the sim's monotonic clock).
 *
 * @return milliseconds since the run began.
 */
static uint32_t nowms() {
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        clk::now() - g_start).count();
}

/**
 * @brief Half-duplex medium between the two link ends.
 *
 * A frame is delivered to a side only if that side is currently blocked in rx()
 * (listening) — otherwise it's missed, just like a real radio that wasn't in
 * receive mode when the preamble arrived.
 */
struct SimChannel {
    std::mutex m;                  ///< guards the per-side mailboxes.
    std::condition_variable cv;    ///< signals a waiting receiver.
    /// @brief One direction's mailbox: is the receiver listening, and the
    ///        pending frame (if any).
    struct Side {
        bool listening = false;    ///< true while the receiver waits in rx.
        std::vector<uint8_t> frame; ///< the pending frame bytes.
        bool has = false;          ///< true if a frame is waiting to be read.
    };
    Side toInitiator, toResponder; ///< the two directional mailboxes.
    int lossPct = 0;               ///< per-frame loss percentage.
    uint32_t rng = 7;              ///< deterministic loss-LCG state.

    /**
     * @brief Roll the loss dice for one frame (deterministic LCG).
     *
     * @return true if this frame should be dropped.
     */
    bool drop() {
        if (lossPct <= 0) return false;
        rng = rng * 1664525u + 1013904223u;
        return (rng % 100) < (uint32_t)lossPct;
    }

    /**
     * @brief Transmit a frame into the channel toward the peer.
     *
     * Realistic-ish: time-on-air scales with frame size; the receiver must be
     * listening for (most of) that window. Models back-to-back burst frames
     * the receiver misses while re-arming.
     *
     * @param[in] fromInitiator true if the initiator is sending (target is the
     *                          responder), false for the reverse direction.
     * @param[in] b             the frame bytes.
     * @param[in] n             the frame length in bytes.
     */
    void tx(bool fromInitiator, const uint8_t* b, size_t n) {
        uint32_t toa = 6 + (uint32_t)n / 12;        // ms, ~ grows with size
        std::this_thread::sleep_for(std::chrono::milliseconds(toa));
        std::lock_guard<std::mutex> lk(m);
        Side& dst = fromInitiator ? toResponder : toInitiator;
        if (dst.listening && !drop()) {
            dst.frame.assign(b, b + n); dst.has = true; cv.notify_all();
        }
        // else: peer not listening (mid-tx/processing/re-arm) or lost -> missed
    }
    /**
     * @brief Block until a frame arrives for this side or the timeout expires.
     *
     * Marks this side as listening for the wait; a frame transmitted while it
     * is not listening is missed, like a radio not in receive mode.
     *
     * @param[in]  isInitiator true to receive on the initiator's side.
     * @param[out] b           buffer the received frame is copied into.
     * @param[in]  cap         capacity of @p b in bytes.
     * @param[out] n           number of bytes written to @p b.
     * @param[in]  timeoutMs   how long to wait for a frame, in milliseconds.
     * @return true if a frame was received, false on timeout.
     */
    bool rx(bool isInitiator, uint8_t* b, size_t cap, size_t& n,
            uint32_t timeoutMs) {
        // RX re-arm latency
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        std::unique_lock<std::mutex> lk(m);
        Side& mine = isInitiator ? toInitiator : toResponder;
        mine.listening = true;
        bool ok = cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                              [&] { return mine.has; });
        mine.listening = false;
        if (ok && mine.has) {
            n = std::min(cap, mine.frame.size());
            memcpy(b, mine.frame.data(), n); mine.has = false; return true;
        }
        return false;
    }
};

/// @brief IRadio adapter that bridges the link layer's radio interface to
///        SimChannel, tagged with which end of the link this instance drives.
struct SimRadio : link_layer::IRadio {
    SimChannel* ch;     ///< the shared channel this radio transmits through.
    bool initiator;     ///< true if this instance drives the initiator end.
    /**
     * @brief Bind this radio adapter to a channel and a link end.
     *
     * @param[in] c the shared SimChannel both ends transmit through.
     * @param[in] m true if this instance drives the initiator end.
     */
    SimRadio(SimChannel* c, bool m) : ch(c), initiator(m) {}

    /**
     * @brief Transmit a frame onto the channel; always "succeeds".
     *
     * @param[in] b the frame bytes.
     * @param[in] n the frame length in bytes.
     * @return true (the sim channel always accepts the transmit).
     */
    bool tx(const uint8_t* b, size_t n) override {
        ch->tx(initiator, b, n); return true;
    }

    /**
     * @brief Receive a frame from the channel (blocks up to @p to ms).
     *
     * @param[out] b   buffer the received frame is copied into.
     * @param[in]  cap capacity of @p b in bytes.
     * @param[out] n   number of bytes written to @p b.
     * @param[in]  to  receive timeout in milliseconds.
     * @return true if a frame was received, false on timeout.
     */
    bool rx(uint8_t* b, size_t cap, size_t& n, uint32_t to) override {
        return ch->rx(initiator, b, cap, n, to);
    }

    /**
     * @brief Current sim time in milliseconds.
     *
     * @return milliseconds since the run began.
     */
    uint32_t now() override { return nowms(); }
};

/// 16-byte AEAD key shared by both ends of the simulated link.
static const uint8_t KEY[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};

/**
 * @brief Modem turn-taking timing, scaled for the fast native sim clock.
 *
 * @return a populated ModemTiming with short, test-friendly intervals.
 */
static link_layer::ModemTiming scaled() {
    link_layer::ModemTiming t;
    t.turn_rx_ms = 200; t.interframe_ms = 90; t.responder_wait_ms = 250;
    t.max_idle_gap_ms = 120; return t;
}
/**
 * @brief Build a link Config for one end of the simulated link.
 *
 * @param[in] a    this end's 1-byte address.
 * @param[in] p    the peer's 1-byte address.
 * @param[in] comp enable payload compression.
 * @param[in] enc  enable AEAD encryption (uses the shared KEY).
 * @return the assembled Config.
 */
static link_layer::Config cfg(uint8_t a, uint8_t p, bool comp, bool enc) {
    link_layer::Config c;
    c.addr = a; c.peer = p; c.window = 4; c.retransmit_ms = 120;
    c.compress = comp; c.encrypt = enc; c.key = enc ? KEY : nullptr; return c;
}

/**
 * @brief Drive a full echo run: initiator sends, responder echoes it back.
 *
 * @param[in] comp      enable compression on both ends.
 * @param[in] enc       enable encryption on both ends.
 * @param[in] total     number of bytes the initiator sends.
 * @param[in] loss      per-frame loss percentage on the channel.
 * @param[in] timeoutMs wall-clock budget for the run, in milliseconds.
 * @return the bytes the initiator received back (should equal what it sent).
 */
static std::vector<uint8_t> echo_test(bool comp, bool enc, size_t total,
                                      int loss, int timeoutMs) {
    g_start = clk::now();
    static link_layer::LinkLayer<16384> M, S;
    M.Init(cfg(1, 2, comp, enc));
    S.Init(cfg(2, 1, comp, enc));
    SimChannel ch; ch.lossPct = loss;
    SimRadio mr(&ch, true), sr(&ch, false);
    link_layer::ModemTiming t = scaled();

    std::vector<uint8_t> src(total);
    for (size_t i = 0; i < total; i++) src[i] = (uint8_t)('A' + (i % 26));
    std::vector<uint8_t> got;
    std::atomic<bool> done{false};

    std::thread mt([&] {
        link_layer::ModemState st;
        M.Write(src.data(), src.size());
        uint8_t b[256]; size_t n;
        while (!done) {
            link_layer::InitiatorStep(M, mr, st, t);
            while ((n = M.Read(b, sizeof(b))) > 0)
                got.insert(got.end(), b, b + n);
        }
    });
    std::thread stx([&] {
        uint8_t b[256]; size_t n;
        while (!done) {
            link_layer::ResponderStep(S, sr, t);
            while ((n = S.Read(b, sizeof(b))) > 0) S.Write(b, n); // echo back
        }
    });

    auto deadline = clk::now() + std::chrono::milliseconds(timeoutMs);
    while (got.size() < total && clk::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    done = true;
    mt.join(); stx.join();
    return got;
}

/**
 * @brief Run an echo test and assert it returns the input byte-exact.
 *
 * @param[in] comp      enable compression on both ends.
 * @param[in] enc       enable encryption on both ends.
 * @param[in] total     number of bytes to send.
 * @param[in] loss      per-frame loss percentage on the channel.
 * @param[in] timeoutMs wall-clock budget for the run, in milliseconds.
 */
static void check_echo(bool comp, bool enc, size_t total, int loss,
                       int timeoutMs) {
    std::vector<uint8_t> src(total);
    for (size_t i = 0; i < total; i++) src[i] = (uint8_t)('A' + (i % 26));
    auto got = echo_test(comp, enc, total, loss, timeoutMs);
    TEST_ASSERT_EQUAL_UINT32(total, got.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src.data(), got.data(), total);
}

/**
 * @brief One-way bulk transfer into a deliberately SLOW receiving consumer.
 *
 * Models a terminal/tio that can't keep up with a big burst: the rings are
 * small, so the recv ring fills. Without end-to-end backpressure the link
 * silently drops the overflow; with it, the sender holds + retransmits and
 * every byte arrives in order.
 *
 * @param[in] total        number of bytes to send.
 * @param[in] drainChunk   bytes the consumer reads per drain tick.
 * @param[in] drainEveryMs minimum interval between drains, in milliseconds.
 * @param[in] timeoutMs    wall-clock budget for the run, in milliseconds.
 * @return the bytes the receiver collected.
 */
static std::vector<uint8_t> oneway_slow(size_t total, int drainChunk,
                                        int drainEveryMs, int timeoutMs) {
    g_start = clk::now();
    // small rings -> backpressure must engage
    static link_layer::LinkLayer<2048> M, S;
    M.Init(cfg(1, 2, false, false));
    S.Init(cfg(2, 1, false, false));
    SimChannel ch;
    SimRadio mr(&ch, true), sr(&ch, false);
    link_layer::ModemTiming t = scaled();

    std::vector<uint8_t> src(total);
    for (size_t i = 0; i < total; i++) src[i] = (uint8_t)('A' + (i % 26));
    std::vector<uint8_t> got;
    std::atomic<bool> done{false};

    std::thread mt([&] {
        link_layer::ModemState st;
        size_t fed = 0;
        while (!done) {
            // honor short writes
            if (fed < total) fed += M.Write(src.data() + fed, total - fed);
            link_layer::InitiatorStep(M, mr, st, t);
        }
    });
    std::thread stx([&] {
        uint8_t b[256]; size_t n; uint32_t lastDrain = 0;
        while (!done) {
            link_layer::ResponderStep(S, sr, t);
            // rate-limited consumer
            if ((int)(nowms() - lastDrain) >= drainEveryMs) {
                lastDrain = nowms();
                size_t want = (size_t)drainChunk < sizeof(b)
                    ? (size_t)drainChunk : sizeof(b);
                if ((n = S.Read(b, want)) > 0) got.insert(got.end(), b, b + n);
            }
        }
    });

    auto deadline = clk::now() + std::chrono::milliseconds(timeoutMs);
    while (got.size() < total && clk::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    done = true;
    mt.join(); stx.join();
    return got;
}

/**
 * @brief Verifies end-to-end backpressure: a slow consumer loses no bytes.
 *
 * The transfer exceeds the recv ring + window, so without backpressure it
 * would overflow; with it, every byte arrives in order.
 */
void test_modem_slow_consumer() {
    // > ring(2048)+window -> overflow without backpressure
    const size_t total = 5000;
    std::vector<uint8_t> src(total);
    for (size_t i = 0; i < total; i++) src[i] = (uint8_t)('A' + (i % 26));
    auto got = oneway_slow(total, 64, 20, 60000);   // ~3.2 KB/s consumer
    TEST_ASSERT_EQUAL_UINT32(total, got.size());     // nothing dropped
    // in order, intact
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src.data(), got.data(), total);
}

/** @brief A tiny clean-channel echo round-trips byte-exact. */
void test_modem_echo_small()       { check_echo(false, false, 32,   0, 15000); }
/** @brief A bulk clean-channel echo round-trips byte-exact. */
void test_modem_echo_bulk()        { check_echo(false, false, 2000, 0, 60000); }
/** @brief A bulk echo with compression + encryption round-trips byte-exact. */
void test_modem_echo_comp_enc()    {
    check_echo(true,  true,  2000, 0, 60000);
}
/** @brief An echo over a 15%-loss channel still round-trips byte-exact. */
void test_modem_echo_loss()        {
    check_echo(false, false, 1000, 15, 60000);
}

/** @brief Unity per-test setup hook (no global state to prepare). */
void setUp() {}
/** @brief Unity per-test teardown hook (nothing to clean up). */
void tearDown() {}

/**
 * @brief Register and run every threaded modem-sim test under Unity.
 *
 * @return Unity's aggregate pass/fail status code.
 */
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_modem_slow_consumer);
    RUN_TEST(test_modem_echo_small);
    RUN_TEST(test_modem_echo_bulk);
    RUN_TEST(test_modem_echo_comp_enc);
    RUN_TEST(test_modem_echo_loss);
    return UNITY_END();
}
