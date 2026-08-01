#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "pricetime/book.hpp"
#include "pricetime/hash.hpp"
#include "pricetime/types.hpp"

namespace pricetime::test {

// An obviously-correct order book. Every operation is a linear scan over a
// flat vector of live orders; there is no ladder, no id map, no pool, no
// intrusive linkage — nothing shared with the real implementation.
//
// This exists because the dense-vs-map differential test has a blind spot:
// both fast books run the SAME matching loop, so they can only disagree about
// ladder behaviour. A semantic bug in the matching itself — filling in the
// wrong order, mispricing a fill, mishandling an IOC remainder — would appear
// identically in both and pass. Checking against an independent model written
// straight from the definition of price-time priority is what closes that gap.
//
// Correspondingly, this code optimizes for being visibly right: "the next
// order to fill is the one with the best price, and among those the one that
// arrived first" is written as exactly that search.
class ReferenceBook {
 public:
  explicit ReferenceBook(Price min_price, Price max_price)
      : min_(min_price), max_(max_price) {}

  ReferenceBook() : min_(0), max_(0), bounded_(false) {}

  template <class OnExec>
  Result add_limit(OrderId id, Side side, Price price, Qty qty, OnExec&& on_exec) {
    if (const Result r = validate(id, price, qty); r != Result::Ok) return r;
    added_qty_ += qty;
    const Qty left = match(id, side, price, true, qty, on_exec);
    if (left > 0) rest(id, side, price, left);
    return Result::Ok;
  }

  Result add_limit(OrderId id, Side side, Price price, Qty qty) {
    return add_limit(id, side, price, qty, [](const Execution&) {});
  }

  template <class OnExec>
  Result add_ioc(OrderId id, Side side, Price price, Qty qty, OnExec&& on_exec) {
    if (const Result r = validate(id, price, qty); r != Result::Ok) return r;
    added_qty_ += qty;
    canceled_qty_ += match(id, side, price, true, qty, on_exec);
    return Result::Ok;
  }

  Result add_ioc(OrderId id, Side side, Price price, Qty qty) {
    return add_ioc(id, side, price, qty, [](const Execution&) {});
  }

  template <class OnExec>
  Result add_fok(OrderId id, Side side, Price price, Qty qty, OnExec&& on_exec) {
    if (const Result r = validate(id, price, qty); r != Result::Ok) return r;
    std::uint64_t reachable = 0;
    for (const RefOrder& o : live_) {
      if (o.side == side) continue;
      const bool crosses = side == Side::Bid ? price >= o.price : price <= o.price;
      if (crosses) reachable += o.qty;
    }
    if (reachable < qty) return Result::RejectedNoLiquidity;
    added_qty_ += qty;
    canceled_qty_ += match(id, side, price, true, qty, on_exec);
    return Result::Ok;
  }

  Result add_fok(OrderId id, Side side, Price price, Qty qty) {
    return add_fok(id, side, price, qty, [](const Execution&) {});
  }

  template <class OnExec>
  Result add_market(OrderId id, Side side, Qty qty, OnExec&& on_exec) {
    if (id == 0) return Result::RejectedBadId;
    if (qty == 0) return Result::RejectedBadQty;
    if (find(id) != nullptr) return Result::RejectedDuplicateId;
    added_qty_ += qty;
    canceled_qty_ += match(id, side, 0, false, qty, on_exec);
    return Result::Ok;
  }

  Result add_market(OrderId id, Side side, Qty qty) {
    return add_market(id, side, qty, [](const Execution&) {});
  }

  Result cancel(OrderId id) {
    RefOrder* o = find(id);
    if (o == nullptr) return Result::RejectedUnknownId;
    canceled_qty_ += o->qty;
    erase(id);
    return Result::Ok;
  }

  template <class OnExec>
  Result replace(OrderId old_id, OrderId new_id, Price new_price, Qty new_qty,
                 OnExec&& on_exec) {
    RefOrder* old = find(old_id);
    if (old == nullptr) return Result::RejectedUnknownId;
    if (new_id == 0) return Result::RejectedBadId;
    if (new_qty == 0) return Result::RejectedBadQty;
    if (!valid_price(new_price)) return Result::RejectedBadPrice;
    if (new_id != old_id && find(new_id) != nullptr) return Result::RejectedDuplicateId;
    const Side side = old->side;
    canceled_qty_ += old->qty;
    erase(old_id);
    return add_limit(new_id, side, new_price, new_qty, std::forward<OnExec>(on_exec));
  }

  Result replace(OrderId old_id, OrderId new_id, Price new_price, Qty new_qty) {
    return replace(old_id, new_id, new_price, new_qty, [](const Execution&) {});
  }

  [[nodiscard]] std::size_t open_orders() const noexcept { return live_.size(); }
  [[nodiscard]] std::uint64_t added_qty() const noexcept { return added_qty_; }
  [[nodiscard]] std::uint64_t traded_qty() const noexcept { return traded_qty_; }
  [[nodiscard]] std::uint64_t canceled_qty() const noexcept { return canceled_qty_; }

  // Byte-for-byte the same fingerprint scheme as OrderBook::state_hash():
  // per side, levels best -> worst, orders within a level in arrival order.
  [[nodiscard]] std::uint64_t state_hash() const {
    Fnv1a64 h;
    for (const Side s : {Side::Bid, Side::Ask}) {
      h.mix(static_cast<std::uint64_t>(s) + 1);
      std::map<Price, std::vector<const RefOrder*>> levels;
      for (const RefOrder& o : live_) {
        if (o.side == s) levels[o.price].push_back(&o);
      }
      std::vector<Price> prices;
      prices.reserve(levels.size());
      for (const auto& kv : levels) prices.push_back(kv.first);
      if (s == Side::Bid) std::reverse(prices.begin(), prices.end());  // best = highest
      for (const Price p : prices) {
        std::vector<const RefOrder*>& os = levels[p];
        std::sort(os.begin(), os.end(),
                  [](const RefOrder* a, const RefOrder* b) { return a->seq < b->seq; });
        std::uint64_t total = 0;
        for (const RefOrder* o : os) total += o->qty;
        h.mix(static_cast<std::uint64_t>(p));
        h.mix(total);
        h.mix(static_cast<std::uint32_t>(os.size()));
        for (const RefOrder* o : os) {
          h.mix(o->id);
          h.mix(o->qty);
        }
      }
    }
    return h.value;
  }

 private:
  struct RefOrder {
    OrderId id = 0;
    Price price = 0;
    Qty qty = 0;
    Side side = Side::Bid;
    std::uint64_t seq = 0;  // arrival order == time priority
  };

  [[nodiscard]] bool valid_price(Price p) const noexcept {
    return !bounded_ || (p >= min_ && p <= max_);
  }

  [[nodiscard]] Result validate(OrderId id, Price price, Qty qty) const {
    if (id == 0) return Result::RejectedBadId;
    if (qty == 0) return Result::RejectedBadQty;
    if (!valid_price(price)) return Result::RejectedBadPrice;
    if (find(id) != nullptr) return Result::RejectedDuplicateId;
    return Result::Ok;
  }

  RefOrder* find(OrderId id) {
    for (RefOrder& o : live_) {
      if (o.id == id) return &o;
    }
    return nullptr;
  }

  const RefOrder* find(OrderId id) const {
    for (const RefOrder& o : live_) {
      if (o.id == id) return &o;
    }
    return nullptr;
  }

  void erase(OrderId id) {
    for (std::size_t i = 0; i < live_.size(); ++i) {
      if (live_[i].id == id) {
        live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
      }
    }
  }

  // The definition of price-time priority, written literally: among orders on
  // the opposite side that this order can trade with, take the best price,
  // and among those the earliest arrival.
  std::size_t best_counterparty(Side side, Price price, bool has_limit) const {
    std::size_t best = live_.size();
    for (std::size_t i = 0; i < live_.size(); ++i) {
      const RefOrder& o = live_[i];
      if (o.side == side) continue;
      if (has_limit) {
        const bool crosses = side == Side::Bid ? price >= o.price : price <= o.price;
        if (!crosses) continue;
      }
      if (best == live_.size()) {
        best = i;
        continue;
      }
      const RefOrder& b = live_[best];
      const bool better_price = side == Side::Bid ? o.price < b.price : o.price > b.price;
      if (better_price || (o.price == b.price && o.seq < b.seq)) best = i;
    }
    return best;
  }

  template <class OnExec>
  Qty match(OrderId aggressor, Side side, Price price, bool has_limit, Qty qty,
            OnExec& on_exec) {
    while (qty > 0) {
      const std::size_t i = best_counterparty(side, price, has_limit);
      if (i == live_.size()) break;
      RefOrder& resting = live_[i];
      const Qty fill = qty < resting.qty ? qty : resting.qty;
      resting.qty -= fill;
      qty -= fill;
      traded_qty_ += fill;
      on_exec(Execution{resting.id, aggressor, resting.price, fill});
      if (resting.qty == 0) live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return qty;
  }

  void rest(OrderId id, Side side, Price price, Qty qty) {
    live_.push_back(RefOrder{id, price, qty, side, next_seq_++});
  }

  std::vector<RefOrder> live_;
  std::uint64_t next_seq_ = 0;
  std::uint64_t added_qty_ = 0;
  std::uint64_t traded_qty_ = 0;
  std::uint64_t canceled_qty_ = 0;
  Price min_ = 0;
  Price max_ = 0;
  bool bounded_ = true;
};

}  // namespace pricetime::test
