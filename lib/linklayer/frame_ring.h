/**
 * @file frame_ring.h
 * @brief Single-producer / single-consumer (SPSC) ring of radio frames.
 *
 * For the interrupt-driven RX path (docs/INTERRUPT_RX.md): the high-priority
 * radio task PUSHes a frame it just read out of the SX1262 FIFO; the main loop
 * POPs it and hands it to the link layer. Exactly one producer + one consumer,
 * so it's lock-free — head_ is written only by the producer, tail_ only by the
 * consumer, with acquire/release ordering so the consumer never sees a slot
 * before its bytes are fully written (and vice-versa). This keeps g_link
 * single-threaded (only this ring crosses the task boundary). Header-only +
 * portable (no Arduino/FreeRTOS) so it unit-tests in the native sim. SLOTS is
 * the number of in-flight frames buffered; FRAME_MAX their size.
 */
#ifndef LINK_LAYER_FRAME_RING_H_
#define LINK_LAYER_FRAME_RING_H_
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace link_layer {

/** @brief Lock-free SPSC ring of fixed-size radio frames. */
template <size_t SLOTS, size_t FRAME_MAX>
class FrameRing {
  public:
    /**
     * @brief Producer (radio task): copy a frame into the ring.
     *
     * On a full ring or an oversized frame the frame is dropped — the link
     * layer's ARQ will retransmit it.
     *
     * @param[in] data the frame bytes to copy in.
     * @param[in] len  number of bytes (must be <= FRAME_MAX).
     * @return true if enqueued; false if full or oversized.
     */
    bool Push(const uint8_t* data, size_t len) {
        if (len > FRAME_MAX) return false;
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t nxt = inc(h);
        // Full if advancing head would collide with the consumer's tail.
        if (nxt == tail_.load(std::memory_order_acquire)) return false;
        std::memcpy(slots_[h].buf, data, len);
        slots_[h].len = len;
        head_.store(nxt, std::memory_order_release);   // publish the slot
        return true;
    }

    /**
     * @brief Consumer (main loop): copy the oldest frame out.
     *
     * Reads at most max_len bytes of the oldest frame.
     *
     * @param[out] out     destination buffer for the frame bytes.
     * @param[in]  max_len capacity of `out`.
     * @return the frame's length (clamped to max_len), or 0 if empty.
     */
    size_t Pop(uint8_t* out, size_t max_len) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return 0;   // empty
        size_t n = slots_[t].len;
        if (n > max_len) n = max_len;
        std::memcpy(out, slots_[t].buf, n);
        tail_.store(inc(t), std::memory_order_release);   // release the slot
        return n;
    }

    /** @brief True if the ring currently holds no frames. */
    bool Empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

  private:
    /**
     * @brief Advance a ring index one slot, wrapping at SLOTS.
     * @param[in] i the index to advance.
     * @return the next slot index (i + 1 modulo SLOTS).
     */
    static size_t inc(size_t i) { return (i + 1) % SLOTS; }
    /** @brief One ring entry: a frame buffer and its byte length. */
    struct Slot {
        uint8_t buf[FRAME_MAX];  ///< The frame bytes.
        size_t len;              ///< Number of valid bytes in `buf`.
    };
    Slot slots_[SLOTS];             ///< Backing storage for the ring entries.
    std::atomic<size_t> head_{0};   ///< Producer-owned: next write slot.
    std::atomic<size_t> tail_{0};   ///< Consumer-owned: next read slot.
};

}  // namespace link_layer

#endif  // LINK_LAYER_FRAME_RING_H_
