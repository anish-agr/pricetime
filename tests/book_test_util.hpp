#pragma once

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "pricetime/book.hpp"
#include "pricetime/ladder_dense.hpp"
#include "pricetime/ladder_map.hpp"

namespace pricetime::test {

// Price band shared by every generated stream: bids 950..1005, asks 995..1050.
// The 995..1005 overlap makes a fraction of adds cross and execute, and lets
// the same price serve one side, empty out, and later serve the other.
inline constexpr Price kDenseMin = 0;
inline constexpr Price kDenseMax = 4095;

template <class Ladder>
OrderBook<Ladder> make_book();

template <>
inline OrderBook<MapLadder> make_book<MapLadder>() {
  return OrderBook<MapLadder>{};
}

template <>
inline OrderBook<DenseLadder> make_book<DenseLadder>() {
  return OrderBook<DenseLadder>{kDenseMin, kDenseMax};
}

// Walks the entire book and checks every structural invariant:
// sides never crossed or locked, levels sorted best->worst, FIFO links
// consistent, per-level aggregates equal to the sum of their orders, node
// count equal to the id-map size, and global share conservation.
template <class Ladder>
void check_invariants(const OrderBook<Ladder>& book) {
  const Level* best_bid = book.best(Side::Bid);
  const Level* best_ask = book.best(Side::Ask);
  if (best_bid != nullptr && best_ask != nullptr) {
    REQUIRE(best_bid->price < best_ask->price);
  }

  std::uint64_t resting_qty = 0;
  std::size_t order_nodes = 0;
  for (const Side s : {Side::Bid, Side::Ask}) {
    bool first = true;
    Price prev_price = 0;
    book.for_each_level(s, [&](const Level& lvl) {
      REQUIRE(lvl.order_count > 0);
      if (!first) {
        if (s == Side::Bid) {
          REQUIRE(lvl.price < prev_price);
        } else {
          REQUIRE(lvl.price > prev_price);
        }
      }
      first = false;
      prev_price = lvl.price;

      std::uint64_t level_qty = 0;
      std::uint32_t level_count = 0;
      const Order* prev = nullptr;
      for (const Order* o = lvl.head; o != nullptr; o = o->next) {
        REQUIRE(o->prev == prev);
        REQUIRE(o->level == &lvl);
        REQUIRE(o->side == s);
        REQUIRE(o->price == lvl.price);
        REQUIRE(o->qty > 0);
        level_qty += o->qty;
        ++level_count;
        prev = o;
      }
      REQUIRE(lvl.tail == prev);
      REQUIRE(level_qty == lvl.total_qty);
      REQUIRE(level_count == lvl.order_count);
      resting_qty += level_qty;
      order_nodes += level_count;
    });
  }
  REQUIRE(order_nodes == book.open_orders());

  const auto& c = book.counters();
  REQUIRE(c.added_qty == 2 * c.traded_qty + c.canceled_qty + resting_qty);
}

// Deterministic random op stream: 60% add / 30% cancel / 10% replace.
// Every RNG draw happens unconditionally in a fixed order, and the live-id
// list evolves identically on every replay, so a given seed produces exactly
// the same op sequence — the foundation of the determinism and cross-ladder
// differential tests. mt19937_64's output sequence is fixed by the standard;
// no std distributions are used because their algorithms are not.
template <class Ladder>
class StreamRunner {
 public:
  explicit StreamRunner(std::uint64_t seed) : rng_(seed) {}

  void step(OrderBook<Ladder>& book) {
    const std::uint64_t roll = rng_() % 100;
    if (roll < 60 || live_.empty()) {
      do_add(book);
    } else if (roll < 90) {
      do_cancel(book);
    } else {
      do_replace(book);
    }
  }

 private:
  Price gen_price(Side side) {
    return side == Side::Bid ? static_cast<Price>(950 + rng_() % 56)
                             : static_cast<Price>(995 + rng_() % 56);
  }

  void do_add(OrderBook<Ladder>& book) {
    const Side side = rng_() % 2 == 0 ? Side::Bid : Side::Ask;
    const Price price = gen_price(side);
    const Qty qty = static_cast<Qty>(1 + rng_() % 100);
    const OrderId id = next_id_++;
    REQUIRE(book.add_limit(id, side, price, qty) == Result::Ok);
    live_.push_back(id);
  }

  // A picked id may already be gone (fully filled while resting); the book
  // must reject that cancel, and the test accepts either outcome.
  void do_cancel(OrderBook<Ladder>& book) {
    const OrderId id = take_random_live();
    const Result r = book.cancel(id);
    REQUIRE((r == Result::Ok || r == Result::RejectedUnknownId));
  }

  void do_replace(OrderBook<Ladder>& book) {
    const OrderId old_id = take_random_live();
    const Side side = rng_() % 2 == 0 ? Side::Bid : Side::Ask;  // band only; book keeps real side
    const Price price = gen_price(side);
    const Qty qty = static_cast<Qty>(1 + rng_() % 100);
    const OrderId new_id = next_id_++;
    const Result r = book.replace(old_id, new_id, price, qty);
    if (r == Result::Ok) {
      live_.push_back(new_id);
    } else {
      REQUIRE(r == Result::RejectedUnknownId);
    }
  }

  OrderId take_random_live() {
    const std::size_t i = static_cast<std::size_t>(rng_() % live_.size());
    const OrderId id = live_[i];
    live_[i] = live_.back();
    live_.pop_back();
    return id;
  }

  std::mt19937_64 rng_;
  std::vector<OrderId> live_;
  OrderId next_id_ = 1;
};

}  // namespace pricetime::test
