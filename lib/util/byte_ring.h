/**
 * @file byte_ring.h
 * @brief A circular byte buffer with a retention policy — the device's single
 *        configurable buffer (the outbound "transmit queue"; see AT+BUFMODE).
 *
 * This is the accumulator that holds host bytes waiting to go out over the
 * radio, so it's where a backlog piles up when the peer is slow or absent. Its
 * retention policy decides what happens when it can't drain fast enough:
 *
 *   - @ref kKeepAll — byte-exact, oldest-preserving. Push() writes only what
 *     fits; overflow is left for the caller to back-pressure or count. Nothing
 *     already queued is discarded. (TCP send buffer / `SO_SNDBUF`; DDS
 *     `HISTORY = KEEP_ALL`.)
 *   - @ref kKeepLatest — freshness-first. Push() ALWAYS accepts, discarding the
 *     OLDEST queued bytes so the retained data never exceeds the keep window
 *     (min(keep, capacity)). Bounds catch-up to the window, at the cost of the
 *     byte-exact guarantee. (DDS `HISTORY = KEEP_LAST`, depth = keep.)
 *
 * Operates on caller-owned storage (no allocation) and has no Arduino/platform
 * dependencies, so it is unit-testable in the native suite. One slot is
 * reserved to tell full from empty, so usable capacity is cap-1.
 */
#ifndef LORA_SERIAL_UTIL_BYTE_RING_H_
#define LORA_SERIAL_UTIL_BYTE_RING_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace util {

/// Circular byte buffer + retention policy (see file comment). Header-only so
/// the native tests and the firmware share exactly one implementation.
class ByteRing {
 public:
  /// Retention policy when the queue can't drain fast enough (see file comment).
  enum Policy : uint8_t {
    kKeepAll,     ///< keep oldest, refuse/report overflow (byte-exact)
    kKeepLatest   ///< keep the freshest `keep` bytes, drop oldest
  };

  /**
   * @brief Bind the ring to caller-owned storage (not owned/freed here).
   * @param[in] buf  storage. @param[in] cap  its size in bytes (usable: cap-1).
   */
  void Init(uint8_t* buf, size_t cap) {
    buf_ = buf;
    cap_ = cap;
    head_ = tail_ = 0;
  }

  /**
   * @brief Set the retention policy and (for kKeepLatest) the keep window.
   * @param[in] p     the policy. @param[in] keep  window bytes (kKeepLatest).
   */
  void SetPolicy(Policy p, size_t keep) {
    policy_ = p;
    keep_ = keep;
  }

  /** @brief The active retention policy. @return the policy. */
  Policy policy() const { return policy_; }

  /** @brief Total storage bound to the ring. @return capacity in bytes. */
  size_t capacity() const { return cap_; }

  /** @brief Bytes currently queued. @return count in bytes. */
  size_t count() const { return (head_ - tail_ + cap_) % cap_; }

  /** @brief Free space (usable is cap-1). @return free bytes, 0 if unbound. */
  size_t free() const { return cap_ ? cap_ - 1 - count() : 0; }

  /**
   * @brief Enqueue bytes, applying the retention policy.
   *
   * kKeepAll writes only what fits and reports the overflow (newest bytes not
   * written). kKeepLatest always accepts, evicting the oldest queued bytes so
   * the retained data stays within the keep window; it reports the bytes it
   * evicted. Either way the return is the number of bytes DROPPED, which the
   * caller adds to its drop counter.
   *
   * @param[in] s  bytes to enqueue. @param[in] n  how many.
   * @return bytes dropped (0 if all retained).
   */
  size_t Push(const uint8_t* s, size_t n) {
    if (!cap_ || n == 0) return 0;
    if (policy_ == kKeepLatest) return PushLatest(s, n);
    size_t fr = free();
    size_t dropped = (n > fr) ? (n - fr) : 0;   // overflow: newest not written
    Write(s, n - dropped);
    return dropped;
  }

  /**
   * @brief Copy up to n oldest bytes out and consume them.
   * @param[out] out  destination. @param[in] n  max bytes.
   * @return bytes copied (< n if the ring emptied).
   */
  size_t Read(uint8_t* out, size_t n) {
    size_t got = 0;
    while (got < n && count() > 0) {
      size_t run;
      const uint8_t* p = Peek(&run);
      size_t c = (n - got) < run ? (n - got) : run;
      memcpy(out + got, p, c);
      Drop(c);
      got += c;
    }
    return got;
  }

  /**
   * @brief The contiguous readable run at the tail (for zero-copy draining).
   * @param[out] len  length of the run (0 if empty).
   * @return pointer to the oldest byte (only `len` bytes are valid).
   */
  const uint8_t* Peek(size_t* len) const {
    *len = (head_ >= tail_) ? (head_ - tail_) : (cap_ - tail_);
    return buf_ + tail_;
  }

  /** @brief Consume n bytes from the tail (after Peek). @param[in] n bytes. */
  void Drop(size_t n) { tail_ = (tail_ + n) % cap_; }

 private:
  /// Write n bytes at the head (caller guarantees free() >= n).
  void Write(const uint8_t* s, size_t n) {
    size_t acc = 0;
    while (acc < n) {
      size_t contig = cap_ - head_;
      size_t c = (n - acc) < contig ? (n - acc) : contig;
      memcpy(buf_ + head_, s + acc, c);
      head_ = (head_ + c) % cap_;
      acc += c;
    }
  }

  /// kKeepLatest push: evict oldest so the retained data fits the keep window.
  size_t PushLatest(const uint8_t* s, size_t n) {
    size_t limit = keep_;
    if (limit > cap_ - 1) limit = cap_ - 1;   // never exceed usable capacity
    size_t dropped = 0;
    if (n >= limit) {                 // this push alone fills/exceeds the window
      dropped = count() + (n - limit);   // evict everything + s's older part
      tail_ = head_;                     // clear
      s += (n - limit);
      n = limit;
    } else {
      size_t need = count() + n;
      if (need > limit) {             // evict oldest to make room
        dropped = need - limit;
        tail_ = (tail_ + dropped) % cap_;
      }
    }
    Write(s, n);
    return dropped;
  }

  uint8_t* buf_ = nullptr;   ///< storage (caller-owned)
  size_t   cap_ = 0;         ///< storage size (usable capacity is cap-1)
  size_t   head_ = 0;        ///< producer index (Push/Write)
  size_t   tail_ = 0;        ///< consumer index (Read/Peek/Drop)
  size_t   keep_ = 0;        ///< kKeepLatest window (bytes)
  Policy   policy_ = kKeepAll;   ///< active retention policy
};

}  // namespace util

#endif  // LORA_SERIAL_UTIL_BYTE_RING_H_
