#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <type_traits>
#include <utility>
#include <vector>

#include "pricetime/level.hpp"
#include "pricetime/types.hpp"

namespace pricetime {

// Tree ladder with node recycling, the fix suggested by the replay profile.
//
// Profiling full-day replay (docs/PROFILE.md) put 26% of runtime inside
// std::map node churn: real order flow creates a price level and empties it
// again 1.47M times a day, and every cycle is a tree-node allocate + free.
// The dense array avoids that but pays range-proportional memory (~160 MB
// per symbol at a feed-safe range), which loses even harder end-to-end.
//
// This ladder keeps the tree's O(active-levels) footprint and unbounded
// price range, and removes the churn using C++17 node handles: when a level
// empties, its node is extract()ed, detached from the tree without passing
// through the allocator, and parked in a cache. The next level creation
// re-keys a parked node and splices it back in. After warmup, level churn
// touches the allocator exactly zero times, and the zero-allocation test
// enforces that with a replaced operator new.
//
// Same observable semantics as MapLadder; the cross-ladder differential
// tests hold it to that.
class PooledMapLadder {
 public:
  [[nodiscard]] static bool valid_price(Price) noexcept { return true; }

  Level* get_or_create(Side s, Price p) {
    return s == Side::Bid ? get_in(bids_, bid_cache_, p) : get_in(asks_, ask_cache_, p);
  }

  // The level's node is parked, not destroyed; the Level object inside it is
  // dead to callers either way.
  void on_level_empty(Side s, Level* lvl) {
    if (s == Side::Bid) {
      park(bids_, bid_cache_, lvl->price);
    } else {
      park(asks_, ask_cache_, lvl->price);
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

  [[nodiscard]] std::size_t cached_nodes() const noexcept {
    return bid_cache_.size() + ask_cache_.size();
  }

 private:
  using BidMap = std::map<Price, Level, std::greater<Price>>;
  using AskMap = std::map<Price, Level>;

  // Unbounded caching would let one volatile morning pin memory all day;
  // beyond this many parked nodes per side, extras go back to the allocator.
  static constexpr std::size_t kMaxCached = 4096;

  template <class Map, class Cache>
  static Level* get_in(Map& m, Cache& cache, Price p) {
    const auto it = m.lower_bound(p);
    if (it != m.end() && it->first == p) return &it->second;
    if (!cache.empty()) {
      auto nh = std::move(cache.back());
      cache.pop_back();
      nh.key() = p;
      nh.mapped() = Level{};
      nh.mapped().price = p;
      // lower_bound already found the insertion point; splice the node there.
      const auto pos = m.insert(it, std::move(nh));
      return &pos->second;
    }
    const auto pos = m.emplace_hint(it, std::piecewise_construct, std::forward_as_tuple(p),
                                    std::forward_as_tuple());
    pos->second.price = p;
    return &pos->second;
  }

  template <class Map, class Cache>
  static void park(Map& m, Cache& cache, Price p) {
    auto nh = m.extract(p);
    if (!nh.empty() && cache.size() < kMaxCached) cache.push_back(std::move(nh));
    // A dropped handle frees its node on destruction, the bounded case.
  }

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

  BidMap bids_;
  AskMap asks_;
  std::vector<typename BidMap::node_type> bid_cache_;
  std::vector<typename AskMap::node_type> ask_cache_;
};

}  // namespace pricetime
