#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "pricetime/spsc_queue.hpp"

using namespace pricetime;

TEST_CASE("spsc queue: single-threaded push and pop preserve order") {
  SpscQueue<int> q(8);
  int out = 0;
  CHECK_FALSE(q.try_pop(out));  // empty
  CHECK(q.empty_approx());

  for (int i = 0; i < 5; ++i) REQUIRE(q.try_push(i));
  CHECK(q.size_approx() == 5);
  for (int i = 0; i < 5; ++i) {
    REQUIRE(q.try_pop(out));
    CHECK(out == i);  // FIFO
  }
  CHECK(q.empty_approx());
  CHECK_FALSE(q.try_pop(out));
}

TEST_CASE("spsc queue: capacity is honoured and one slot is reserved") {
  SpscQueue<int> q(4);  // rounds to 8 slots, 7 usable
  CHECK(q.capacity() == 7);
  for (std::size_t i = 0; i < q.capacity(); ++i) {
    REQUIRE(q.try_push(static_cast<int>(i)));
  }
  CHECK_FALSE(q.try_push(999));  // full, and says so rather than overwriting

  int out = 0;
  REQUIRE(q.try_pop(out));
  CHECK(out == 0);
  CHECK(q.try_push(999));  // space again
}

TEST_CASE("spsc queue: indices wrap without losing data") {
  SpscQueue<int> q(4);
  int out = 0;
  // Cycle several times through the ring so head and tail wrap repeatedly.
  for (int round = 0; round < 100; ++round) {
    for (int i = 0; i < 5; ++i) REQUIRE(q.try_push(round * 100 + i));
    for (int i = 0; i < 5; ++i) {
      REQUIRE(q.try_pop(out));
      CHECK(out == round * 100 + i);
    }
  }
  CHECK(q.empty_approx());
}

TEST_CASE("spsc queue: a non-power-of-two capacity rounds up") {
  CHECK(SpscQueue<int>(5).capacity() == 7);   // 8 slots
  CHECK(SpscQueue<int>(100).capacity() == 127);  // 128 slots
  CHECK(SpscQueue<int>(1).capacity() == 1);   // 2 slots
}

// The real test: two threads, a checksum, and a value stream where any loss,
// duplication, or reordering changes the result. Run this under
// ThreadSanitizer (CI does) and it also proves the memory ordering, which no
// single-threaded test can.
TEST_CASE("spsc queue: concurrent producer and consumer lose nothing") {
  constexpr std::uint64_t kCount = 2000000;
  SpscQueue<std::uint64_t> q(1024);
  std::atomic<bool> producer_done{false};

  std::uint64_t consumed = 0;
  std::uint64_t checksum = 0;
  bool ordered = true;

  std::thread producer([&] {
    for (std::uint64_t i = 1; i <= kCount; ++i) {
      // Busy-wait on a full queue: this is the backpressure path, and
      // exercising it is the point.
      while (!q.try_push(i)) {
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&] {
    std::uint64_t expected = 1;
    std::uint64_t value = 0;
    for (;;) {
      if (q.try_pop(value)) {
        if (value != expected) ordered = false;  // gap or reorder
        ++expected;
        ++consumed;
        checksum += value;
        continue;
      }
      // Empty: only safe to stop once the producer has finished AND a
      // subsequent pop still finds nothing. Checking the flag first would
      // race against items still in flight.
      if (producer_done.load(std::memory_order_acquire) && !q.try_pop(value)) break;
      std::this_thread::yield();
    }
  });

  producer.join();
  consumer.join();

  CHECK(ordered);
  CHECK(consumed == kCount);
  CHECK(checksum == kCount * (kCount + 1) / 2);  // Gauss: nothing lost, nothing duplicated
}

// A payload wider than a machine word: if the release/acquire pairing were
// wrong, a consumer could observe a half-written struct. The invariant here
// is internal to each element, so torn writes show up as mismatched fields
// rather than as a missing item.
TEST_CASE("spsc queue: multi-word payloads are published atomically") {
  struct Payload {
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    std::uint64_t c = 0;
    std::uint64_t d = 0;
  };

  constexpr std::uint64_t kCount = 500000;
  SpscQueue<Payload> q(256);
  std::atomic<bool> done{false};
  std::uint64_t torn = 0;
  std::uint64_t seen = 0;

  std::thread producer([&] {
    for (std::uint64_t i = 1; i <= kCount; ++i) {
      Payload p{i, i * 2, i * 3, i * 4};
      while (!q.try_push(p)) std::this_thread::yield();
    }
    done.store(true, std::memory_order_release);
  });

  std::thread consumer([&] {
    Payload p;
    for (;;) {
      if (q.try_pop(p)) {
        if (p.b != p.a * 2 || p.c != p.a * 3 || p.d != p.a * 4) ++torn;
        ++seen;
        continue;
      }
      if (done.load(std::memory_order_acquire) && !q.try_pop(p)) break;
      std::this_thread::yield();
    }
  });

  producer.join();
  consumer.join();

  CHECK(seen == kCount);
  CHECK(torn == 0);
}

// The producer must see backpressure rather than silently overwriting when
// the consumer stalls.
TEST_CASE("spsc queue: a stalled consumer produces backpressure, not data loss") {
  SpscQueue<int> q(16);
  int pushed = 0;
  while (q.try_push(pushed)) ++pushed;
  CHECK(pushed == static_cast<int>(q.capacity()));

  int out = 0;
  int popped = 0;
  while (q.try_pop(out)) {
    CHECK(out == popped);  // still in order, nothing overwritten
    ++popped;
  }
  CHECK(popped == pushed);
}
