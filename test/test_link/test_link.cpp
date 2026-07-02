/**
 * @file test_link.cpp
 * @brief Native unit tests for the portable link layer: ARQ, AEAD, X25519,
 *        mode-switch, ADR, sessions, the frame ring, and identity election.
 *
 * Exercises the link layer over a simulated in-memory radio channel with loss
 * injection, plus the supporting crypto and control logic. Run:
 * pio test -e native
 */
#include <unity.h>
#include <vector>
#include <cstdint>
#include <cstdio>
#include "linklayer.h"
#include "ll_transform.h"
#include "ll_aead.h"     // AsconKdf16 (pairing KDF)
#include "x25519.h"      // ECDH for secure pairing
#include "modeswitch.h"  // coordinated PHY mode-switch handshake
#include "adr.h"          // portable Adaptive-Data-Rate decision logic
#include "session.h"      // per-session key handshake (forward secrecy)
#include "frame_ring.h"   // SPSC frame ring for interrupt-driven RX
#include "prng.h"         // link_layer::Lcg — deterministic loss PRNG
#include "identity.h"     // link_layer::ElectInitiator — MAC role election
#include <cstring>

/// 16-byte AEAD key shared by both ends of the simulated link.
static const uint8_t KEY[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};

/// Deterministic PRNG for reproducible loss patterns (see prng.h).
static link_layer::Lcg g_loss_rng(12345, link_layer::kLcgNumRecipes);
/**
 * @brief Reseed the loss PRNG for a reproducible run.
 *
 * @param[in] s the new PRNG seed.
 */
static void rng_seed(uint32_t s) { g_loss_rng.Seed(s); }
/**
 * @brief Next pseudo-random word from the loss PRNG.
 *
 * @return the next 32-bit pseudo-random value.
 */
static uint32_t rnd() { return g_loss_rng.Next(); }
/**
 * @brief True with probability @p pct percent (for per-frame loss).
 *
 * @param[in] pct the drop probability, in percent.
 * @return true if this frame should be dropped.
 */
static bool drop(int pct) { return pct > 0 && (rnd() % 100) < (uint32_t)pct; }

/**
 * @brief Generate a realistic-ish payload (mixed text, compressible).
 *
 * @param[in] n number of bytes to generate.
 * @return a vector of @p n bytes of repeating, compressible text.
 */
static std::vector<uint8_t> make_payload(size_t n) {
    std::vector<uint8_t> v;
    const char* lines[] = {
        "total 1234\n", "drwxr-xr-x 2 root root 4096 Jun 25 file_",
        "the quick brown fox\n",
        "-rw-r--r-- 1 terry terry  220 Apr  3 .bashrc\n",
        "1234567890 abcdefghij\n"};
    size_t i = 0;
    while (v.size() < n) {
        const char* s = lines[i++ % 5];
        for (const char* p = s; *p && v.size() < n; ++p)
            v.push_back((uint8_t)*p);
    }
    return v;
}

/// @brief Outcome of a simulated stream: the received bytes + round count.
struct Result {
    std::vector<uint8_t> got;   ///< the bytes the receiver collected.
    int rounds;                 ///< number of round-trips the transfer took.
};

/**
 * @brief Stream `total` bytes A->B through the sim channel.
 *
 * lossPct drops frames in BOTH directions, using the deterministic loss PRNG
 * seeded by @p seed.
 *
 * @param[in] ca      end A's link Config.
 * @param[in] cb      end B's link Config.
 * @param[in] total   number of bytes to stream.
 * @param[in] lossPct per-frame loss percentage (both directions).
 * @param[in] seed    loss PRNG seed for a reproducible pattern.
 * @return what B received plus the number of round-trips used.
 */
static Result stream(link_layer::Config ca, link_layer::Config cb,
                     size_t total, int lossPct, uint32_t seed) {
    static link_layer::LinkLayer<16384> A, B;  // static: avoid big stack frames
    A.Init(ca); B.Init(cb);
    rng_seed(seed);
    auto src = make_payload(total);
    Result r; r.rounds = 0;
    size_t sent = 0;
    uint32_t now = 0;
    uint8_t buf[link_layer::MAXFRAME]; size_t len;
    uint8_t out[512];
    for (int round = 0; round < 100000 && r.got.size() < total; round++) {
        r.rounds++;
        sent += A.Write(src.data() + sent, src.size() - sent);
        A.BeginTurn();
        while (A.NextTx(buf, sizeof(buf), len, now))
            if (!drop(lossPct)) B.OnRx(buf, len, now);
        size_t k;
        while ((k = B.Read(out, sizeof(out))) > 0)
            for (size_t i = 0; i < k; i++) r.got.push_back(out[i]);
        B.BeginTurn();
        while (B.NextTx(buf, sizeof(buf), len, now))
            if (!drop(lossPct)) A.OnRx(buf, len, now);
        while ((k = A.Read(out, sizeof(out))) > 0) { /* B sends nothing */ }
        now += 150;   // > retransmit_ms so lost frames get resent
    }
    return r;
}

/**
 * @brief Build end A's link Config (addr 1, peer 2).
 *
 * @param[in] comp enable compression.
 * @param[in] enc  enable AEAD encryption (uses the shared KEY).
 * @return the assembled Config.
 */
static link_layer::Config cfgA(bool comp, bool enc) {
    link_layer::Config c;
    c.addr = 1; c.peer = 2; c.window = 4; c.retransmit_ms = 100;
    c.compress = comp; c.encrypt = enc; c.key = enc ? KEY : nullptr; return c;
}

/**
 * @brief Build end B's link Config (addr 2, peer 1).
 *
 * @param[in] comp enable compression.
 * @param[in] enc  enable AEAD encryption (uses the shared KEY).
 * @return the assembled Config.
 */
static link_layer::Config cfgB(bool comp, bool enc) {
    link_layer::Config c;
    c.addr = 2; c.peer = 1; c.window = 4; c.retransmit_ms = 100;
    c.compress = comp; c.encrypt = enc; c.key = enc ? KEY : nullptr; return c;
}

/**
 * @brief Stream a payload A->B and assert it arrives complete + byte-exact.
 *
 * @param[in] comp  enable compression on both ends.
 * @param[in] enc   enable encryption on both ends.
 * @param[in] total number of bytes to stream.
 * @param[in] loss  per-frame loss percentage (both directions).
 */
static void check_stream(bool comp, bool enc, size_t total, int loss) {
    auto src = make_payload(total);
    Result r = stream(cfgA(comp, enc), cfgB(comp, enc), total, loss, 999);
    TEST_ASSERT_EQUAL_UINT32(total, r.got.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src.data(), r.got.data(), total);
}

// ---- tests ----

/**
 * @brief Compress then decompress a payload recovers it byte-exact + smaller.
 */
void test_compress_roundtrip() {
    auto v = make_payload(200);
    uint8_t comp[300];
    int c = link_layer::Compress(v.data(), v.size(), comp, sizeof(comp));
    TEST_ASSERT_GREATER_THAN(0, c);
    TEST_ASSERT_LESS_THAN((int)v.size(), c);            // actually shrank
    uint8_t dec[300]; int d = link_layer::Decompress(comp, c, dec, sizeof(dec));
    TEST_ASSERT_EQUAL_INT((int)v.size(), d);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(v.data(), dec, v.size());
}

/**
 * @brief Format bytes as uppercase hex and assert they equal @p exp.
 *
 * @param[in] got the bytes to format.
 * @param[in] n   number of bytes in @p got.
 * @param[in] exp the expected uppercase-hex string.
 */
static void assert_hex(const uint8_t* got, size_t n, const char* exp) {
    char h[80]; size_t p = 0;
    for (size_t i = 0; i < n && p + 2 < sizeof(h); i++)
        p += snprintf(h + p, sizeof(h) - p, "%02X", got[i]);
    TEST_ASSERT_EQUAL_STRING(exp, h);
}

/**
 * @brief Ascon-128 AEAD matches the official LWC known-answer vectors.
 *
 * If these pass, the cipher is real, correct, and interoperable.
 */
void test_aead_kat() {
    uint8_t k[16], n[16]; for (int i = 0; i < 16; i++) { k[i] = i; n[i] = i; }
    uint8_t tag[16];
    link_layer::Ascon128Encrypt(k, n, nullptr, 0, nullptr, 0, nullptr, tag);
    assert_hex(tag, 16, "E355159F292911F794CB1432A0103A8A");          // Count 1
    uint8_t ad1[1] = {0x00};
    link_layer::Ascon128Encrypt(k, n, ad1, 1, nullptr, 0, nullptr, tag);
    assert_hex(tag, 16, "944DF887CD4901614C5DEDBC42FC0DA0");          // Count 2
    uint8_t pt1[1] = {0x00}, ct1[1], full[17];
    link_layer::Ascon128Encrypt(k, n, nullptr, 0, pt1, 1, ct1, tag);
    full[0] = ct1[0]; memcpy(full + 1, tag, 16);
    // Count 34
    assert_hex(full, 17, "BC18C3F4E39ECA7222490D967C79BFFC92");
}

/**
 * @brief AEAD detects tampering and a wrong key; an 8-byte tag round-trips.
 */
void test_aead_tamper() {
    uint8_t k[16], n[16];
    for (int i = 0; i < 16; i++) { k[i] = i; n[i] = 0x40 + i; }
    const char* m = "authenticated payload xyz"; size_t mn = strlen(m);
    uint8_t ad[4] = {9, 8, 7, 6}, ct[64], tag[16], out[64];
    link_layer::Ascon128Encrypt(k, n, ad, 4, (const uint8_t*)m, mn, ct, tag);
    TEST_ASSERT_TRUE(
        link_layer::Ascon128Decrypt(k, n, ad, 4, ct, mn, tag, 16, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(m, out, mn);
    ct[5] ^= 1;
    TEST_ASSERT_FALSE(
        link_layer::Ascon128Decrypt(k, n, ad, 4, ct, mn, tag, 16, out));
    ct[5] ^= 1;
    ad[1] ^= 1;
    TEST_ASSERT_FALSE(
        link_layer::Ascon128Decrypt(k, n, ad, 4, ct, mn, tag, 16, out));
    ad[1] ^= 1;
    tag[0] ^= 1;
    TEST_ASSERT_FALSE(
        link_layer::Ascon128Decrypt(k, n, ad, 4, ct, mn, tag, 16, out));
    tag[0] ^= 1;
    // 8-byte tag ok
    TEST_ASSERT_TRUE(
        link_layer::Ascon128Decrypt(k, n, ad, 4, ct, mn, tag, 8, out));
}

/**
 * @brief End-to-end integrity: an active attacker flipping bits in in-flight
 *        frames cannot corrupt the delivered stream.
 *
 * AEAD drops every tampered frame (never delivers garbage); the ARQ resends
 * them, so the stream still arrives byte-exact.
 */
void test_aead_tamper_stream() {
    static link_layer::LinkLayer<16384> A, B;
    A.Init(cfgA(false, true)); B.Init(cfgB(false, true));
    rng_seed(2024);
    auto src = make_payload(4000);
    std::vector<uint8_t> got;
    size_t sent = 0; uint32_t now = 0;
    uint8_t buf[link_layer::MAXFRAME]; size_t len; uint8_t out[512];
    for (int round = 0; round < 100000 && got.size() < 4000; round++) {
        sent += A.Write(src.data() + sent, src.size() - sent);
        A.BeginTurn();
        while (A.NextTx(buf, sizeof(buf), len, now)) {
            // tamper ciphertext
            if (len > link_layer::HDR + link_layer::CTRW && (rnd() % 100) < 25)
                buf[link_layer::HDR + link_layer::CTRW] ^= 1;
            B.OnRx(buf, len, now);
        }
        size_t k;
        while ((k = B.Read(out, sizeof(out))) > 0)
            for (size_t i = 0; i < k; i++) got.push_back(out[i]);
        B.BeginTurn();
        while (B.NextTx(buf, sizeof(buf), len, now)) A.OnRx(buf, len, now);
        now += 150;
    }
    TEST_ASSERT_EQUAL_UINT32(4000, got.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src.data(), got.data(), 4000);
}

/**
 * @brief Parse a hex string into bytes (for known-answer test vectors).
 *
 * @param[in]  h the hex string (2 chars per byte).
 * @param[out] o the output byte buffer.
 * @param[in]  n number of bytes to parse.
 */
static void hexb(const char* h, uint8_t* o, int n) {
    for (int i=0;i<n;i++){
        unsigned b; sscanf(h+2*i,"%02x",&b); o[i]=(uint8_t)b;
    }
}

/**
 * @brief X25519 matches the RFC 7748 known-answer vectors (curve is correct).
 */
void test_x25519_kat() {
    uint8_t s[32], u[32], o[32];
    hexb("a546e36bf0527c9d3b16154b82465edd"
         "62144c0ac1fc5a18506a2244ba449ac4", s, 32);
    hexb("e6db6867583030db3594c1a424b15f7c"
         "726624ec26b3353b10a903a6d0ab1c4c", u, 32);
    x25519::scalarmult(o, s, u);
    uint8_t exp[32];
    hexb("c3da55379de9c6908e94ea4df28d084f"
         "32eccf03491c71f754b4075577a28552", exp, 32);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp, o, 32);
}

/**
 * @brief Secure-pairing key agreement: both parties derive the SAME 16-byte
 *        link key via X25519 + Ascon KDF (what AT+TRAIN does over the air).
 *
 * Also checks a different peer key yields a different link key.
 */
void test_pair_agreement() {
    uint8_t apriv[32], bpriv[32];
    // fixed "random" keys
    for (int i=0;i<32;i++){
        apriv[i]=(uint8_t)(i*7+1); bpriv[i]=(uint8_t)(i*13+9);
    }
    uint8_t apub[32], bpub[32];
    x25519::scalarmult_base(apub, apriv);
    x25519::scalarmult_base(bpub, bpriv);
    uint8_t sa[32], sb[32];
    x25519::scalarmult(sa, apriv, bpub);   // Alice: own priv + Bob's public
    x25519::scalarmult(sb, bpriv, apub);   // Bob:   own priv + Alice's public
    TEST_ASSERT_EQUAL_UINT8_ARRAY(sa, sb, 32);           // same shared secret
    uint8_t ka[16], kb[16];
    link_layer::AsconKdf16(sa, 32, ka);
    link_layer::AsconKdf16(sb, 32, kb);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ka, kb, 16);           // -> same link key
    // sanity: a different peer key yields a different link key
    uint8_t cpriv[32]; for (int i=0;i<32;i++) cpriv[i]=(uint8_t)(i*3+5);
    uint8_t cpub[32], sc[32], kc[16];
    x25519::scalarmult_base(cpub, cpriv);
    x25519::scalarmult(sc, apriv, cpub); link_layer::AsconKdf16(sc, 32, kc);
    TEST_ASSERT_TRUE(memcmp(ka, kc, 16) != 0);
}

/** @brief A clean-channel 10 KB stream arrives byte-exact. */
void test_stream_lossless()        { check_stream(false, false, 10000, 0); }
/** @brief A 6 KB stream over 20% loss arrives byte-exact. */
void test_stream_loss_20()         { check_stream(false, false, 6000, 20); }
/** @brief A compressed 10 KB stream arrives byte-exact. */
void test_stream_compress()        { check_stream(true,  false, 10000, 0); }
/** @brief An encrypted (AEAD, 8-byte tag) 6 KB stream arrives byte-exact. */
void test_stream_encrypt()         { check_stream(false, true,  6000, 0); }
/** @brief A compressed + encrypted stream over 15% loss arrives byte-exact. */
void test_stream_comp_enc_loss()   { check_stream(true,  true,  6000, 15); }

/**
 * @brief AEAD with the full 16-byte tag also streams byte-exact under loss.
 */
void test_stream_encrypt_tag16() {
    link_layer::Config ca = cfgA(false, true), cb = cfgB(false, true);
    ca.tag_len = 16; cb.tag_len = 16;
    auto src = make_payload(6000);
    Result r = stream(ca, cb, 6000, 10, 777);
    TEST_ASSERT_EQUAL_UINT32(6000, r.got.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src.data(), r.got.data(), 6000);
}

/**
 * @brief Anti-replay window: a counter is accepted once; replays and too-old
 *        counters are rejected; in-window reordering is tolerated.
 */
void test_aead_replay_window() {
    static link_layer::LinkLayer<1024> L; L.Init(cfgB(false, true));
    TEST_ASSERT_TRUE (L.DbgReplayOk(5));     // first
    TEST_ASSERT_TRUE (L.DbgReplayOk(6));
    TEST_ASSERT_FALSE(L.DbgReplayOk(6));     // exact replay
    TEST_ASSERT_FALSE(L.DbgReplayOk(5));     // replay of older
    TEST_ASSERT_TRUE (L.DbgReplayOk(4));     // in-window reorder, fresh
    TEST_ASSERT_TRUE (L.DbgReplayOk(100));   // jump forward
    TEST_ASSERT_FALSE(L.DbgReplayOk(100));   // replay at new max
    TEST_ASSERT_FALSE(L.DbgReplayOk(20));    // now > 64 behind -> too old
    TEST_ASSERT_TRUE (L.DbgReplayOk(99));    // just inside window, fresh
}

/**
 * @brief Verify the opaque control nibble (mode-switch/ADR) rides in the
 *        header, survives the AEAD, and reads back as 0 when unset.
 *
 * Also confirms data still flows intact alongside the control bits.
 *
 * @param[in] enc run the check on an encrypted link when true.
 */
static void check_ctrl(bool enc) {
    static link_layer::LinkLayer<1024> A, B;
    A.Init(cfgA(false, enc)); B.Init(cfgB(false, enc));
    uint8_t fr[link_layer::MAXFRAME]; size_t fl;
    // No control set -> peer reads 0.
    A.BeginTurn();
    fl = A.MakePoll(fr, sizeof(fr));
    B.OnRx(fr, fl, 0);
    TEST_ASSERT_EQUAL_UINT8(0, B.CtrlRx());
    // A sets a control nibble -> B sees exactly those bits (CTRL_MASK only).
    A.SetCtrlTx(0x30);
    A.BeginTurn();
    fl = A.MakePoll(fr, sizeof(fr));
    B.OnRx(fr, fl, 0);
    TEST_ASSERT_EQUAL_UINT8(0x30, B.CtrlRx());
    // Low (non-control) bits passed to SetCtrlTx are ignored.
    A.SetCtrlTx(0xF7);
    A.BeginTurn();
    fl = A.MakePoll(fr, sizeof(fr));
    B.OnRx(fr, fl, 0);
    TEST_ASSERT_EQUAL_UINT8(0xF0, B.CtrlRx());
    // Data still flows intact alongside control.
    A.SetCtrlTx(0x10);
    A.Write((const uint8_t*)"hello", 5);
    A.BeginTurn();
    while (A.NextTx(fr, sizeof(fr), fl, 0))
        B.OnRx(fr, fl, 0);
    uint8_t out[16];
    size_t n = B.Read(out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(5, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY("hello", out, 5);
    TEST_ASSERT_EQUAL_UINT8(0x10, B.CtrlRx());
}
/** @brief The control nibble works on a plaintext link. */
void test_ctrl_channel_plain() { check_ctrl(false); }
/** @brief The control nibble works on an encrypted link. */
void test_ctrl_channel_enc()   { check_ctrl(true); }

/**
 * @brief The opaque aux telemetry byte rides the authenticated header,
 *        survives the AEAD, and reads back as 0 before any RX.
 *
 * Mirrors the control-nibble check for the firmware peer-SNR byte (header[9]).
 */
void test_aux_roundtrip() {
    static link_layer::LinkLayer<1024> A, B;
    A.Init(cfgA(false, true)); B.Init(cfgB(false, true));   // encrypted link
    uint8_t fr[link_layer::MAXFRAME]; size_t fl;
    // Default before any received frame is 0.
    TEST_ASSERT_EQUAL_UINT8(0, B.AuxRx());
    // A stamps a telemetry byte -> B reads it back exactly.
    A.SetAuxTx(0x2A);
    A.BeginTurn();
    fl = A.MakePoll(fr, sizeof(fr));
    B.OnRx(fr, fl, 0);
    TEST_ASSERT_EQUAL_UINT8(0x2A, B.AuxRx());
}

/**
 * @brief Flipping the on-wire aux byte makes the AEAD reject the frame,
 *        proving aux is authenticated (tamper-proof) when encrypted.
 *
 * The aux byte at header index 9 is part of the AEAD associated data, so any
 * bit flip there fails the tag check exactly like a flipped header field.
 */
void test_aux_tamper() {
    static link_layer::LinkLayer<1024> A, B;
    A.Init(cfgA(false, true)); B.Init(cfgB(false, true));   // encrypted link
    uint8_t fr[link_layer::MAXFRAME]; size_t fl;
    A.SetAuxTx(0x2A);
    A.BeginTurn();
    fl = A.MakePoll(fr, sizeof(fr));
    fr[9] ^= 1;                       // tamper the authenticated aux byte
    TEST_ASSERT_FALSE(B.OnRx(fr, fl, 0));   // AEAD rejects the whole frame
    TEST_ASSERT_EQUAL_UINT8(0, B.AuxRx());  // nothing was accepted
}

/**
 * @brief Fast retransmit: a gap the SACK bitmap exposes is resent WITHOUT
 *        waiting for retransmit_ms.
 *
 * retransmit_ms is set absurdly high so the timeout can't fire — the dropped
 * frame can only return via SACK-driven fast retransmit.
 */
void test_fast_retransmit() {
    static link_layer::LinkLayer<4096> A, B;
    link_layer::Config ca = cfgA(false, false); ca.retransmit_ms = 100000000u;
    link_layer::Config cb = cfgB(false, false); cb.retransmit_ms = 100000000u;
    A.Init(ca); B.Init(cb);
    auto src = make_payload(5 * link_layer::MAXPAY);
    A.Write(src.data(), src.size());
    uint8_t fr[link_layer::MAXFRAME]; size_t fl, n; uint8_t out[600];
    std::vector<uint8_t> got;
    bool dropped = false;
    for (int turn = 0; turn < 30 && got.size() < src.size(); turn++) {
        A.BeginTurn();                          // now stays 0 throughout
        while (A.NextTx(fr, sizeof(fr), fl, 0)) {
            bool is_data = fr[2] & link_layer::F_DATA;
            if (is_data && fr[3] == 2 && !dropped) { dropped = true; continue; }
            B.OnRx(fr, fl, 0);                  // drop seq 2 exactly once
        }
        B.BeginTurn();
        while (B.NextTx(fr, sizeof(fr), fl, 0)) A.OnRx(fr, fl, 0);
        while ((n = B.Read(out, sizeof(out))) > 0)
            got.insert(got.end(), out, out + n);
    }
    TEST_ASSERT_TRUE(dropped);
    TEST_ASSERT_EQUAL_UINT32(src.size(), got.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src.data(), got.data(), src.size());
}

/**
 * @brief Run the coordinated mode-switch handshake between two ModeSwitch
 *        instances over a simulated mode-deaf PHY.
 *
 * A frame is delivered only if both ends are on the same mode (a mismatch =
 * deaf link); @p drop_ack_rounds ACK frames are lost to exercise
 * probation/revert.
 *
 * @param[in] start           the mode both ends start on.
 * @param[in] target          the mode the initiator requests.
 * @param[in] drop_ack_rounds number of initial rounds whose ACK is lost.
 * @return true if both ends converge, quiescent, on @p target.
 */
static bool run_modeswitch(int start, int target, int drop_ack_rounds) {
    link_layer::ModeSwitch I, R;
    // Probation must exceed the per-round dt so a post-switch frame can clear
    // it before it would (wrongly) revert; small enough that a real revert
    // still happens within the loop.
    I.probation_ms = 250; R.probation_ms = 250;
    I.Begin(start); R.Begin(start);
    I.Request(target);
    int m; uint32_t now = 0;
    for (int round = 0; round < 60; round++, now += 100) {
        I.Poll(now); while (I.TakeApply(&m, now)) {}
        R.Poll(now); while (R.TakeApply(&m, now)) {}
        // Initiator transmits; responder hears it only if modes match.
        uint8_t itx = I.TxCtrl();
        if (I.Current() == R.Current()) R.AfterRecv(itx, false, now);
        while (R.TakeApply(&m, now)) {}          // (REQ doesn't apply yet)
        // Responder replies (ACK rides this frame) on the OLD PHY, then moves.
        uint8_t rtx = R.TxCtrl();
        bool ack_lost = round < drop_ack_rounds;
        if (R.Current() == I.Current() && !ack_lost)
            I.AfterRecv(rtx, true, now);
        R.AfterSend(now);
        while (R.TakeApply(&m, now)) {}
        while (I.TakeApply(&m, now)) {}
        if (I.Current() == target && R.Current() == target &&
            !I.Busy() && !R.Busy())
            return true;
    }
    return false;
}
/**
 * @brief No loss: the two ends negotiate and both land on the target mode.
 */
void test_modeswitch_converge() {
    TEST_ASSERT_TRUE(run_modeswitch(2, 0, 0));
}

/**
 * @brief A lost ACK (only the responder switched) is reverted by probation and
 *        the retry still converges both ends on the target.
 */
void test_modeswitch_ack_loss() {
    TEST_ASSERT_TRUE(run_modeswitch(2, 0, 3));
}

/**
 * @brief One simulated turn over a same-mode-only channel.
 *
 * @param[in,out] I        the initiator's ModeSwitch.
 * @param[in,out] R        the responder's ModeSwitch.
 * @param[in]     now      the current sim time in milliseconds.
 * @param[in]     ack_lost drop the responder's reply, so the initiator never
 *                         learns the switch was accepted.
 */
static void ms_round(link_layer::ModeSwitch& I, link_layer::ModeSwitch& R,
                     uint32_t now, bool ack_lost) {
    int m;
    I.Poll(now); while (I.TakeApply(&m, now)) {}
    R.Poll(now); while (R.TakeApply(&m, now)) {}
    uint8_t itx = I.TxCtrl();
    if (I.Current() == R.Current()) R.AfterRecv(itx, false, now);
    while (R.TakeApply(&m, now)) {}
    uint8_t rtx = R.TxCtrl();
    if (R.Current() == I.Current() && !ack_lost) I.AfterRecv(rtx, true, now);
    R.AfterSend(now);
    while (R.TakeApply(&m, now)) {}
    while (I.TakeApply(&m, now)) {}
}

/**
 * @brief Persistent ACK loss would dead-flap forever; the firmware's
 *        timeout-Abort() must settle BOTH ends back on the original mode.
 *
 * Without Abort the responder keeps switching and reverting while the
 * initiator stays put. Mirrors the hardware bug hit with loss-aware ADR on
 * turbo; asserts there is no permanent desync.
 */
void test_modeswitch_abort_persistent_loss() {
    link_layer::ModeSwitch I, R;
    I.probation_ms = 250; R.probation_ms = 250;
    I.Begin(2); R.Begin(2);
    I.Request(0);
    uint32_t now = 0;
    for (int r = 0; r < 12; r++, now += 100) ms_round(I, R, now, true);
    TEST_ASSERT_EQUAL_INT(2, I.Current());   // initiator never completed
    TEST_ASSERT_TRUE(I.Busy());              // still trying (would dead-flap)
    I.Abort();                               // firmware timeout fires
    for (int r = 0; r < 12; r++, now += 100) ms_round(I, R, now, true);
    TEST_ASSERT_EQUAL_INT(2, I.Current());   // both back on the original mode,
    TEST_ASSERT_EQUAL_INT(2, R.Current());   // no permanent desync
    TEST_ASSERT_FALSE(I.Busy());
    TEST_ASSERT_FALSE(R.Busy());
}

// ===========================================================================
// Mode-switch MECHANISM model-checker (docs/MODE_SWITCH_SPEC.md).
//
// Two ends over a MODE-DEAF channel: a control frame is heard only if the peer
// is on the SENDER's mode at send time (LoRa SF/BW or LoRa<->GFSK mismatch =
// deaf). An adversary may drop either direction every round. The dead-link
// RENDEZVOUS backstop (no valid frame for N rounds -> fall back to a fixed
// mode) is modeled too, exactly as the firmware does it. We EXHAUSTIVELY
// enumerate loss patterns (+ reboots, + up/down requests) and assert the ends
// always re-converge to the SAME mode once the channel heals — no interleaving
// leads to a permanent split. This is the deterministic search for holes that
// hardware testing couldn't do.
// ===========================================================================
/// @brief One end in the mode-switch model-checker: its state machine plus the
///        silence clock that triggers its rendezvous backstop.
struct MsmNode {
    link_layer::ModeSwitch ms;   ///< this node's mode-switch state machine.
    int last_heard = 0;          ///< round index of the last frame we received.
    /**
     * @brief (Re)start this node on @p mode and reset its silence clock.
     *
     * @param[in] mode the mode index to (re)start on.
     */
    void begin(int mode) { ms.Begin(mode); last_heard = 0; }
};

/// Rendezvous fallback mode for the model-checker (cf. 'medium').
static const int      kMsmRzMode    = 2;
/// No-hear rounds before a node falls back to the rendezvous mode.
static const int      kMsmRzRounds  = 9;
/// Mode-switch probation window, in ms (scaled for the round clock).
static const uint32_t kMsmProbation = 300;

/**
 * @brief One faithful half-duplex exchange + rendezvous backstop.
 *
 * The responder applies its switch only AFTER transmitting its ACK
 * (make-before-break), so the ACK leaves on the old mode; that ordering creates
 * the deaf window the protocol must survive.
 *
 * @param[in,out] I          the initiator node.
 * @param[in,out] R          the responder node.
 * @param[in]     round      the current round index.
 * @param[in]     now        the current sim time in milliseconds.
 * @param[in]     dropI2R    drop the initiator->responder frame this round.
 * @param[in]     dropR2I    drop the responder->initiator frame this round.
 * @param[in]     rendezvous allow the dead-link rendezvous backstop to fire.
 */
static void msm_step(MsmNode& I, MsmNode& R, int round, uint32_t now,
                     bool dropI2R, bool dropR2I, bool rendezvous = true) {
    int m;
    I.ms.Poll(now); while (I.ms.TakeApply(&m, now)) {}
    R.ms.Poll(now); while (R.ms.TakeApply(&m, now)) {}
    uint8_t itx = I.ms.TxCtrl();                       // initiator transmits
    if (!dropI2R && I.ms.Current() == R.ms.Current()) {
        R.ms.AfterRecv(itx, false, now); R.last_heard = round;
    }
    while (R.ms.TakeApply(&m, now)) {}
    uint8_t rtx = R.ms.TxCtrl();                  // responder replies (old)
    if (!dropR2I && R.ms.Current() == I.ms.Current()) {
        I.ms.AfterRecv(rtx, true, now); I.last_heard = round;
    }
    R.ms.AfterSend(now);                               // ...then it switches
    while (R.ms.TakeApply(&m, now)) {}
    while (I.ms.TakeApply(&m, now)) {}
    // Dead-link rendezvous (firmware MaybeRendezvous): heard nothing valid
    // for a while -> fall back to the fixed mode. Both do it independently.
    if (!rendezvous) return;
    if (round - I.last_heard >= kMsmRzRounds && I.ms.Current() != kMsmRzMode) {
        I.begin(kMsmRzMode); I.last_heard = round;
    }
    if (round - R.last_heard >= kMsmRzRounds && R.ms.Current() != kMsmRzMode) {
        R.begin(kMsmRzMode); R.last_heard = round;
    }
}

/**
 * @brief Run one model-checker scenario and report whether the ends end
 *        QUIESCENT on the SAME mode.
 *
 * @param[in] start        the mode both ends start on.
 * @param[in] target       the mode the initiator first requests.
 * @param[in] pattern      packs 2 drop bits per round (I2R, R2I).
 * @param[in] nrounds      number of lossy rounds before the heal phase.
 * @param[in] reboot_round round at which to reboot a node (-1 = none).
 * @param[in] reboot_who   which node reboots (0 = initiator, 1 = responder).
 * @param[in] target2      a 2nd requested mode mid-run (-1 = none).
 * @param[in] rendezvous   allow the rendezvous backstop to fire.
 * @return true if both ends converge, quiescent, on the same mode.
 */
static bool msm_scenario(int start, int target, uint32_t pattern, int nrounds,
                         int reboot_round, int reboot_who, int target2,
                         bool rendezvous = true) {
    MsmNode I, R;
    I.ms.probation_ms = kMsmProbation; R.ms.probation_ms = kMsmProbation;
    I.begin(start); R.begin(start);
    I.ms.Request(target);
    uint32_t now = 0; int round = 0;
    for (; round < nrounds; round++, now += 100) {
        if (round == reboot_round) {
            (reboot_who ? R : I).begin(start);     // reboot -> loads base mode
        }
        if (round == nrounds / 2 && target2 >= 0) I.ms.Request(target2);
        bool d1 = (pattern >> (2 * round)) & 1u;
        bool d2 = (pattern >> (2 * round + 1)) & 1u;
        msm_step(I, R, round, now, d1, d2, rendezvous);
    }
    // Heal: deliver everything; bounded rounds to reach quiescent + converged.
    for (int k = 0; k < 60 && (I.ms.Busy() || R.ms.Busy() ||
                               I.ms.Current() != R.ms.Current());
         k++, round++, now += 100) {
        msm_step(I, R, round, now, false, false, rendezvous);
    }
    return !I.ms.Busy() && !R.ms.Busy() && I.ms.Current() == R.ms.Current();
}

/**
 * @brief Exhaustive over all 4^6 loss patterns for a switch UP and DOWN.
 */
void test_msm_exhaustive_loss() {
    const int D = 6;
    for (uint32_t p = 0; p < (1u << (2 * D)); p++) {
        if (!msm_scenario(0, 1, p, D, -1, 0, -1)) {
            char b[80]; snprintf(b, sizeof b, "UP pattern %u no converge", p);
            TEST_FAIL_MESSAGE(b); return;
        }
        if (!msm_scenario(3, 1, p, D, -1, 0, -1)) {
            char b[80]; snprintf(b, sizeof b, "DOWN pattern %u no converge", p);
            TEST_FAIL_MESSAGE(b); return;
        }
    }
}

/**
 * @brief A switch into a MODE-DEAF (GFSK-like) peer converges under all loss
 *        patterns.
 *
 * Mode 5 is reachable but a node on it can't hear a node on any other mode.
 */
void test_msm_exhaustive_gfsk() {
    const int D = 6;
    for (uint32_t p = 0; p < (1u << (2 * D)); p++) {
        if (!msm_scenario(0, 5, p, D, -1, 0, -1)) {
            char b[80]; snprintf(b, sizeof b, "GFSK pattern %u no converge", p);
            TEST_FAIL_MESSAGE(b); return;
        }
    }
}

/**
 * @brief Reboot at every round, either node, across sampled loss patterns.
 */
void test_msm_reboot_midswitch() {
    const int D = 6;
    for (int rr = 0; rr < D; rr++)
        for (int who = 0; who < 2; who++)
            for (uint32_t p = 0; p < (1u << (2 * D)); p += 7) {  // sample
                if (!msm_scenario(0, 1, p, D, rr, who, -1)) {
                    char b[80];
                    snprintf(b, sizeof b, "reboot r%d who%d p%u", rr, who, p);
                    TEST_FAIL_MESSAGE(b); return;
                }
            }
}

/**
 * @brief Teeth: the rendezvous backstop is load-bearing for reboot
 *        interleavings.
 *
 * The make-before-break handshake + probation alone recovers all loss-only
 * interleavings, but a reboot clears the in-flight switch state, so neither
 * end's probation is armed and they can be stranded on different modes forever.
 * Asserts (a) such an interleaving exists and strands WITHOUT rendezvous
 * (proving the checker finds real holes), and (b) the same interleavings all
 * recover WITH rendezvous.
 */
void test_msm_rendezvous_is_required() {
    const int D = 6;
    bool stranded_without = false, all_recover_with = true;
    for (int rr = 0; rr < D; rr++)
        for (int who = 0; who < 2; who++)
            for (uint32_t p = 0; p < (1u << (2 * D)); p += 5) {  // sample
                if (!msm_scenario(0, 1, p, D, rr, who, -1, /*rz=*/false))
                    stranded_without = true;
                if (!msm_scenario(0, 1, p, D, rr, who, -1, /*rz=*/true))
                    all_recover_with = false;
            }
    TEST_ASSERT_TRUE_MESSAGE(stranded_without,
        "expected a reboot interleaving to strand without rendezvous");
    TEST_ASSERT_TRUE_MESSAGE(all_recover_with,
        "rendezvous must recover every reboot interleaving");
}

/**
 * @brief Two switches in flight (up then down) converge under all patterns.
 */
void test_msm_double_switch() {
    const int D = 6;
    for (uint32_t p = 0; p < (1u << (2 * D)); p++) {
        if (!msm_scenario(0, 1, p, D, -1, 0, 4)) {
            char b[80]; snprintf(b, sizeof b, "double p%u no converge", p);
            TEST_FAIL_MESSAGE(b); return;
        }
    }
}

// ===========================================================================
// ADR (Adaptive Data Rate) decision logic — lib/linklayer/adr.h.
// A test mode ladder mirroring the firmware's real table: 6 presets, the GFSK
// 'ludicrous' rung last (index 5). Indices: turbo0 fast1 medium2 slow3 far4.
// ===========================================================================
/// Per-rung SNR demod floor (dB) for the test ADR ladder: turbo, fast, medium,
/// slow, far, ludicrous(GFSK). GFSK is RSSI-gated, so its floor is unused (0).
static const float kAdrFloor[6] = {-5.0f, -7.5f, -7.5f, -12.5f, -20.0f, 0.0f};
/// Per-rung GFSK flag for the test ADR ladder (true only for the top rung).
static const bool  kAdrFsk[6]   = {false, false, false, false, false, true};

/**
 * @brief Build the test ADR ladder (6 presets, GFSK rung last) with a 6 dB
 *        margin.
 *
 * @return the populated AdrLadder.
 */
static link_layer::AdrLadder adr_ladder() {
    link_layer::AdrLadder L;
    L.count = 6; L.snr_floor = kAdrFloor; L.is_fsk = kAdrFsk;
    L.margin_db = 6.0f;
    return L;
}

/**
 * @brief Build an ADR controller with short timers (cheap fake-clock tests).
 *
 * @param[in] up_stable consecutive good evals required to step UP.
 * @param[in] gfsk      enable the GFSK 'ludicrous' top rung opt-in.
 * @return the Begin()-ed AdrController.
 */
static link_layer::AdrController adr_ctrl(int up_stable, bool gfsk) {
    link_layer::AdrController c;
    c.ladder = adr_ladder();
    c.cfg.period_ms = 100;
    c.cfg.up_stable = up_stable;
    c.cfg.down_retx_pct = 25;
    c.cfg.up_max_retx_pct = 8;
    c.cfg.gfsk_up_rssi = -70.0f;
    c.cfg.switch_timeout_ms = 500;
    c.cfg.cooldown_ms = 1000;
    c.cfg.gfsk_enabled = gfsk;
    c.Begin();
    return c;
}
/**
 * @brief Build an ADR decision input struct.
 *
 * @param[in] now  the current time in milliseconds.
 * @param[in] busy true if a mode switch is in flight.
 * @param[in] cur  the current mode index.
 * @param[in] snr  the link SNR in dB.
 * @param[in] retx the retransmit percentage.
 * @param[in] rssi the link RSSI in dBm (GFSK gate).
 * @return the populated AdrController::In.
 */
static link_layer::AdrController::In adr_in(uint32_t now, bool busy, int cur,
                                    float snr, int retx, float rssi = -50.0f) {
    link_layer::AdrController::In in;
    in.now = now; in.busy = busy; in.have_link = true;
    in.cur = cur; in.snr = snr; in.rssi = rssi; in.retx_pct = retx;
    return in;
}

/**
 * @brief The pure ladder math: speed ordering, SNR thresholds, robustness.
 */
void test_adr_ladder() {
    link_layer::AdrLadder L = adr_ladder();
    TEST_ASSERT_EQUAL_INT(0, L.FastestLora());      // turbo
    TEST_ASSERT_EQUAL_INT(4, L.LastLora());         // far
    TEST_ASSERT_EQUAL_INT(5, L.Ludicrous());        // GFSK rung
    TEST_ASSERT_TRUE(L.IsFsk(5));
    TEST_ASSERT_FALSE(L.IsFsk(0));
    TEST_ASSERT_TRUE(L.Faster(0, 4));               // turbo faster than far
    TEST_ASSERT_TRUE(L.Faster(5, 0));               // GFSK fastest of all
    TEST_ASSERT_FALSE(L.Faster(4, 0));
    // SNR picks the fastest LoRa preset whose floor+margin it clears.
    TEST_ASSERT_EQUAL_INT(0, L.PickBySnr(100.0f));  // clears turbo (-5+6=1)
    TEST_ASSERT_EQUAL_INT(1, L.PickBySnr(0.0f));    // clears fast, not turbo
    TEST_ASSERT_EQUAL_INT(4, L.PickBySnr(-100.0f)); // too weak -> robust LoRa
    TEST_ASSERT_EQUAL_INT(1, L.MoreRobust(0));      // turbo -> fast
    TEST_ASSERT_EQUAL_INT(4, L.MoreRobust(4));      // far stays far
    TEST_ASSERT_EQUAL_INT(0, L.MoreRobust(5));      // GFSK -> turbo
}

/**
 * @brief No ADR action fires until the re-evaluation cadence elapses.
 */
void test_adr_cadence_gate() {
    link_layer::AdrController c = adr_ctrl(3, false);
    // High loss would normally force a step-down, but it's too soon.
    link_layer::AdrAction a = c.Decide(adr_in(50, false, 2, 5.0f, 30));
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_NONE, a.kind);
    a = c.Decide(adr_in(200, false, 2, 5.0f, 30));  // cadence elapsed
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_REQUEST, a.kind);
}

/**
 * @brief A high retransmit rate steps to a more robust mode at once.
 */
void test_adr_loss_steps_down() {
    link_layer::AdrController c = adr_ctrl(3, false);
    link_layer::AdrAction a = c.Decide(adr_in(1000, false, 2, 5.0f, 30));
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_REQUEST, a.kind);
    TEST_ASSERT_EQUAL_INT(3, a.mode);               // medium -> slow
}

/**
 * @brief With SNR allowing a faster mode but loss moderate, ADR does NOT
 *        climb.
 */
void test_adr_no_climb_into_loss() {
    link_layer::AdrController c = adr_ctrl(3, false);
    for (uint32_t t = 1000; t <= 2000; t += 200) {
        // far, strong SNR (would pick turbo) but retx 15 (between the marks)
        link_layer::AdrAction a = c.Decide(adr_in(t, false, 4, 100.0f, 15));
        TEST_ASSERT_EQUAL_INT(link_layer::ADR_NONE, a.kind);
    }
}

/**
 * @brief Stepping UP needs a good read for up_stable consecutive evals.
 */
void test_adr_step_up_hysteresis() {
    link_layer::AdrController c = adr_ctrl(3, false);
    link_layer::AdrAction a = c.Decide(adr_in(1000, false, 4, 100.0f, 0));
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_NONE, a.kind);    // 1/3
    a = c.Decide(adr_in(1100, false, 4, 100.0f, 0));
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_NONE, a.kind);    // 2/3
    a = c.Decide(adr_in(1200, false, 4, 100.0f, 0));
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_REQUEST, a.kind); // 3/3 -> commit
    TEST_ASSERT_EQUAL_INT(0, a.mode);               // jump straight to turbo
}

/**
 * @brief Weak SNR commits a step DOWN immediately, with no hysteresis.
 */
void test_adr_snr_down_immediate() {
    link_layer::AdrController c = adr_ctrl(3, false);
    link_layer::AdrAction a = c.Decide(adr_in(1000, false, 0, -100.0f, 0));
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_REQUEST, a.kind);
    TEST_ASSERT_EQUAL_INT(4, a.mode);               // turbo -> far
}

/**
 * @brief The GFSK top rung is reached from turbo only when enabled + strong
 *        SNR + low loss.
 */
void test_adr_gfsk_gate() {
    link_layer::AdrController on = adr_ctrl(1, true);
    link_layer::AdrAction a = on.Decide(adr_in(1000, false, 0, 15.0f, 0));
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_REQUEST, a.kind);
    TEST_ASSERT_EQUAL_INT(5, a.mode);               // -> ludicrous (GFSK)
    // Same inputs but the opt-in is off: turbo is already the SNR pick -> stay.
    link_layer::AdrController off = adr_ctrl(1, false);
    a = off.Decide(adr_in(1000, false, 0, 15.0f, 0));
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_NONE, a.kind);
}

/// Per-rung 'auto-OK' flag: which rungs 'auto' ADR may pick (turbo is
/// manual-only, so index 0 is false).
static const bool kAdrAutoOk[6] = {false, true, true, true, true, true};

/**
 * @brief 'auto' must NOT pick a manual-only rung (turbo).
 *
 * Turbo is excluded from the SNR ladder, and the GFSK top rung is reached from
 * the fastest auto rung instead of turbo.
 */
void test_adr_auto_excludes_turbo() {
    link_layer::AdrLadder L = adr_ladder();
    L.auto_ok = kAdrAutoOk;                 // turbo (index 0) manual-only
    TEST_ASSERT_EQUAL_INT(1, L.FastestAutoLora());   // fast, not turbo
    TEST_ASSERT_EQUAL_INT(1, L.PickBySnr(100.0f));   // never climbs to turbo
    TEST_ASSERT_EQUAL_INT(1, L.MoreRobust(5));       // GFSK falls back to fast
    // From 'fast', a strong link + GFSK opt-in jumps to ludicrous (the gate now
    // keys off the fastest *auto* rung, not turbo).
    link_layer::AdrController c = adr_ctrl(1, true);
    c.ladder = L;
    link_layer::AdrAction a = c.Decide(adr_in(1000, false, 1, 15.0f, 0));
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_REQUEST, a.kind);
    TEST_ASSERT_EQUAL_INT(5, a.mode);                // -> ludicrous from fast
}

/**
 * @brief The GFSK rung is gated on RSSI, not SNR, so a weak RSSI must NOT
 *        trigger GFSK even at high SNR.
 *
 * LoRa SNR saturates ~+11 dB and can't signal "very strong"; RSSI can.
 */
void test_adr_gfsk_gated_on_rssi() {
    link_layer::AdrController c = adr_ctrl(1, true);
    // turbo (fastest auto), strong SNR but weak RSSI (-90 < -70 gate): no GFSK.
    link_layer::AdrAction a =
        c.Decide(adr_in(1000, false, 0, 11.0f, 0, -90.0f));
    // SNR pick == turbo == cur, so no action.
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_NONE, a.kind);
    // Same but strong RSSI: GFSK fires.
    link_layer::AdrController c2 = adr_ctrl(1, true);
    a = c2.Decide(adr_in(1000, false, 0, 11.0f, 0, -55.0f));
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_REQUEST, a.kind);
    TEST_ASSERT_EQUAL_INT(5, a.mode);
}

/**
 * @brief On GFSK there is no usable SNR, so ADR is loss-down only.
 */
void test_adr_gfsk_down_only() {
    link_layer::AdrController c = adr_ctrl(1, true);
    link_layer::AdrAction a =
        c.Decide(adr_in(1000, false, 5, 0.0f, 0));  // low loss
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_NONE, a.kind);
    a = c.Decide(adr_in(1100, false, 5, 0.0f, 30));               // high loss
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_REQUEST, a.kind);
    TEST_ASSERT_EQUAL_INT(0, a.mode);               // GFSK -> turbo
}

/**
 * @brief A switch that never completes (busy past the timeout) triggers Abort
 *        + cooldown, and no new request fires until the cooldown elapses.
 */
void test_adr_abort_when_stuck() {
    link_layer::AdrController c = adr_ctrl(3, false);
    link_layer::AdrAction a =
        c.Decide(adr_in(1000, false, 2, 5.0f, 30));  // ask: slow
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_REQUEST, a.kind);
    a = c.Decide(adr_in(1200, true, 2, 5.0f, 30));   // in flight, pre-timeout
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_NONE, a.kind);
    a = c.Decide(adr_in(1600, true, 2, 5.0f, 30));   // 600ms > 500ms timeout
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_ABORT, a.kind);
    a = c.Decide(adr_in(1700, false, 2, 5.0f, 30));  // cooling down -> nothing
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_NONE, a.kind);
    a = c.Decide(adr_in(2700, false, 2, 5.0f, 30));  // cooldown done -> retry
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_REQUEST, a.kind);
}

/**
 * @brief Flap guard: after a rendezvous fallback, ADR must NOT immediately
 *        re-climb into the mode that just died.
 *
 * Models the real bug: 'auto' climbs to a fast rung, that rung goes deaf, the
 * firmware rendezvous-falls-back to a robust mode and calls OnFallback(). With
 * strong SNR the SNR pick is the dead rung again — absent the penalty cap ADR
 * would re-request it at once and the link flaps. After OnFallback the target
 * must instead be more robust than the failed mode until fallback_penalty_ms
 * elapses, then the failed mode becomes eligible again. (FAILS without the
 * Decide() penalty cap: the request would target the failed mode.)
 */
void test_adr_fallback_penalizes_mode() {
    link_layer::AdrController c = adr_ctrl(1, false);  // up_stable=1: commits
    c.cfg.fallback_penalty_ms = 5000;       // short for the fake clock
    const int kFailed = 0;                  // turbo: the dead fast rung
    c.OnFallback(1000, kFailed);   // cooldown -> 2000; penalty -> 6000

    // Strong SNR (would pick turbo) from a robust mode, low loss — without the
    // guard this climbs straight back to turbo. now is past both the cooldown
    // and the cadence, and still inside the penalty window.
    link_layer::AdrAction a = c.Decide(adr_in(2100, false, 2, 100.0f, 0));
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_REQUEST, a.kind);
    TEST_ASSERT_NOT_EQUAL(kFailed, a.mode);            // NOT the dead rung
    link_layer::AdrLadder L = adr_ladder();
    TEST_ASSERT_TRUE(L.Rank(a.mode) < L.Rank(kFailed)); // more robust than it

    // Past the penalty window: the failed mode is eligible again.
    a = c.Decide(adr_in(7000, false, 2, 100.0f, 0));
    TEST_ASSERT_EQUAL_INT(link_layer::ADR_REQUEST, a.kind);
    TEST_ASSERT_EQUAL_INT(kFailed, a.mode);            // may re-pick turbo now
}

// ===========================================================================
// Per-session key handshake (forward secrecy) — lib/linklayer/session.h.
// ===========================================================================
/**
 * @brief Run a full two-message session handshake over a lossless channel and
 *        copy out both ends' derived session keys.
 *
 * @param[in]  static_i the initiator's paired (static) key.
 * @param[in]  static_r the responder's paired (static) key.
 * @param[in]  ipriv    the initiator's ephemeral private key.
 * @param[in]  rpriv    the responder's ephemeral private key.
 * @param[out] ikey     the initiator's derived 16-byte session key.
 * @param[out] rkey     the responder's derived 16-byte session key.
 */
static void session_run(const uint8_t static_i[16], const uint8_t static_r[16],
                        const uint8_t ipriv[32], const uint8_t rpriv[32],
                        uint8_t ikey[16], uint8_t rkey[16]) {
    link_layer::SessionHandshake I, R;
    I.Begin(static_i, true, ipriv);
    R.Begin(static_r, false, rpriv);
    uint8_t ipub[32], rpub[32];
    memcpy(ipub, I.MyPublic(), 32);
    memcpy(rpub, R.MyPublic(), 32);
    R.OnPeerPublic(ipub);   // responder hears the initiator
    I.OnPeerPublic(rpub);   // initiator hears the responder
    memcpy(ikey, I.SessionKey(), 16);
    memcpy(rkey, R.SessionKey(), 16);
}

/**
 * @brief Both ends sharing the paired key derive the SAME session key.
 */
void test_session_handshake_agreement() {
    uint8_t ipriv[32], rpriv[32];
    memset(ipriv, 0x11, 32); memset(rpriv, 0x22, 32);
    uint8_t ik[16], rk[16];
    session_run(KEY, KEY, ipriv, rpriv, ik, rk);
    TEST_ASSERT_EQUAL_MEMORY(ik, rk, 16);
}

/**
 * @brief Forward secrecy: fresh ephemerals (same paired key) yield a different
 *        session key, so a leaked paired key can't rederive a past one.
 */
void test_session_forward_secrecy() {
    uint8_t a1[32], b1[32], a2[32], b2[32];
    memset(a1, 1, 32); memset(b1, 2, 32); memset(a2, 3, 32); memset(b2, 4, 32);
    uint8_t k1i[16], k1r[16], k2i[16], k2r[16];
    session_run(KEY, KEY, a1, b1, k1i, k1r);
    session_run(KEY, KEY, a2, b2, k2i, k2r);
    TEST_ASSERT_EQUAL_MEMORY(k1i, k1r, 16);          // each session agrees
    TEST_ASSERT_EQUAL_MEMORY(k2i, k2r, 16);
    TEST_ASSERT_TRUE(memcmp(k1i, k2i, 16) != 0);     // differ across sessions
}

/**
 * @brief Mutual auth: a peer WITHOUT the paired key derives a different session
 *        key, so the first AEAD frame won't open (no data to an impostor).
 */
void test_session_wrong_static_key_fails_aead() {
    uint8_t key2[16];
    memcpy(key2, KEY, 16); key2[0] ^= 0xFF;          // attacker's wrong key
    uint8_t ipriv[32], rpriv[32];
    memset(ipriv, 0x33, 32); memset(rpriv, 0x44, 32);
    uint8_t ik[16], rk[16];
    session_run(KEY, key2, ipriv, rpriv, ik, rk);
    TEST_ASSERT_TRUE(memcmp(ik, rk, 16) != 0);       // keys disagree

    // Initiator encrypts a frame under its key; the wrong-key responder can't
    // open it.
    const uint8_t npub[16] = {0};
    uint8_t pt[8] = {1,2,3,4,5,6,7,8}, ct[8], tag[16], out[8];
    link_layer::Ascon128Encrypt(ik, npub, nullptr, 0, pt, 8, ct, tag);
    bool ok =
        link_layer::Ascon128Decrypt(rk, npub, nullptr, 0, ct, 8, tag, 16, out);
    TEST_ASSERT_FALSE(ok);
}

/**
 * @brief A genuine session AEAD round-trips: encrypt under the initiator key,
 *        decrypt under the responder's (they match), recovering plaintext.
 */
void test_session_aead_roundtrip() {
    uint8_t ipriv[32], rpriv[32];
    memset(ipriv, 0x55, 32); memset(rpriv, 0x66, 32);
    uint8_t ik[16], rk[16];
    session_run(KEY, KEY, ipriv, rpriv, ik, rk);
    const uint8_t npub[16] = {9};
    uint8_t pt[5] = {'h','e','l','l','o'}, ct[5], tag[16], out[5];
    link_layer::Ascon128Encrypt(ik, npub, nullptr, 0, pt, 5, ct, tag);
    bool ok =
        link_layer::Ascon128Decrypt(rk, npub, nullptr, 0, ct, 5, tag, 16, out);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_MEMORY(pt, out, 5);
}

/**
 * @brief Handshake integrity: a MITM tampering with an ephemeral public in
 *        transit changes the derived key, so the two ends disagree.
 */
void test_session_tampered_public() {
    uint8_t ipriv[32], rpriv[32];
    memset(ipriv, 0x77, 32); memset(rpriv, 0x88, 32);
    link_layer::SessionHandshake I, R;
    I.Begin(KEY, true, ipriv);
    R.Begin(KEY, false, rpriv);
    uint8_t ipub[32], rpub[32];
    memcpy(ipub, I.MyPublic(), 32);
    memcpy(rpub, R.MyPublic(), 32);
    rpub[0] ^= 0x01;                 // flip a bit in the responder's public
    R.OnPeerPublic(ipub);
    I.OnPeerPublic(rpub);            // initiator saw the tampered key
    TEST_ASSERT_TRUE(memcmp(I.SessionKey(), R.SessionKey(), 16) != 0);
}

/**
 * @brief Reboot must NOT reuse a nonce: after restart the counter resumes from
 *        a persisted+bumped value, exceeding every pre-reboot counter.
 */
void test_aead_reboot_nonce_skip() {
    static uint64_t saved; saved = 0;
    struct H { static void cb(void* a, uint64_t c) { *(uint64_t*)a = c; } };
    static link_layer::LinkLayer<1024> A;
    link_layer::Config ca = cfgA(false, true);
    ca.persist_stride = 4; ca.persist_arg = &saved; ca.persist_cb = H::cb;
    A.Init(ca);
    uint8_t fr[link_layer::MAXFRAME]; size_t fl;
    for (int i = 0; i < 20; i++) {
        A.Write((const uint8_t*)"x", 1);
        A.BeginTurn(); A.NextTx(fr, sizeof(fr), fl, 0);
    }
    uint64_t before = A.DbgTxCtr();
    TEST_ASSERT_TRUE(saved > 0);              // persisted along the way
    // Simulate reboot: reload from saved counter, pre-bumped by the stride.
    link_layer::Config cr = cfgA(false, true);
    cr.start_ctr = saved + cr.persist_stride; cr.epoch = 2;
    A.Init(cr);
    A.Write((const uint8_t*)"y", 1);
    A.BeginTurn(); A.NextTx(fr, sizeof(fr), fl, 0);
    // never reuses a pre-reboot counter
    TEST_ASSERT_TRUE(A.DbgTxCtr() > before);
}

/**
 * @brief Incompressible (random) data with compression ENABLED still streams
 *        byte-exact.
 *
 * heatshrink expands such data; the encoder must fall back to raw rather than
 * truncate an oversized compressed length into uint8_t (which corrupted the
 * stream on hardware).
 */
void test_stream_incompressible_comp() {
    static link_layer::LinkLayer<16384> A, B;
    A.Init(cfgA(true, false));        // compression ON
    B.Init(cfgB(true, false));
    size_t total = 4000;
    std::vector<uint8_t> src(total);
    uint32_t r = 99;
    for (size_t i = 0; i < total; i++) {
        r = r * 1664525u + 1013904223u; src[i] = (uint8_t)(r >> 16);
    }
    static link_layer::LinkLayer<16384>* pa = &A; (void)pa;
    // stream A->B
    Result res; res.rounds = 0;
    size_t sent = 0; uint32_t now = 0;
    uint8_t buf[link_layer::MAXFRAME]; size_t len; uint8_t out[512];
    std::vector<uint8_t> got;
    for (int round = 0; round < 100000 && got.size() < total; round++) {
        sent += A.Write(src.data() + sent, src.size() - sent);
        A.BeginTurn();
        while (A.NextTx(buf, sizeof(buf), len, now)) B.OnRx(buf, len, now);
        size_t k;
        while ((k = B.Read(out, sizeof(out))) > 0)
            for (size_t i = 0; i < k; i++) got.push_back(out[i]);
        B.BeginTurn();
        while (B.NextTx(buf, sizeof(buf), len, now)) A.OnRx(buf, len, now);
        now += 150;
    }
    TEST_ASSERT_EQUAL_UINT32(total, got.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src.data(), got.data(), total);
}

/**
 * @brief A window=4 link clears 10 KB in far fewer round-trips than
 *        stop-and-wait.
 */
void test_windowing_efficiency() {
    // 10KB lossless with window=4 should take far fewer round-trips than the
    // ~44 a stop-and-wait (1 frame/round at 230B) would need.
    Result r = stream(cfgA(false, false), cfgB(false, false), 10000, 0, 1);
    TEST_ASSERT_EQUAL_UINT32(10000, r.got.size());
    printf("  [10KB lossless took %d round-trips]\n", r.rounds);
    // ~11 expected (4x230/round)
    TEST_ASSERT_LESS_THAN(20, r.rounds);
}

/**
 * @brief Basics: a bigger BDP window is STRICTLY faster (fewer round-trips) for
 *        the same data on a clean channel.
 *
 * "turbo" (window 16) beats "medium" (window 8) in the link layer itself. If
 * hardware shows turbo slower than medium, the cause is radio/timing, not this
 * code. The deterministic check for a per-mode throughput regression.
 */
void test_basics_window_ordering() {
    const size_t total = 60000;
    auto src = make_payload(total);
    int win[] = {2, 4, 8, 16}; int rounds[4];
    for (int i = 0; i < 4; i++) {
        link_layer::Config ca = cfgA(false, true); ca.window = win[i];
        link_layer::Config cb = cfgB(false, true); cb.window = win[i];
        Result r = stream(ca, cb, total, 0, 1);
        TEST_ASSERT_EQUAL_UINT32(total, r.got.size());
        TEST_ASSERT_EQUAL_UINT8_ARRAY(src.data(), r.got.data(), total);
        rounds[i] = r.rounds;
    }
    printf("  [window 2/4/8/16 round-trips: %d/%d/%d/%d]\n",
           rounds[0], rounds[1], rounds[2], rounds[3]);
    TEST_ASSERT_TRUE_MESSAGE(rounds[3] < rounds[2], "w16 not faster than w8");
    TEST_ASSERT_TRUE_MESSAGE(rounds[2] < rounds[1], "w8 not faster than w4");
    TEST_ASSERT_TRUE_MESSAGE(rounds[1] < rounds[0], "w4 not faster than w2");
}

/**
 * @brief Basics: a 0.5 MB compressed+encrypted transfer arrives byte-exact,
 *        and a 100 KB transfer survives 15% loss byte-exact.
 *
 * Confirms no large-data / backlog bug in the link code.
 */
void test_basics_large_transfer() {
    const size_t big = 500000;
    auto src = make_payload(big);
    link_layer::Config ca = cfgA(true, true); ca.window = 16;
    link_layer::Config cb = cfgB(true, true); cb.window = 16;
    Result r = stream(ca, cb, big, 0, 7);
    TEST_ASSERT_EQUAL_UINT32(big, r.got.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src.data(), r.got.data(), big);
    auto src2 = make_payload(100000);
    Result r2 = stream(ca, cb, 100000, 15, 7);
    TEST_ASSERT_EQUAL_UINT32(100000, r2.got.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src2.data(), r2.got.data(), 100000);
}

/**
 * @brief Reproduce the reconnect-desync (one side reboots while the other keeps
 *        state) and verify the epoch handshake resyncs them.
 */
void test_reconnect_resync() {
    static link_layer::LinkLayer<8192> A, B;
    link_layer::Config ca = cfgA(false, false); ca.epoch = 10;
    link_layer::Config cb = cfgB(false, false); cb.epoch = 20;
    A.Init(ca); B.Init(cb);
    uint8_t fr[link_layer::MAXFRAME]; size_t fl; uint8_t out[64];
    auto pump = [&](link_layer::LinkLayer<8192>& X,
                    link_layer::LinkLayer<8192>& Y) {
        X.BeginTurn();
        while (X.NextTx(fr, sizeof(fr), fl, 0)) Y.OnRx(fr, fl, 0);
    };
    A.Write((const uint8_t*)"hello", 5);
    for (int i = 0; i < 6; i++) { pump(A, B); pump(B, A); }
    // B got "hello", B.rxNext now advanced
    TEST_ASSERT_EQUAL_UINT32(5, B.Read(out, sizeof(out)));

    // A "reboots": re-init with a NEW epoch and fresh seq. B still has stale
    // state.
    ca.epoch = 11; A.Init(ca);
    A.Write((const uint8_t*)"world", 5);
    for (int i = 0; i < 8; i++) { pump(A, B); pump(B, A); }
    size_t n = B.Read(out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(5, n);              // resynced -> got "world"
    TEST_ASSERT_EQUAL_UINT8_ARRAY("world", out, 5);
}

/**
 * @brief Selective-Repeat: frames delivered OUT OF ORDER are buffered and
 *        reassembled in order (Go-Back-N would have discarded them).
 */
void test_sr_reordering() {
    static link_layer::LinkLayer<8192> A, B;
    A.Init(cfgA(false, false)); B.Init(cfgB(false, false));
    size_t total = 3 * link_layer::MAXPAY;
    auto src = make_payload(total);
    A.Write(src.data(), total);
    static uint8_t frames[8][link_layer::MAXFRAME]; size_t flens[8]; int nf = 0;
    A.BeginTurn();
    size_t fl;
    while (nf < 8 && A.NextTx(frames[nf], link_layer::MAXFRAME, fl, 0)) {
        flens[nf] = fl; nf++;
    }
    TEST_ASSERT_EQUAL_INT(3, nf);
    // reverse order
    for (int i = nf - 1; i >= 0; i--) B.OnRx(frames[i], flens[i], 0);
    std::vector<uint8_t> got; uint8_t out[1024]; size_t k;
    while ((k = B.Read(out, sizeof(out))) > 0)
        got.insert(got.end(), out, out + k);
    TEST_ASSERT_EQUAL_UINT32(total, got.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src.data(), got.data(), total);
}

/**
 * @brief Build a plaintext link Config with an explicit window size.
 *
 * @param[in] a      this end's 1-byte address.
 * @param[in] p      the peer's 1-byte address.
 * @param[in] window the SR window size.
 * @return the assembled Config.
 */
static link_layer::Config cfgW(uint8_t a, uint8_t p, uint8_t window) {
    link_layer::Config c; c.addr = a; c.peer = p; c.window = window;
    c.retransmit_ms = 100; return c;
}

/**
 * @brief A BDP-sized window (16) clears 10 KB in far fewer round-trips.
 */
void test_sr_window16_efficiency() {
    Result r = stream(cfgW(1, 2, 16), cfgW(2, 1, 16), 10000, 0, 1);
    TEST_ASSERT_EQUAL_UINT32(10000, r.got.size());
    printf("  [10KB window=16 took %d round-trips]\n", r.rounds);
    // ~3 (16*230=3680/round) vs ~11 at window=4
    TEST_ASSERT_LESS_THAN(8, r.rounds);
}

/**
 * @brief Selective-Repeat with a big window under 20% loss delivers intact.
 */
void test_sr_window16_loss() {
    auto src = make_payload(8000);
    Result r = stream(cfgW(1, 2, 16), cfgW(2, 1, 16), 8000, 20, 4242);
    TEST_ASSERT_EQUAL_UINT32(8000, r.got.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src.data(), r.got.data(), 8000);
}

/**
 * @brief Large-stream regression: 256 KB (hundreds of seq wraps) with window 16
 *        + AEAD + compression + loss arrives byte-exact.
 *
 * Guards the bulk path the 6-10 KB tests miss. (A 128 KB hardware run once
 * failed on a USB-hop drop; the sim has no USB hop, so this proves the link
 * layer itself is clean at scale.)
 */
void test_stream_large_enc_comp() {
    link_layer::Config ca = cfgA(true, true); ca.window = 16;
    link_layer::Config cb = cfgB(true, true); cb.window = 16;
    const size_t N = 256 * 1024;
    auto src = make_payload(N);
    Result r = stream(ca, cb, N, 15, 31337);
    TEST_ASSERT_EQUAL_UINT32(N, r.got.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src.data(), r.got.data(), N);
}

// ---- SPSC frame ring (interrupt-driven RX plumbing) ----

/**
 * @brief Push N frames, pop them back: FIFO order + byte-exact.
 */
void test_frame_ring_basic() {
    static link_layer::FrameRing<4, 255> ring;
    TEST_ASSERT_TRUE(ring.Empty());
    uint8_t a[3] = {1, 2, 3}, b[2] = {9, 8};
    TEST_ASSERT_TRUE(ring.Push(a, 3));
    TEST_ASSERT_TRUE(ring.Push(b, 2));
    TEST_ASSERT_FALSE(ring.Empty());
    uint8_t out[255];
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)ring.Pop(out, sizeof out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, out, 3);
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)ring.Pop(out, sizeof out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(b, out, 2);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)ring.Pop(out, sizeof out));  // empty
    TEST_ASSERT_TRUE(ring.Empty());
}

/**
 * @brief A full ring drops the newest push (capacity = SLOTS-1); ARQ resends.
 */
void test_frame_ring_full_drops() {
    static link_layer::FrameRing<4, 8> ring;   // holds 3 (one slot reserved)
    uint8_t f[1] = {0};
    TEST_ASSERT_TRUE(ring.Push(f, 1));
    TEST_ASSERT_TRUE(ring.Push(f, 1));
    TEST_ASSERT_TRUE(ring.Push(f, 1));
    TEST_ASSERT_FALSE(ring.Push(f, 1));    // full -> dropped
    uint8_t out[8];
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)ring.Pop(out, sizeof out));
    TEST_ASSERT_TRUE(ring.Push(f, 1));     // now fits again
}

/**
 * @brief Interleaved push/pop wraps the indices many times, staying byte-exact
 *        — the real radio-task/main-loop access pattern (produce one, consume
 *        one).
 */
void test_frame_ring_wrap_byte_exact() {
    static link_layer::FrameRing<4, 255> ring;
    uint8_t in[200], out[255];
    for (int round = 0; round < 1000; round++) {
        size_t n = 1 + (round % 200);
        for (size_t i = 0; i < n; i++) in[i] = (uint8_t)(round + i);
        TEST_ASSERT_TRUE(ring.Push(in, n));
        size_t got = ring.Pop(out, sizeof out);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)n, (uint32_t)got);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(in, out, n);
    }
    TEST_ASSERT_TRUE(ring.Empty());
}

/**
 * @brief An oversized frame is rejected, not truncated or overflowed.
 */
void test_frame_ring_oversize_rejected() {
    static link_layer::FrameRing<4, 16> ring;
    uint8_t big[20] = {0};
    TEST_ASSERT_FALSE(ring.Push(big, 20));   // > FRAME_MAX
    TEST_ASSERT_TRUE(ring.Empty());
}

/**
 * @brief MAC-based role election (identity.h): the lower MAC is the initiator,
 *        and the elected role yields DISTINCT link addresses (1/2).
 *
 * Distinct addresses are required so the AEAD nonce (which includes the
 * address) never repeats across the two ends.
 */
void test_identity_election() {
    uint8_t lo[6] = {0xB8,0xF8,0x62,0xF8,0x97,0x04};   // responder's real MAC
    uint8_t hi[6] = {0xB8,0xF8,0x62,0xF9,0xFA,0x50};   // initiator's real MAC
    // Symmetric + decisive: exactly one side is the initiator.
    TEST_ASSERT_TRUE(link_layer::ElectInitiator(lo, hi));
    TEST_ASSERT_FALSE(link_layer::ElectInitiator(hi, lo));
    // A board that hears its own echo must NOT elect itself initiator.
    TEST_ASSERT_FALSE(link_layer::ElectInitiator(lo, lo));
    // Tie-break falls to the least-significant byte when the prefix matches.
    uint8_t a[6] = {1,2,3,4,5,6}, b[6] = {1,2,3,4,5,7};
    TEST_ASSERT_TRUE(link_layer::ElectInitiator(a, b));
    // Addresses follow the role and are always distinct (nonce safety).
    uint8_t ia, ip, ra, rp;
    link_layer::AssignAddrs(true,  &ia, &ip);   // initiator
    link_layer::AssignAddrs(false, &ra, &rp);   // responder
    TEST_ASSERT_EQUAL_UINT8(1, ia); TEST_ASSERT_EQUAL_UINT8(2, ip);
    TEST_ASSERT_EQUAL_UINT8(2, ra); TEST_ASSERT_EQUAL_UINT8(1, rp);
    TEST_ASSERT_TRUE(ia != ra);         // the two ends never share an address
}

/**
 * @brief One board's view of MAC discovery: from our MAC and the peer MAC we
 *        heard beaconed, elect the role and assign addresses.
 *
 * Mirrors what Device::DiscoverRole() does per board (identity.h election +
 * AssignAddrs), so the two-board OUTCOME can be checked natively without a
 * radio. A test fixture only — it stays here, not in identity.h.
 *
 * @param[in]  my_mac   this board's 6-byte MAC.
 * @param[in]  peer_mac the peer MAC this board heard.
 * @param[out] addr     receives this board's elected link address.
 * @param[out] peer     receives the peer's link address.
 * @return true if this board elected itself the initiator.
 */
static bool discover_view(const uint8_t my_mac[6], const uint8_t peer_mac[6],
                          uint8_t* addr, uint8_t* peer) {
    bool init = link_layer::ElectInitiator(my_mac, peer_mac);
    link_layer::AssignAddrs(init, addr, peer);
    return init;
}

/**
 * @brief Assert the two-board discovery outcome is always safe: complementary
 *        roles, mutually-agreed addresses, and distinct (nonce-safe) addresses.
 *
 * This is the invariant the entry-34 regression broke on hardware: with role
 * election compiled out, BOTH boards defaulted to address 1 and both elected
 * themselves initiator, so no link formed. Whenever the election runs, exactly
 * one board is the initiator — never both, never neither.
 *
 * @param[in] mac_a first board's MAC.
 * @param[in] mac_b second board's MAC (must differ from @p mac_a).
 */
static void assert_discovery_safe(const uint8_t mac_a[6],
                                  const uint8_t mac_b[6]) {
    uint8_t aa, ap, ba, bp;
    bool a_init = discover_view(mac_a, mac_b, &aa, &ap);   // board A's view
    bool b_init = discover_view(mac_b, mac_a, &ba, &bp);   // board B's view
    // Exactly one initiator: "both initiator" (the entry-34 symptom) and
    // "neither initiator" are both impossible for two distinct MACs.
    TEST_ASSERT_TRUE(a_init != b_init);
    // The two ends agree on the address map (A's addr is B's peer, and v.v.).
    TEST_ASSERT_EQUAL_UINT8(aa, bp);
    TEST_ASSERT_EQUAL_UINT8(ba, ap);
    // Distinct, non-zero addresses -> the AEAD nonce never repeats per side.
    TEST_ASSERT_TRUE(aa != ba);
    TEST_ASSERT_TRUE(aa != 0 && ba != 0);
}

/**
 * @brief MAC discovery converges to a safe role split for every MAC pair.
 *
 * Drives assert_discovery_safe over two example board MACs, a near-tie pair
 * (differ only in the last byte), and a deterministic sweep of random pairs —
 * so the "both boards became initiator" hardware failure (journey entry 34)
 * can never originate in the election logic itself. (The regression was the
 * feature being compiled OUT of the firmware, which native tests can't see;
 * the build-time guard in fw_device.cpp covers that half.)
 */
void test_mac_discovery_outcome() {
    // Two example distinct factory MACs (the bench boards' USB-id MACs).
    uint8_t b0[6] = {0xB8,0xF8,0x62,0xF8,0x9A,0xF8};
    uint8_t b1[6] = {0xB8,0xF8,0x62,0xF9,0xFA,0x50};
    assert_discovery_safe(b0, b1);
    // Near-tie: identical except the least-significant byte.
    uint8_t t0[6] = {1,2,3,4,5,6}, t1[6] = {1,2,3,4,5,7};
    assert_discovery_safe(t0, t1);
    // Property sweep: many random distinct pairs all resolve safely.
    rng_seed(0xD15C0DE);   // "DISC0DE" — deterministic discovery test vector
    for (int i = 0; i < 2000; i++) {
        uint8_t x[6], y[6];
        for (int j = 0; j < 6; j++) {
            x[j] = (uint8_t)(rnd() & 0xFF);
            y[j] = (uint8_t)(rnd() & 0xFF);
        }
        if (memcmp(x, y, 6) == 0) y[5] ^= 1;   // force the MACs distinct
        assert_discovery_safe(x, y);
    }
}

/** @brief Unity per-test setup hook (no global state to prepare). */
void setUp() {}
/** @brief Unity per-test teardown hook (nothing to clean up). */
void tearDown() {}

/**
 * @brief Register and run every link-layer unit test under Unity.
 *
 * @return Unity's aggregate pass/fail status code.
 */
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_compress_roundtrip);
    RUN_TEST(test_aead_kat);
    RUN_TEST(test_aead_tamper);
    RUN_TEST(test_x25519_kat);
    RUN_TEST(test_pair_agreement);
    RUN_TEST(test_aead_replay_window);
    RUN_TEST(test_ctrl_channel_plain);
    RUN_TEST(test_ctrl_channel_enc);
    RUN_TEST(test_aux_roundtrip);
    RUN_TEST(test_aux_tamper);
    RUN_TEST(test_modeswitch_converge);
    RUN_TEST(test_modeswitch_ack_loss);
    RUN_TEST(test_modeswitch_abort_persistent_loss);
    RUN_TEST(test_msm_exhaustive_loss);
    RUN_TEST(test_msm_exhaustive_gfsk);
    RUN_TEST(test_msm_reboot_midswitch);
    RUN_TEST(test_msm_rendezvous_is_required);
    RUN_TEST(test_msm_double_switch);
    RUN_TEST(test_adr_ladder);
    RUN_TEST(test_adr_cadence_gate);
    RUN_TEST(test_adr_loss_steps_down);
    RUN_TEST(test_adr_no_climb_into_loss);
    RUN_TEST(test_adr_step_up_hysteresis);
    RUN_TEST(test_adr_snr_down_immediate);
    RUN_TEST(test_adr_gfsk_gate);
    RUN_TEST(test_adr_auto_excludes_turbo);
    RUN_TEST(test_adr_gfsk_gated_on_rssi);
    RUN_TEST(test_adr_gfsk_down_only);
    RUN_TEST(test_adr_abort_when_stuck);
    RUN_TEST(test_adr_fallback_penalizes_mode);
    RUN_TEST(test_session_handshake_agreement);
    RUN_TEST(test_session_forward_secrecy);
    RUN_TEST(test_session_wrong_static_key_fails_aead);
    RUN_TEST(test_session_aead_roundtrip);
    RUN_TEST(test_session_tampered_public);
    RUN_TEST(test_fast_retransmit);
    RUN_TEST(test_aead_reboot_nonce_skip);
    RUN_TEST(test_aead_tamper_stream);
    RUN_TEST(test_stream_lossless);
    RUN_TEST(test_stream_loss_20);
    RUN_TEST(test_stream_compress);
    RUN_TEST(test_stream_encrypt);
    RUN_TEST(test_stream_encrypt_tag16);
    RUN_TEST(test_stream_comp_enc_loss);
    RUN_TEST(test_stream_incompressible_comp);
    RUN_TEST(test_windowing_efficiency);
    RUN_TEST(test_basics_window_ordering);
    RUN_TEST(test_basics_large_transfer);
    RUN_TEST(test_reconnect_resync);
    RUN_TEST(test_sr_reordering);
    RUN_TEST(test_sr_window16_efficiency);
    RUN_TEST(test_sr_window16_loss);
    RUN_TEST(test_stream_large_enc_comp);
    RUN_TEST(test_frame_ring_basic);
    RUN_TEST(test_frame_ring_full_drops);
    RUN_TEST(test_frame_ring_wrap_byte_exact);
    RUN_TEST(test_frame_ring_oversize_rejected);
    RUN_TEST(test_identity_election);
    RUN_TEST(test_mac_discovery_outcome);
    return UNITY_END();
}
