#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "pricetime/hash.hpp"
#include "pricetime/id_map.hpp"
#include "pricetime/level.hpp"
#include "pricetime/order_pool.hpp"
#include "pricetime/types.hpp"

namespace pricetime {

enum class Result : std::uint8_t {
  Ok = 0,
  RejectedBadId,       // id 0 is reserved (see id_map.hpp)
  RejectedBadQty,      // zero quantity
  RejectedBadPrice,    // outside the ladder's representable range
  RejectedDuplicateId,
  RejectedUnknownId,
  RejectedNoLiquidity,  // fill-or-kill that could not be filled in full
};

// One level of aggregated (L2) depth.
struct DepthLevel {
  Price price = 0;
  std::uint64_t qty = 0;
  std::uint32_t orders = 0;
};

struct Depth {
  std::vector<DepthLevel> bids;
  std::vector<DepthLevel> asks;
};

// Price-time-priority limit order book. Single-threaded by design.
//
// Two compile-time policies, both benchmarked head-to-head:
//   Ladder — price -> Level lookup and best-price tracking:
//     bool   valid_price(Price) [may be static]
//     Level* get_or_create(Side, Price)
//     void   on_level_empty(Side, Level*)  — the level is dead after this call
//     Level* best(Side) (+ const overload) — nullptr when that side is empty
//     template <class F> void for_each_level(Side, F) const — best -> worst,
//         stopping early if F returns false
//   IdMap  — order id -> Order* (see id_map.hpp)
//
// Validation order is fixed and identical for every entry point, so rejection
// codes are reproducible: bad id, bad quantity, bad price, duplicate id.
//
// Matching: an aggressive order consumes the opposite side while its limit
// crosses, filling resting orders in FIFO order at THEIR price — price
// improvement accrues to the aggressor, as on a real exchange.
//
// replace() is ITCH-style: the new order is validated completely, then the old
// one is canceled and the remainder entered as a brand-new order (new id, new
// time priority). A replace priced through the book executes like any
// aggressive add. There is no in-place "reduce keeps priority" modify here;
// that is an OUCH-style extension.
template <class Ladder, class IdMap = OpenAddressIdMap>
class OrderBook {
 public:
  // Share-conservation accounting, maintained on the hot path for the tests:
  //   added == 2 * traded + canceled + (open quantity resting in the book)
  // because each execution consumes quantity from both the aggressor and a
  // rester. Quantity killed by IOC/FOK/market semantics counts as canceled.
  struct Counters {
    std::uint64_t added_qty = 0;
    std::uint64_t traded_qty = 0;
    std::uint64_t canceled_qty = 0;
    // Quantity removed by a feed-driven execution (see execute_resting).
    // Counted separately from traded_qty because it consumes only ONE side:
    // when replaying a market-data feed the aggressor never entered this
    // book, so it contributes nothing to added_qty.
    std::uint64_t executed_qty = 0;
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

  // --- order entry ------------------------------------------------------

  // Good-till-cancel limit order: match while crossing, rest any remainder.
  template <class OnExec>
  Result add_limit(OrderId id, Side side, Price price, Qty qty, OnExec&& on_exec) {
    if (const Result r = validate(id, price, qty); r != Result::Ok) return r;
    counters_.added_qty += qty;
    const Qty left = match<true>(id, side, price, qty, on_exec);
    if (left > 0) rest(id, side, price, left);
    return Result::Ok;
  }

  Result add_limit(OrderId id, Side side, Price price, Qty qty) {
    return add_limit(id, side, price, qty, [](const Execution&) {});
  }

  // Immediate-or-cancel: match while crossing, kill the remainder.
  template <class OnExec>
  Result add_ioc(OrderId id, Side side, Price price, Qty qty, OnExec&& on_exec) {
    if (const Result r = validate(id, price, qty); r != Result::Ok) return r;
    counters_.added_qty += qty;
    counters_.canceled_qty += match<true>(id, side, price, qty, on_exec);
    return Result::Ok;
  }

  Result add_ioc(OrderId id, Side side, Price price, Qty qty) {
    return add_ioc(id, side, price, qty, [](const Execution&) {});
  }

  // Fill-or-kill: all of it, right now, or nothing at all. The pre-scan walks
  // the crossing levels before touching the book, so a kill leaves no trace —
  // that walk is the honest cost of the guarantee.
  template <class OnExec>
  Result add_fok(OrderId id, Side side, Price price, Qty qty, OnExec&& on_exec) {
    if (const Result r = validate(id, price, qty); r != Result::Ok) return r;
    if (available<true>(side, price, qty) < qty) return Result::RejectedNoLiquidity;
    counters_.added_qty += qty;
    const Qty left = match<true>(id, side, price, qty, on_exec);
    // The pre-scan guaranteed full liquidity, so nothing can be left over.
    counters_.canceled_qty += left;
    return Result::Ok;
  }

  Result add_fok(OrderId id, Side side, Price price, Qty qty) {
    return add_fok(id, side, price, qty, [](const Execution&) {});
  }

  // Market order: no limit, sweeps until filled or the book runs dry; never
  // rests. An empty opposite side is not an error — the order is simply
  // killed in full, which is what an exchange does with it.
  template <class OnExec>
  Result add_market(OrderId id, Side side, Qty qty, OnExec&& on_exec) {
    if (id == 0) return Result::RejectedBadId;
    if (qty == 0) return Result::RejectedBadQty;
    if (orders_.find(id) != nullptr) return Result::RejectedDuplicateId;
    counters_.added_qty += qty;
    counters_.canceled_qty += match<false>(id, side, 0, qty, on_exec);
    return Result::Ok;
  }

  Result add_market(OrderId id, Side side, Qty qty) {
    return add_market(id, side, qty, [](const Execution&) {});
  }

  Result cancel(OrderId id) {
    Order* o = orders_.find(id);
    if (o == nullptr) return Result::RejectedUnknownId;
    cancel_open(o);
    return Result::Ok;
  }

  // --- feed reconstruction ---------------------------------------------
  //
  // These exist for rebuilding a book from a market-data feed, where the
  // exchange has ALREADY matched everything. Running such a stream through
  // add_limit() would be wrong: a resting order that appears to cross would
  // trade against the book a second time and the reconstruction would
  // diverge from the real book immediately.

  // Rest an order without matching, whatever the price implies.
  Result insert_passive(OrderId id, Side side, Price price, Qty qty) {
    if (const Result r = validate(id, price, qty); r != Result::Ok) return r;
    counters_.added_qty += qty;
    rest(id, side, price, qty);
    return Result::Ok;
  }

  // Remove `qty` shares from a resting order because the feed reported a
  // trade against it. Removing the whole remaining quantity deletes the
  // order. Quantity larger than the remainder is clamped, and reported.
  Result execute_resting(OrderId id, Qty qty, Qty* executed = nullptr) {
    return consume(id, qty, /*is_execution=*/true, executed);
  }

  // Remove `qty` shares because the feed reported a partial cancel.
  Result reduce_resting(OrderId id, Qty qty, Qty* removed = nullptr) {
    return consume(id, qty, /*is_execution=*/false, removed);
  }

  // Price and side of a resting order, for feed handlers that must know them
  // (an ITCH replace repeats neither).
  [[nodiscard]] const Order* find_order(OrderId id) const { return orders_.find(id); }

  template <class OnExec>
  Result replace(OrderId old_id, OrderId new_id, Price new_price, Qty new_qty,
                 OnExec&& on_exec) {
    Order* old = orders_.find(old_id);
    if (old == nullptr) return Result::RejectedUnknownId;
    // Validate the new order completely before destroying the old one, so a
    // rejected replace leaves the book untouched.
    if (new_id == 0) return Result::RejectedBadId;
    if (new_qty == 0) return Result::RejectedBadQty;
    if (!ladder_.valid_price(new_price)) return Result::RejectedBadPrice;
    if (new_id != old_id && orders_.find(new_id) != nullptr) return Result::RejectedDuplicateId;
    const Side side = old->side;
    cancel_open(old);
    return add_limit(new_id, side, new_price, new_qty, std::forward<OnExec>(on_exec));
  }

  Result replace(OrderId old_id, OrderId new_id, Price new_price, Qty new_qty) {
    return replace(old_id, new_id, new_price, new_qty, [](const Execution&) {});
  }

  // --- inspection -------------------------------------------------------

  [[nodiscard]] const Level* best(Side s) const noexcept { return ladder_.best(s); }
  [[nodiscard]] std::size_t open_orders() const noexcept { return orders_.size(); }
  [[nodiscard]] const Counters& counters() const noexcept { return counters_; }

  template <class F>
  void for_each_level(Side s, F&& f) const {
    ladder_.for_each_level(s, std::forward<F>(f));
  }

  // Aggregated L2 snapshot, best levels first. Allocates — this is a
  // reporting call for strategy code and tooling, not a hot-path operation.
  [[nodiscard]] Depth depth(std::size_t max_levels) const {
    Depth d;
    d.bids.reserve(max_levels);
    d.asks.reserve(max_levels);
    collect(Side::Bid, max_levels, d.bids);
    collect(Side::Ask, max_levels, d.asks);
    return d;
  }

  // Deterministic fingerprint of the full book: every level best -> worst,
  // every resting order in FIFO order, hashed platform-independently. Books
  // that processed equivalent histories hash identically — regardless of
  // which ladder or id-map policy they run on.
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
  [[nodiscard]] Result validate(OrderId id, Price price, Qty qty) const {
    if (id == 0) return Result::RejectedBadId;
    if (qty == 0) return Result::RejectedBadQty;
    if (!ladder_.valid_price(price)) return Result::RejectedBadPrice;
    if (orders_.find(id) != nullptr) return Result::RejectedDuplicateId;
    return Result::Ok;
  }

  // Consume the opposite side while the aggressor's limit crosses; returns
  // the quantity left unfilled. HasLimit is a compile-time switch so market
  // orders share this code without putting a branch on the limit-order path.
  template <bool HasLimit, class OnExec>
  Qty match(OrderId aggressor, Side side, Price price, Qty qty, OnExec& on_exec) {
    const Side opp = opposite(side);
    while (qty > 0) {
      Level* lvl = ladder_.best(opp);
      if (lvl == nullptr) break;
      if constexpr (HasLimit) {
        const bool crosses = side == Side::Bid ? price >= lvl->price : price <= lvl->price;
        if (!crosses) break;
      }
      while (qty > 0 && lvl->head != nullptr) {
        Order* resting = lvl->head;
        const Qty fill = qty < resting->qty ? qty : resting->qty;
        lvl->reduce(resting, fill);
        qty -= fill;
        counters_.traded_qty += fill;
        on_exec(Execution{resting->id, aggressor, lvl->price, fill});
        if (resting->qty == 0) {
          lvl->remove(resting);
          orders_.erase(resting->id);
          pool_.release(resting);
        }
      }
      if (lvl->order_count == 0) ladder_.on_level_empty(opp, lvl);
    }
    return qty;
  }

  // Quantity resting on the opposite side that this order could reach, capped
  // at `need` so a deep book is not walked further than necessary.
  template <bool HasLimit>
  [[nodiscard]] std::uint64_t available(Side side, Price price, Qty need) const {
    std::uint64_t total = 0;
    ladder_.for_each_level(opposite(side), [&](const Level& lvl) -> bool {
      if constexpr (HasLimit) {
        const bool crosses = side == Side::Bid ? price >= lvl.price : price <= lvl.price;
        if (!crosses) return false;
      }
      total += lvl.total_qty;
      return total < need;
    });
    return total;
  }

  void rest(OrderId id, Side side, Price price, Qty qty) {
    Level* lvl = ladder_.get_or_create(side, price);
    Order* o = pool_.alloc();
    o->id = id;
    o->price = price;
    o->qty = qty;
    o->side = side;
    lvl->push_back(o);
    orders_.insert(id, o);
  }

  // Shared body of execute_resting / reduce_resting.
  Result consume(OrderId id, Qty qty, bool is_execution, Qty* out) {
    if (out != nullptr) *out = 0;
    Order* o = orders_.find(id);
    if (o == nullptr) return Result::RejectedUnknownId;
    if (qty == 0) return Result::RejectedBadQty;
    // A well-formed feed never over-consumes an order, but a truncated or
    // filtered stream can, so clamp rather than underflow the quantity.
    const Qty take = qty < o->qty ? qty : o->qty;
    if (out != nullptr) *out = take;
    if (is_execution) {
      counters_.executed_qty += take;
    } else {
      counters_.canceled_qty += take;
    }
    Level* lvl = o->level;
    if (take == o->qty) {
      const Side side = o->side;
      lvl->remove(o);
      orders_.erase(o->id);
      pool_.release(o);
      if (lvl->order_count == 0) ladder_.on_level_empty(side, lvl);
    } else {
      lvl->reduce(o, take);
    }
    return Result::Ok;
  }

  void cancel_open(Order* o) {
    Level* lvl = o->level;
    const Side side = o->side;
    counters_.canceled_qty += o->qty;
    lvl->remove(o);
    orders_.erase(o->id);
    pool_.release(o);
    if (lvl->order_count == 0) ladder_.on_level_empty(side, lvl);
  }

  void collect(Side s, std::size_t max_levels, std::vector<DepthLevel>& out) const {
    if (max_levels == 0) return;
    ladder_.for_each_level(s, [&](const Level& lvl) -> bool {
      out.push_back(DepthLevel{lvl.price, lvl.total_qty, lvl.order_count});
      return out.size() < max_levels;
    });
  }

  Ladder ladder_;
  OrderPool pool_;
  IdMap orders_;
  Counters counters_;
};

}  // namespace pricetime
