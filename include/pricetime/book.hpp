#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>

#include "pricetime/hash.hpp"
#include "pricetime/level.hpp"
#include "pricetime/order_pool.hpp"
#include "pricetime/types.hpp"

namespace pricetime {

enum class Result : std::uint8_t {
  Ok = 0,
  RejectedDuplicateId,
  RejectedBadQty,
  RejectedBadPrice,
  RejectedUnknownId,
};

// Price-time-priority limit order book. Single-threaded by design (M1).
//
// The Ladder policy owns price -> Level lookup and best-price tracking:
//   bool   valid_price(Price) [may be static]
//   Level* get_or_create(Side, Price)
//   void   on_level_empty(Side, Level*)  — the level is dead after this call
//   Level* best(Side) (+ const overload) — nullptr when that side is empty
//   template <class F> void for_each_level(Side, F) const — best -> worst
//
// Semantics:
//  - add_limit: matches against the opposite side while the limit price
//    crosses, filling resting orders in FIFO order at THEIR price (price
//    improvement goes to the aggressor); any remainder rests.
//  - cancel: removes the remaining quantity of an open order.
//  - replace: ITCH-style — validates the new order fully, then cancels the
//    old one and enters the remainder as a brand-new order (new id, new time
//    priority). A replace priced through the book executes like any
//    aggressive add. There is no in-place "reduce keeps priority" modify in
//    M1; that is an OUCH-style extension.
template <class Ladder>
class OrderBook {
 public:
  // Share-conservation accounting, maintained on the hot path for the tests:
  // added == 2 * traded + canceled + (open quantity resting in the book),
  // because each execution consumes qty from both the aggressor and a rester.
  struct Counters {
    std::uint64_t added_qty = 0;     // accepted add/replace quantity
    std::uint64_t traded_qty = 0;    // per execution event
    std::uint64_t canceled_qty = 0;  // remaining quantity at cancel/replace-out
  };

  template <class... LadderArgs>
  explicit OrderBook(LadderArgs&&... args) : ladder_(std::forward<LadderArgs>(args)...) {
    orders_.reserve(1u << 16);
  }

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;

  // Pre-size the id map (e.g., to the expected daily order count) so bucket
  // rehashes never land on the hot path.
  void reserve_orders(std::size_t expected) { orders_.reserve(expected); }

  template <class OnExec>
  Result add_limit(OrderId id, Side side, Price price, Qty qty, OnExec&& on_exec) {
    if (qty == 0) return Result::RejectedBadQty;
    if (!ladder_.valid_price(price)) return Result::RejectedBadPrice;
    if (orders_.find(id) != orders_.end()) return Result::RejectedDuplicateId;
    counters_.added_qty += qty;

    const Side opp = opposite(side);
    while (qty > 0) {
      Level* best_lvl = ladder_.best(opp);
      if (best_lvl == nullptr) break;
      const bool crosses =
          side == Side::Bid ? price >= best_lvl->price : price <= best_lvl->price;
      if (!crosses) break;
      while (qty > 0 && best_lvl->head != nullptr) {
        Order* resting = best_lvl->head;
        const Qty fill = qty < resting->qty ? qty : resting->qty;
        best_lvl->reduce(resting, fill);
        qty -= fill;
        counters_.traded_qty += fill;
        on_exec(Execution{resting->id, id, best_lvl->price, fill});
        if (resting->qty == 0) {
          best_lvl->remove(resting);
          orders_.erase(resting->id);
          pool_.release(resting);
        }
      }
      if (best_lvl->order_count == 0) ladder_.on_level_empty(opp, best_lvl);
    }

    if (qty > 0) {
      Level* lvl = ladder_.get_or_create(side, price);
      Order* o = pool_.alloc();
      o->id = id;
      o->price = price;
      o->qty = qty;
      o->side = side;
      lvl->push_back(o);
      orders_.emplace(id, o);
    }
    return Result::Ok;
  }

  Result add_limit(OrderId id, Side side, Price price, Qty qty) {
    return add_limit(id, side, price, qty, [](const Execution&) {});
  }

  Result cancel(OrderId id) {
    const auto it = orders_.find(id);
    if (it == orders_.end()) return Result::RejectedUnknownId;
    cancel_open(it);
    return Result::Ok;
  }

  template <class OnExec>
  Result replace(OrderId old_id, OrderId new_id, Price new_price, Qty new_qty,
                 OnExec&& on_exec) {
    const auto it = orders_.find(old_id);
    if (it == orders_.end()) return Result::RejectedUnknownId;
    // Validate the new order completely before destroying the old one, so a
    // rejected replace leaves the book untouched.
    if (new_qty == 0) return Result::RejectedBadQty;
    if (!ladder_.valid_price(new_price)) return Result::RejectedBadPrice;
    if (new_id != old_id && orders_.find(new_id) != orders_.end()) {
      return Result::RejectedDuplicateId;
    }
    const Side side = it->second->side;
    cancel_open(it);
    return add_limit(new_id, side, new_price, new_qty, std::forward<OnExec>(on_exec));
  }

  Result replace(OrderId old_id, OrderId new_id, Price new_price, Qty new_qty) {
    return replace(old_id, new_id, new_price, new_qty, [](const Execution&) {});
  }

  [[nodiscard]] const Level* best(Side s) const noexcept { return ladder_.best(s); }
  [[nodiscard]] std::size_t open_orders() const noexcept { return orders_.size(); }
  [[nodiscard]] const Counters& counters() const noexcept { return counters_; }

  template <class F>
  void for_each_level(Side s, F&& f) const {
    ladder_.for_each_level(s, std::forward<F>(f));
  }

  // Deterministic fingerprint of the full book: every level best -> worst,
  // every resting order in FIFO order, hashed platform-independently. Books
  // that processed equivalent histories hash identically — regardless of
  // which ladder policy they run on.
  [[nodiscard]] std::uint64_t state_hash() const {
    Fnv1a64 h;
    for (const Side s : {Side::Bid, Side::Ask}) {
      h.mix(static_cast<std::uint64_t>(s) + 1);
      ladder_.for_each_level(s, [&h](const Level& lvl) {
        h.mix(static_cast<std::uint64_t>(lvl.price));
        h.mix(lvl.total_qty);
        h.mix(lvl.order_count);
        for (const Order* o = lvl.head; o != nullptr; o = o->next) {
          h.mix(o->id);
          h.mix(o->qty);
        }
      });
    }
    return h.value;
  }

 private:
  using OrderMap = std::unordered_map<OrderId, Order*>;

  void cancel_open(typename OrderMap::iterator it) {
    Order* o = it->second;
    Level* lvl = o->level;
    const Side side = o->side;
    counters_.canceled_qty += o->qty;
    lvl->remove(o);
    orders_.erase(it);
    pool_.release(o);
    if (lvl->order_count == 0) ladder_.on_level_empty(side, lvl);
  }

  Ladder ladder_;
  OrderPool pool_;
  OrderMap orders_;
  Counters counters_;
};

}  // namespace pricetime
