#pragma once

#include <cstdint>
#include <vector>

#include "pricetime/types.hpp"

namespace pricetime {

// A deliberately naive market maker, and the accounting needed to see why it
// loses money.
//
// The strategy: quote a fixed number of ticks either side of the mid, skewing
// quotes against the current inventory so the position mean-reverts. That is
// about the simplest thing that can be called market making. It is not meant
// to be profitable; the P&L decomposition below is there to show where a
// naive quoter loses money.
//
// Accounting notes:
//
//  - Cash and position are integers. Cash is in price-ticks x shares, and is
//    converted only at the reporting boundary. Accumulating P&L in a double
//    across hundreds of thousands of fills loses cents to rounding, and cents
//    are most of the margin in a market-making strategy.
//  - Mark-to-market uses the mid, not the last trade. Marking at last trade
//    lets a single print on the far side flatter or wreck the reported P&L.
//  - Realized and unrealized are reported separately, because a strategy that
//    looks profitable while accumulating inventory is usually just short
//    volatility and has not paid for it yet.
struct FillEvent {
  Side side = Side::Bid;  // the side our order was on: Bid = we bought
  Price price = 0;
  Qty qty = 0;
  std::uint64_t timestamp = 0;
  Price mid_at_fill = 0;
};

struct MarketMakerConfig {
  Price half_spread_ticks = 5;   // how far off the mid we quote
  Qty quote_size = 100;          // shares per side
  std::int64_t max_position = 1000;  // stop quoting a side beyond this
  Price skew_per_100_shares = 1;  // ticks of quote skew per 100 shares held
};

// Markout: where the mid went after a fill. The core measure of adverse
// selection: if the mid consistently moves against us right after we trade,
// we are being picked off by better-informed flow, and spread capture alone
// will not save the strategy.
struct Markout {
  std::uint64_t horizon_ns = 0;
  double mean_ticks = 0;  // signed: positive = the market moved our way
  std::uint64_t samples = 0;
};

class MarketMaker {
 public:
  explicit MarketMaker(MarketMakerConfig cfg = {}) : cfg_(cfg) {}

  // Where we would quote given the current mid and inventory. Skewing means
  // a long position lowers both quotes, making us more likely to sell and
  // less likely to buy.
  [[nodiscard]] Price bid_quote(Price mid) const {
    return mid - cfg_.half_spread_ticks - skew();
  }

  [[nodiscard]] Price ask_quote(Price mid) const {
    return mid + cfg_.half_spread_ticks - skew();
  }

  // Position limits: refuse to add to a position that is already at its cap.
  [[nodiscard]] bool wants_bid() const { return position_ < cfg_.max_position; }
  [[nodiscard]] bool wants_ask() const { return position_ > -cfg_.max_position; }
  [[nodiscard]] Qty quote_size() const noexcept { return cfg_.quote_size; }

  void on_fill(const FillEvent& f) {
    const std::int64_t signed_qty =
        f.side == Side::Bid ? static_cast<std::int64_t>(f.qty) : -static_cast<std::int64_t>(f.qty);
    // Buying spends cash; selling receives it.
    cash_ -= signed_qty * static_cast<std::int64_t>(f.price);
    position_ += signed_qty;
    ++fills_;
    volume_ += f.qty;
    fills_log_.push_back(f);
  }

  // Called as the market moves, so markouts can be evaluated once each
  // horizon has elapsed. Mids must arrive in non-decreasing timestamp order.
  void on_mid(Price mid, std::uint64_t timestamp) {
    last_mid_ = mid;
    last_ts_ = timestamp;
    mid_history_.push_back(MidPoint{timestamp, mid});
  }

  [[nodiscard]] std::int64_t position() const noexcept { return position_; }
  [[nodiscard]] std::int64_t cash_ticks() const noexcept { return cash_; }
  [[nodiscard]] std::uint64_t fills() const noexcept { return fills_; }
  [[nodiscard]] std::uint64_t volume() const noexcept { return volume_; }
  [[nodiscard]] Price last_mid() const noexcept { return last_mid_; }

  // Total P&L in ticks x shares: cash plus inventory marked at the mid.
  [[nodiscard]] std::int64_t total_pnl_ticks(Price mark) const {
    return cash_ + position_ * static_cast<std::int64_t>(mark);
  }

  [[nodiscard]] std::int64_t inventory_value_ticks(Price mark) const {
    return position_ * static_cast<std::int64_t>(mark);
  }

  // Largest absolute position held; a blunt risk measure.
  [[nodiscard]] std::int64_t peak_abs_position() const {
    std::int64_t peak = 0;
    std::int64_t pos = 0;
    for (const FillEvent& f : fills_log_) {
      pos += f.side == Side::Bid ? static_cast<std::int64_t>(f.qty)
                                 : -static_cast<std::int64_t>(f.qty);
      const std::int64_t a = pos < 0 ? -pos : pos;
      if (a > peak) peak = a;
    }
    return peak;
  }

  // Average signed mid move over `horizon_ns` after each fill, in ticks, from
  // the perspective of the position taken. Negative means the market moved
  // against us: classic adverse selection.
  [[nodiscard]] Markout markout(std::uint64_t horizon_ns) const {
    Markout out;
    out.horizon_ns = horizon_ns;
    if (mid_history_.empty()) return out;
    double total = 0;
    std::uint64_t n = 0;
    for (const FillEvent& f : fills_log_) {
      const Price future = mid_at_or_after(f.timestamp + horizon_ns);
      if (future == kNoMid) continue;  // not enough history after this fill
      const double move = static_cast<double>(future - f.mid_at_fill);
      // A buy profits when the mid rises; a sell profits when it falls.
      total += f.side == Side::Bid ? move : -move;
      ++n;
    }
    out.samples = n;
    out.mean_ticks = n > 0 ? total / static_cast<double>(n) : 0.0;
    return out;
  }

  // Spread actually captured per share, ignoring what happened afterwards.
  // Compare against the markout: capturing 5 ticks of spread while suffering
  // a 6-tick adverse move is a losing strategy that looks busy.
  [[nodiscard]] double captured_ticks_per_share() const {
    if (volume_ == 0) return 0.0;
    double total = 0;
    for (const FillEvent& f : fills_log_) {
      const double edge = f.side == Side::Bid ? static_cast<double>(f.mid_at_fill - f.price)
                                              : static_cast<double>(f.price - f.mid_at_fill);
      total += edge * static_cast<double>(f.qty);
    }
    return total / static_cast<double>(volume_);
  }

  [[nodiscard]] const std::vector<FillEvent>& fill_log() const noexcept { return fills_log_; }

 private:
  static constexpr Price kNoMid = INT64_MIN;

  struct MidPoint {
    std::uint64_t ts = 0;
    Price mid = 0;
  };

  // Quote skew in ticks, opposing the current inventory.
  [[nodiscard]] Price skew() const {
    return static_cast<Price>(position_ * cfg_.skew_per_100_shares / 100);
  }

  // First recorded mid at or after `ts`. Binary search: mids are appended in
  // timestamp order.
  [[nodiscard]] Price mid_at_or_after(std::uint64_t ts) const {
    std::size_t lo = 0;
    std::size_t hi = mid_history_.size();
    while (lo < hi) {
      const std::size_t mid_idx = lo + (hi - lo) / 2;
      if (mid_history_[mid_idx].ts < ts) {
        lo = mid_idx + 1;
      } else {
        hi = mid_idx;
      }
    }
    return lo < mid_history_.size() ? mid_history_[lo].mid : kNoMid;
  }

  MarketMakerConfig cfg_;
  std::int64_t position_ = 0;
  std::int64_t cash_ = 0;
  std::uint64_t fills_ = 0;
  std::uint64_t volume_ = 0;
  Price last_mid_ = 0;
  std::uint64_t last_ts_ = 0;
  std::vector<FillEvent> fills_log_;
  std::vector<MidPoint> mid_history_;
};

}  // namespace pricetime
