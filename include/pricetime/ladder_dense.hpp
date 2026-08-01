#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "pricetime/level.hpp"
#include "pricetime/types.hpp"

namespace pricetime {

// Contiguous price ladder: per side, a vector<Level> indexed by (price - min).
// Level lookup is one subtraction and one array access; the best price is a
// cached index. The costs of the layout are explicit: memory scales with the
// configured price range, prices outside [min, max] are rejected (the book
// checks valid_price() before matching), and when the best level empties we
// scan toward worse prices for the next active one — rare, and short in
// practice because activity clusters near the touch, but worst-case O(range).
// Level addresses are stable: the vectors are sized once and never resized.
class DenseLadder {
 public:
  DenseLadder(Price min_price, Price max_price) : min_(min_price), max_(max_price) {
    assert(max_price >= min_price);
    const auto span = static_cast<std::size_t>(max_ - min_ + 1);
    sides_[0].levels.resize(span);
    sides_[1].levels.resize(span);
  }

  [[nodiscard]] bool valid_price(Price p) const noexcept { return p >= min_ && p <= max_; }

  Level* get_or_create(Side s, Price p) {
    SideLadder& sl = sides_[index_of(s)];
    const std::int64_t idx = p - min_;
    Level& lvl = sl.levels[static_cast<std::size_t>(idx)];
    if (lvl.order_count == 0) {  // activating an empty slot
      lvl.price = p;
      ++sl.active_levels;
      if (sl.best == kNone || better(s, idx, sl.best)) sl.best = idx;
    }
    return &lvl;
  }

  void on_level_empty(Side s, Level* lvl) noexcept {
    SideLadder& sl = sides_[index_of(s)];
    const std::int64_t idx = lvl->price - min_;
    --sl.active_levels;
    if (sl.active_levels == 0) {
      sl.best = kNone;
      return;
    }
    if (idx != sl.best) return;
    // The best level emptied: scan toward worse prices. Terminates because at
    // least one worse level on this side is still active.
    std::int64_t i = sl.best;
    if (s == Side::Bid) {
      do {
        --i;
      } while (sl.levels[static_cast<std::size_t>(i)].order_count == 0);
    } else {
      do {
        ++i;
      } while (sl.levels[static_cast<std::size_t>(i)].order_count == 0);
    }
    sl.best = i;
  }

  [[nodiscard]] Level* best(Side s) noexcept {
    SideLadder& sl = sides_[index_of(s)];
    return sl.best == kNone ? nullptr : &sl.levels[static_cast<std::size_t>(sl.best)];
  }

  [[nodiscard]] const Level* best(Side s) const noexcept {
    const SideLadder& sl = sides_[index_of(s)];
    return sl.best == kNone ? nullptr : &sl.levels[static_cast<std::size_t>(sl.best)];
  }

  // Visit levels best -> worst. Walks the array from the best index, skipping
  // inactive slots, until every active level has been seen. A callback
  // returning bool stops the walk when it returns false.
  template <class F>
  void for_each_level(Side s, F&& f) const {
    const SideLadder& sl = sides_[index_of(s)];
    if (sl.best == kNone) return;
    const std::int64_t step = s == Side::Bid ? -1 : 1;
    std::uint32_t seen = 0;
    for (std::int64_t i = sl.best; seen < sl.active_levels; i += step) {
      const Level& lvl = sl.levels[static_cast<std::size_t>(i)];
      if (lvl.order_count != 0) {
        if constexpr (std::is_invocable_r_v<bool, F&, const Level&>) {
          if (!f(lvl)) return;
        } else {
          f(lvl);
        }
        ++seen;
      }
    }
  }

 private:
  static constexpr std::int64_t kNone = -1;

  static std::size_t index_of(Side s) noexcept { return static_cast<std::size_t>(s); }

  static bool better(Side s, std::int64_t a, std::int64_t b) noexcept {
    return s == Side::Bid ? a > b : a < b;
  }

  struct SideLadder {
    std::vector<Level> levels;
    std::int64_t best = kNone;
    std::uint32_t active_levels = 0;
  };

  Price min_;
  Price max_;
  SideLadder sides_[2];
};

}  // namespace pricetime
