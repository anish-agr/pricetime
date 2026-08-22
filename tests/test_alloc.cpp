// Deterministic performance regression tests.
//
// Timing numbers from a shared CI runner are worthless, so these assert on
// allocation behaviour instead: machine-independent, reproducible, and
// directly tied to the property that matters — an exchange's matching loop
// must not call into the allocator, because that is where unbounded tail
// latency comes from.
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "alloc_counter.hpp"
#include "pricetime/book.hpp"
#include "pricetime/id_map.hpp"
#include "pricetime/ladder_dense.hpp"
#include "pricetime/ladder_map.hpp"

using namespace pricetime;
using pricetime::test::AllocGuard;

namespace {

constexpr Price kMin = 0;
constexpr Price kMax = 4095;
constexpr Price kMid = 2048;

std::uint64_t rng(std::uint64_t& s) {
  s += 0x9E3779B97F4A7C15ull;
  std::uint64_t z = s;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

void* volatile g_escape_sink = nullptr;

// Round-trips a pointer through a volatile so the optimizer must treat the
// allocation behind it as observable and cannot elide it. The value is read
// back as well as written, since a write-only sink trips
// -Wunused-but-set-variable.
bool escape(void* p) {
  g_escape_sink = p;
  return g_escape_sink == p;
}

}  // namespace

TEST_CASE("the allocation counter itself works") {
  // A new-expression is NOT a reliable way to provoke an allocation: C++14
  // ([expr.new]/10, N3664) lets an implementation omit calls to a replaceable
  // allocation function, and GCC at -O2 duly deletes a local new/delete pair
  // outright — this test failed on GCC while passing on MSVC for exactly that
  // reason. Calling the allocation function directly and letting the pointer
  // escape leaves nothing to elide.
  std::size_t counted = 0;
  std::size_t counted_bytes = 0;
  bool escaped = false;
  {
    AllocGuard guard;
    void* p = ::operator new(64);
    escaped = escape(p);
    // Sampled before any doctest macro runs, since asserting is not
    // guaranteed to be allocation-free itself.
    counted = guard.allocations();
    counted_bytes = guard.bytes();
    ::operator delete(p);
  }
  CHECK(escaped);
  CHECK(counted == 1);
  CHECK(counted_bytes >= 64);

  std::size_t quiet_count = 0;
  {
    AllocGuard quiet;
    volatile int x = 1;
    (void)x;
    quiet_count = quiet.allocations();
  }
  CHECK(quiet_count == 0);
}

// The headline claim: with the dense ladder and the open-addressing id map,
// a warmed book runs add/cancel traffic without touching the heap once. Both
// the order pool (chunked arena, LIFO free list) and the id map (pre-sized,
// no per-element nodes) have to hold up for this to pass.
TEST_CASE("steady-state add/cancel is allocation-free (dense + open addressing)") {
  OrderBook<DenseLadder, OpenAddressIdMap> book{kMin, kMax};
  book.reserve_orders(300000);

  // Warm up: grow the pool and the id map to their working size, and let the
  // scratch vector reach its final capacity. Allocation here is expected and
  // is exactly what capacity planning is for.
  constexpr std::size_t kResting = 100000;
  std::vector<OrderId> live;
  live.reserve(kResting * 2);
  std::uint64_t seed = 0xBEEF;
  OrderId next_id = 1;
  for (std::size_t i = 0; i < kResting; ++i) {
    const Side side = (i & 1) == 0 ? Side::Bid : Side::Ask;
    const Price price =
        side == Side::Bid ? kMid - 1 - static_cast<Price>(rng(seed) % 500)
                          : kMid + 1 + static_cast<Price>(rng(seed) % 500);
    REQUIRE(book.add_limit(next_id, side, price, static_cast<Qty>(1 + rng(seed) % 100)) ==
            Result::Ok);
    live.push_back(next_id++);
  }

  // Measured window: alternating add and cancel, never exceeding the warmed
  // high-water mark, so every order node comes off the pool's free list.
  {
    AllocGuard guard;
    for (std::size_t i = 0; i < 200000; ++i) {
      if ((i & 1) == 0) {
        const Side side = rng(seed) % 2 == 0 ? Side::Bid : Side::Ask;
        const Price price =
            side == Side::Bid ? kMid - 1 - static_cast<Price>(rng(seed) % 500)
                              : kMid + 1 + static_cast<Price>(rng(seed) % 500);
        book.add_limit(next_id, side, price, static_cast<Qty>(1 + rng(seed) % 100));
        live.push_back(next_id++);
      } else {
        const std::size_t j = static_cast<std::size_t>(rng(seed) % live.size());
        const OrderId id = live[j];
        live[j] = live.back();
        live.pop_back();
        book.cancel(id);
      }
    }
    const std::size_t allocs = guard.allocations();
    INFO("allocations during steady-state window: ", allocs);
    CHECK(allocs == 0);
  }
}

// Matching allocates nothing either: fills only unlink nodes and return them
// to the free list.
TEST_CASE("matching is allocation-free (dense + open addressing)") {
  OrderBook<DenseLadder, OpenAddressIdMap> book{kMin, kMax};
  book.reserve_orders(200000);
  std::uint64_t seed = 0xF00D;
  OrderId next_id = 1;
  constexpr std::size_t kResting = 50000;
  for (std::size_t i = 0; i < kResting; ++i) {
    book.add_limit(next_id++, Side::Ask, kMid + 1 + static_cast<Price>(rng(seed) % 200),
                   static_cast<Qty>(1 + rng(seed) % 100));
  }
  // Let the pool reach the high-water mark this test will need.
  for (int warm = 0; warm < 1000; ++warm) {
    const Level* best = book.best(Side::Ask);
    book.add_limit(next_id++, Side::Bid, best->price, best->head->qty);
  }

  {
    AllocGuard guard;
    for (std::size_t i = 0; i < 20000; ++i) {
      const Level* best = book.best(Side::Ask);
      if (best == nullptr) break;
      book.add_limit(next_id++, Side::Bid, best->price, best->head->qty);
    }
    INFO("allocations during matching window: ", guard.allocations());
    CHECK(guard.allocations() == 0);
  }
}

// The honest counterpart: the tree-backed ladder allocates a node every time a
// price level comes into existence. That is not a bug — it is the cost of the
// data structure, and pinning it in a test keeps the comparison truthful.
TEST_CASE("map ladder allocates per new price level, by design") {
  OrderBook<MapLadder, OpenAddressIdMap> book{};
  book.reserve_orders(10000);
  for (Price p = 1000; p < 1100; ++p) book.add_limit(static_cast<OrderId>(p), Side::Bid, p, 10);

  AllocGuard guard;
  // Every one of these prices is new to the book, so every one creates a level.
  for (Price p = 1100; p < 1200; ++p) {
    book.add_limit(static_cast<OrderId>(p), Side::Bid, p, 10);
  }
  INFO("map-ladder allocations for 100 new levels: ", guard.allocations());
  CHECK(guard.allocations() >= 100);
}

// The order pool is what makes the zero-allocation claim possible: it must
// hand back recycled nodes rather than growing, once the high-water mark is
// reached.
TEST_CASE("order pool recycles instead of growing") {
  OrderBook<DenseLadder, OpenAddressIdMap> book{kMin, kMax};
  book.reserve_orders(50000);
  for (OrderId i = 1; i <= 20000; ++i) book.add_limit(i, Side::Bid, 1000, 10);
  for (OrderId i = 1; i <= 20000; ++i) book.cancel(i);

  AllocGuard guard;
  for (OrderId i = 20001; i <= 40000; ++i) book.add_limit(i, Side::Bid, 1000, 10);
  CHECK(guard.allocations() == 0);
  CHECK(book.open_orders() == 20000);
}
