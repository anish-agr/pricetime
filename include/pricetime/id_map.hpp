#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "pricetime/types.hpp"

namespace pricetime {

// Order-id -> Order* index. Every cancel and replace starts here, so it is on
// the hot path for roughly half of all exchange traffic.
//
// Both implementations expose the same minimal interface, which is smaller
// than std::unordered_map's on purpose: the book only ever needs
//   Order* find(OrderId) const     returns nullptr when absent
//   bool   insert(OrderId, Order*) returns false if the id is already present
//   bool   erase(OrderId)          returns false when absent
//   size_t size() const
//   void   reserve(size_t)
//
// OrderId 0 is reserved as the empty-slot sentinel and is rejected by the
// book before it ever reaches a map, so the two policies stay observationally
// identical. (ITCH order reference numbers start at 1.)

// Baseline: the obvious std::unordered_map. Kept as a policy so the
// open-addressing version has something to be measured against.
class StdIdMap {
 public:
  [[nodiscard]] Order* find(OrderId id) const noexcept {
    const auto it = map_.find(id);
    return it == map_.end() ? nullptr : it->second;
  }

  bool insert(OrderId id, Order* value) { return map_.emplace(id, value).second; }

  bool erase(OrderId id) { return map_.erase(id) != 0; }

  [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }

  void reserve(std::size_t n) { map_.reserve(n); }

 private:
  std::unordered_map<OrderId, Order*> map_;
};

// Open addressing with linear probing and backward-shift deletion,
// generalized over the mapped pointer type: the book maps id -> Order*, and
// MultiBook maps id -> OrderBook* for feed routing. The second use exists
// because profiling the real full-day replay showed the std::unordered_map
// order->book index costing ~64 bytes per live order in node and bucket
// overhead, the same problem as the id map itself one layer up. Here a live
// order costs 16 bytes at 70% load.
//
// Why this beats the node-based map on this workload:
//  - one contiguous array, so a lookup touches one cache line in the common
//    case instead of chasing a bucket pointer to a separately allocated node;
//  - no per-insert allocation (std::unordered_map allocates a node per order);
//  - linear probing is the friendliest possible pattern for the prefetcher.
//
// Deletion uses Knuth's backward-shift (Algorithm R) rather than tombstones:
// on erase, later elements in the probe chain are pulled back into the hole
// when doing so keeps them reachable from their ideal slot. Tombstones would
// be simpler but degrade a long-running book: an exchange session cancels
// millions of orders, and every tombstone permanently lengthens some probe
// chain until a full rehash.
template <class V>
class OpenAddressMap {
  static_assert(std::is_pointer_v<V>, "values are pointers; nullptr marks an empty slot");

 public:
  static constexpr OrderId kEmpty = 0;
  static constexpr std::size_t kInitialCapacity = 1024;  // power of two

  OpenAddressMap() { rehash(kInitialCapacity); }

  [[nodiscard]] V find(OrderId id) const noexcept {
    std::size_t i = hash(id) & mask_;
    for (;;) {
      const Slot& s = slots_[i];
      if (s.key == id) return s.value;
      if (s.key == kEmpty) return nullptr;
      i = (i + 1) & mask_;
    }
  }

  bool insert(OrderId id, V value) {
    // Grow at 70% load: linear probing degrades sharply above that, and the
    // power-of-two capacity keeps the modulo a single AND.
    if ((size_ + 1) * 10 >= slots_.size() * 7) rehash(slots_.size() * 2);
    std::size_t i = hash(id) & mask_;
    while (slots_[i].key != kEmpty) {
      if (slots_[i].key == id) return false;
      i = (i + 1) & mask_;
    }
    slots_[i] = Slot{id, value};
    ++size_;
    return true;
  }

  bool erase(OrderId id) {
    std::size_t i = hash(id) & mask_;
    for (;;) {
      if (slots_[i].key == id) break;
      if (slots_[i].key == kEmpty) return false;
      i = (i + 1) & mask_;
    }
    erase_at(i);
    return true;
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  void reserve(std::size_t n) {
    std::size_t want = kInitialCapacity;
    while (want * 7 < n * 10) want *= 2;  // keep n under the 70% load factor
    if (want > slots_.size()) rehash(want);
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

 private:
  struct Slot {
    OrderId key = kEmpty;
    V value = nullptr;
  };

  // splitmix64's finalizer: cheap (three multiply-shift rounds) and avalanches
  // well. Sequential ids would probe fine under an identity hash, but ITCH
  // reference numbers are only *mostly* sequential and a clustered stride
  // would silently wreck linear probing.
  [[nodiscard]] static std::size_t hash(OrderId id) noexcept {
    std::uint64_t z = id + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return static_cast<std::size_t>(z ^ (z >> 31));
  }

  // Knuth 6.4 Algorithm R. Hole at `i`; scan forward for an element whose
  // ideal slot `k` makes it still findable if moved back into the hole, i.e.
  // the hole lies cyclically within [k, j]. Distances measured from k make
  // that a single comparison.
  void erase_at(std::size_t i) noexcept {
    std::size_t j = i;
    for (;;) {
      slots_[i].key = kEmpty;
      slots_[i].value = nullptr;
      for (;;) {
        j = (j + 1) & mask_;
        if (slots_[j].key == kEmpty) {
          --size_;
          return;
        }
        const std::size_t k = hash(slots_[j].key) & mask_;
        const std::size_t hole_off = (i - k) & mask_;
        const std::size_t cur_off = (j - k) & mask_;
        if (hole_off <= cur_off) break;  // safe to pull slots_[j] back to i
      }
      slots_[i] = slots_[j];
      i = j;
    }
  }

  void rehash(std::size_t new_cap) {
    std::vector<Slot> old;
    old.swap(slots_);
    slots_.assign(new_cap, Slot{});
    mask_ = new_cap - 1;
    for (const Slot& s : old) {
      if (s.key == kEmpty) continue;
      std::size_t i = hash(s.key) & mask_;
      while (slots_[i].key != kEmpty) i = (i + 1) & mask_;
      slots_[i] = s;
    }
  }

  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  std::size_t size_ = 0;
};

using OpenAddressIdMap = OpenAddressMap<Order*>;

}  // namespace pricetime
