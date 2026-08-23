#pragma once

#include <cstdint>
#include <unordered_map>

#include "pricetime/types.hpp"

namespace pricetime {

// Exact queue-position tracking for one simulated passive order.
//
// The optimistic fill model — "any print at our price fills us" — is the
// single largest lie a passive backtest can tell, because a real order waits
// behind every share that was already resting at its price. This tracker
// removes that lie using information the feed genuinely provides:
//
//   When our simulated order joins a level, every real order resting there
//   at that moment is AHEAD of us (we join the back of the FIFO). The feed
//   then reports, order by order, everything that happens to them: executed,
//   canceled, deleted, replaced away. When the recorded set is empty, we are
//   at the front of the queue, and the next incoming marketable order is
//   ours.
//
// This is exact, not approximate: ITCH identifies every displayed order, so
// "who is ahead of me" is knowable, which is precisely why exchanges' own
// members reconstruct books this way.
//
// One approximation remains, and it is worth stating: once we are at the
// front, a real order would absorb the incoming aggressor INSTEAD of the
// order behind us; in replay, that aggressor still consumes the real order
// it historically hit. Our simulated fills therefore slightly double-count
// liquidity at our level. Fixing that requires counterfactual replay (the
// book diverges from history the moment we participate), which changes the
// question the sandbox answers.
class QueuePosition {
 public:
  // Begin tracking a fresh order at the back of a level. `ahead` must be
  // filled by the caller with (id -> open qty) of every order currently
  // resting at the level.
  void place(Price price, Side side, Qty qty,
             std::unordered_map<OrderId, Qty>&& ahead) {
    price_ = price;
    side_ = side;
    remaining_ = qty;
    ahead_ = std::move(ahead);
    shares_ahead_ = 0;
    for (const auto& kv : ahead_) shares_ahead_ += kv.second;
    active_ = true;
  }

  void cancel() {
    active_ = false;
    ahead_.clear();
    shares_ahead_ = 0;
  }

  [[nodiscard]] bool active() const noexcept { return active_; }
  [[nodiscard]] Price price() const noexcept { return price_; }
  [[nodiscard]] Side side() const noexcept { return side_; }
  [[nodiscard]] Qty remaining() const noexcept { return remaining_; }
  [[nodiscard]] std::uint64_t shares_ahead() const noexcept { return shares_ahead_; }
  [[nodiscard]] bool at_front() const noexcept { return active_ && ahead_.empty(); }

  // The feed removed `qty` shares from order `id` (execution, partial
  // cancel, delete, or replace-away — the reason does not matter to the
  // queue). No-op for orders we are not behind.
  void on_shares_removed(OrderId id, Qty qty) {
    const auto it = ahead_.find(id);
    if (it == ahead_.end()) return;
    const Qty cut = qty < it->second ? qty : it->second;
    it->second -= cut;
    shares_ahead_ -= cut;
    if (it->second == 0) ahead_.erase(it);
  }

  // An execution printed at our price on our side while we were at the
  // front: the marketable flow reaches us. Returns the quantity we fill.
  [[nodiscard]] Qty take_fill(Qty print_qty) {
    const Qty fill = print_qty < remaining_ ? print_qty : remaining_;
    remaining_ -= fill;
    if (remaining_ == 0) active_ = false;
    return fill;
  }

 private:
  Price price_ = 0;
  Side side_ = Side::Bid;
  Qty remaining_ = 0;
  std::uint64_t shares_ahead_ = 0;
  std::unordered_map<OrderId, Qty> ahead_;
  bool active_ = false;
};

}  // namespace pricetime
