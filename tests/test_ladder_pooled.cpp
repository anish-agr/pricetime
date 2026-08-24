#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "alloc_counter.hpp"
#include "op_stream.hpp"
#include "pricetime/book.hpp"
#include "pricetime/id_map.hpp"
#include "pricetime/ladder_map.hpp"
#include "pricetime/ladder_pooled.hpp"
#include "pricetime/op.hpp"
#include "reference_book.hpp"

using namespace pricetime;
using pricetime::test::AllocGuard;
using pricetime::test::OpStreamGenerator;
using pricetime::test::ReferenceBook;

// Same observable semantics as every other ladder: op-for-op agreement with
// the independent naive model, on a stream that hammers level churn.
TEST_CASE("pooled ladder: reference differential under heavy level churn") {
  OrderBook<PooledMapLadder, OpenAddressIdMap> fast;
  ReferenceBook ref;
  // Narrow band -> levels constantly created and emptied, the exact pattern
  // the pool exists for.
  const auto ops = OpStreamGenerator(0x900D, 1000, 6).generate(40000);
  for (std::size_t i = 0; i < ops.size(); ++i) {
    const Result a = apply(fast, ops[i]);
    const Result b = apply(ref, ops[i]);
    if (a != b || fast.state_hash() != ref.state_hash()) {
      INFO("diverged at op ", i);
      REQUIRE(a == b);
      REQUIRE(fast.state_hash() == ref.state_hash());
    }
  }
  CHECK(fast.counters().traded_qty == ref.traded_qty());
}

TEST_CASE("pooled ladder: hashes identically to the plain map ladder") {
  OrderBook<PooledMapLadder, OpenAddressIdMap> pooled;
  OrderBook<MapLadder, OpenAddressIdMap> plain;
  const auto ops = OpStreamGenerator(0xCAFE, 1000, 40).generate(60000);
  for (const Op& op : ops) {
    apply(pooled, op);
    apply(plain, op);
  }
  CHECK(pooled.state_hash() == plain.state_hash());
  CHECK(pooled.open_orders() == plain.open_orders());
}

// The reason it exists: after warmup, creating and destroying levels — at
// prices never used before — costs zero allocations. The plain map ladder
// has a test asserting it DOES allocate here; this is its counterpart.
TEST_CASE("pooled ladder: level churn is allocation-free after warmup") {
  OrderBook<PooledMapLadder, OpenAddressIdMap> book;
  book.reserve_orders(1u << 16);

  // Warmup: create then drain a batch of levels so the pool holds nodes and
  // the order arena reaches its high-water mark. BOTH sides: the bid and ask
  // trees have different comparator types, so their node caches are separate
  // — the first version of this test warmed only bids, churned asks, and the
  // 200 "impossible" allocations it counted were exactly the ask nodes.
  OrderId id = 1;
  for (Price p = 1000; p < 1200; ++p) {
    REQUIRE(book.add_limit(id++, Side::Bid, p, 10) == Result::Ok);
    REQUIRE(book.add_limit(id++, Side::Ask, p + 5000, 10) == Result::Ok);
  }
  for (OrderId k = 1; k < id; ++k) REQUIRE(book.cancel(k) == Result::Ok);

  {
    AllocGuard guard;
    // Fresh prices every cycle: every add creates a level, every cancel
    // empties it. 5000 full churn cycles.
    for (int cycle = 0; cycle < 25; ++cycle) {
      const Price base = 10000 + cycle * 400;
      for (Price p = base; p < base + 200; ++p) {
        book.add_limit(id, Side::Ask, p, 10);
        ++id;
      }
      for (OrderId k = id - 200; k < id; ++k) book.cancel(k);
    }
    INFO("allocations during level churn: ", guard.allocations());
    CHECK(guard.allocations() == 0);
  }
  CHECK(book.open_orders() == 0);
}

TEST_CASE("pooled ladder: cache is bounded, extras are released") {
  PooledMapLadder ladder;
  std::vector<Level*> levels;
  // Create far more levels than the per-side cache cap, then empty them all.
  constexpr int kLevels = 6000;
  for (int i = 0; i < kLevels; ++i) {
    Level* lvl = ladder.get_or_create(Side::Bid, 1000 + i);
    REQUIRE(lvl != nullptr);
    // Make the level non-empty so on_level_empty semantics are honest.
    static Order o;  // links unused; only order_count matters to the ladder
    lvl->push_back(&o);
    levels.push_back(lvl);
  }
  for (Level* lvl : levels) {
    lvl->remove(lvl->head);
    ladder.on_level_empty(Side::Bid, lvl);
  }
  CHECK(ladder.cached_nodes() <= 4096);  // the cap held; the rest were freed
  CHECK(ladder.best(Side::Bid) == nullptr);
}
