#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

#if defined(_MSC_VER)
#pragma warning(push)
// C4324: "structure was padded due to alignment specifier". The padding is
// the entire point here — it is what keeps the producer's and consumer's
// indices off each other's cache lines — so the warning is noise.
#pragma warning(disable : 4324)
#endif

namespace pricetime {

// Cache line size. std::hardware_destructive_interference_size is the right
// answer but libstdc++ only exposes it with an ABI-warning opt-in, so the
// value is spelled out. 64 bytes on every x86-64 and on 64-bit ARM; being
// wrong here costs performance, never correctness.
inline constexpr std::size_t kCacheLine = 64;

// Wait-free single-producer / single-consumer ring buffer.
//
// This is the queue between the network thread and the matching thread in
// M3. Exactly one thread may push and exactly one may pop; that restriction
// is what makes the whole thing work without a single lock or CAS.
//
// Three details carry all the correctness:
//
//  1. Memory ordering. The producer writes the slot, then publishes with a
//     release store to `head_`. The consumer acquires `head_`, and that
//     acquire/release pair is what makes the slot's contents visible. Using
//     relaxed ordering on the index would compile and run and pass casual
//     tests, then tear data on a weakly ordered machine (ARM) or under an
//     aggressive optimizer. The stress test below is run under ThreadSanitizer
//     in CI for exactly this reason.
//
//  2. False sharing. The producer's index and the consumer's index sit on
//     separate cache lines. Without the padding, every push invalidates the
//     line the consumer is reading and vice versa — the two threads ping-pong
//     one cache line and throughput collapses by an order of magnitude, with
//     no visible bug to explain it.
//
//  3. Cached opposite index. The producer keeps its own copy of the last
//     `tail_` it saw and only re-reads the real one when the queue looks
//     full. That turns the common case into zero shared-cache-line reads.
//
// Capacity is rounded up to a power of two so the wrap is a mask rather than
// a modulo, and one slot is left unused so full and empty are distinguishable
// without a separate count (which would itself be contended state).
template <class T>
class SpscQueue {
  static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow-destructible");

 public:
  explicit SpscQueue(std::size_t capacity) : mask_(round_up_pow2(capacity + 1) - 1) {
    slots_ = std::make_unique<T[]>(mask_ + 1);
  }

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  // Producer side only.
  [[nodiscard]] bool try_push(const T& value) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t next = (head + 1) & mask_;
    if (next == cached_tail_) {
      // Might be full; re-read the consumer's real position before giving up.
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (next == cached_tail_) return false;
    }
    slots_[head] = value;
    // Release: everything written to the slot above happens-before any
    // consumer that acquires this index.
    head_.store(next, std::memory_order_release);
    return true;
  }

  // Consumer side only.
  [[nodiscard]] bool try_pop(T& out) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == cached_head_) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail == cached_head_) return false;  // genuinely empty
    }
    out = slots_[tail];
    tail_.store((tail + 1) & mask_, std::memory_order_release);
    return true;
  }

  // Both indices are read with acquire, but the result is only ever a hint:
  // the other thread may change it before the caller acts on it. Useful for
  // instrumentation, never for control flow.
  [[nodiscard]] std::size_t size_approx() const {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return (head - tail) & mask_;
  }

  [[nodiscard]] bool empty_approx() const {
    return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
  }

  // Usable slots; one is always sacrificed to distinguish full from empty.
  [[nodiscard]] std::size_t capacity() const noexcept { return mask_; }

 private:
  static std::size_t round_up_pow2(std::size_t n) {
    std::size_t p = 1;
    while (p < n) p <<= 1;
    return p;
  }

  std::unique_ptr<T[]> slots_;
  std::size_t mask_;

  // Written by the producer, read by the consumer.
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  // Producer-private cache of tail_; never touched by the consumer.
  alignas(kCacheLine) std::size_t cached_tail_ = 0;

  // Written by the consumer, read by the producer.
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  // Consumer-private cache of head_.
  alignas(kCacheLine) std::size_t cached_head_ = 0;

  // Keeps whatever follows this object off the consumer's line.
  char padding_[kCacheLine]{};
};

}  // namespace pricetime

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
