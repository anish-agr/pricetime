#pragma once

#include <functional>
#include <map>
#include <type_traits>

#include "pricetime/level.hpp"
#include "pricetime/types.hpp"

namespace pricetime {

// Tree-backed price ladder: one std::map per side, keyed so that begin() is
// the best level (highest bid / lowest ask). O(log L) in the number of active
// levels; no bound on the price range. Level addresses are stable because map
// nodes never move.
class MapLadder {
 public:
  [[nodiscard]] static bool valid_price(Price) noexcept { return true; }

  Level* get_or_create(Side s, Price p) {
    return s == Side::Bid ? get_or_create_in(bids_, p) : get_or_create_in(asks_, p);
  }

  // The level must not be touched after this call (its node is destroyed).
  void on_level_empty(Side s, Level* lvl) {
    if (s == Side::Bid) {
      bids_.erase(lvl->price);
    } else {
      asks_.erase(lvl->price);
    }
  }

  [[nodiscard]] Level* best(Side s) noexcept {
    if (s == Side::Bid) return bids_.empty() ? nullptr : &bids_.begin()->second;
    return asks_.empty() ? nullptr : &asks_.begin()->second;
  }

  [[nodiscard]] const Level* best(Side s) const noexcept {
    if (s == Side::Bid) return bids_.empty() ? nullptr : &bids_.begin()->second;
    return asks_.empty() ? nullptr : &asks_.begin()->second;
  }

  // Visit levels best -> worst. A callback returning bool stops the walk when
  // it returns false.
  template <class F>
  void for_each_level(Side s, F&& f) const {
    if (s == Side::Bid) {
      walk(bids_, f);
    } else {
      walk(asks_, f);
    }
  }

 private:
  template <class Map, class F>
  static void walk(const Map& m, F& f) {
    for (const auto& kv : m) {
      if constexpr (std::is_invocable_r_v<bool, F&, const Level&>) {
        if (!f(kv.second)) return;
      } else {
        f(kv.second);
      }
    }
  }

  template <class Map>
  static Level* get_or_create_in(Map& m, Price p) {
    const auto [it, inserted] = m.try_emplace(p);
    if (inserted) it->second.price = p;
    return &it->second;
  }

  std::map<Price, Level, std::greater<Price>> bids_;  // begin() = highest = best bid
  std::map<Price, Level> asks_;                       // begin() = lowest = best ask
};

}  // namespace pricetime
