#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "pricetime/types.hpp"

namespace pricetime {

// Arena plus free list for Order nodes. Nodes live in fixed-size chunks that
// are never moved or freed, so Order* stay valid for the pool's lifetime; a
// fresh chunk is one allocation per kChunkSize orders. (std::deque would be
// the obvious stand-in, but MSVC's deque uses tiny blocks — one heap
// allocation per Order-sized element — which puts an allocator call on the
// hot path.) Released nodes are recycled LIFO, keeping recently touched
// memory warm.
class OrderPool {
 public:
  static constexpr std::size_t kChunkSize = 4096;

  Order* alloc() {
    if (!free_.empty()) {
      Order* o = free_.back();
      free_.pop_back();
      *o = Order{};
      return o;
    }
    if (used_in_last_ == kChunkSize) {
      chunks_.push_back(std::make_unique<Order[]>(kChunkSize));
      used_in_last_ = 0;
    }
    ++total_;
    return &chunks_.back()[used_in_last_++];
  }

  void release(Order* o) { free_.push_back(o); }

  // Total nodes ever handed out from chunks (recycled nodes are not re-counted).
  [[nodiscard]] std::size_t allocated() const noexcept { return total_; }

 private:
  std::vector<std::unique_ptr<Order[]>> chunks_;
  std::size_t used_in_last_ = kChunkSize;  // forces the first chunk on first alloc
  std::size_t total_ = 0;
  std::vector<Order*> free_;
};

}  // namespace pricetime
