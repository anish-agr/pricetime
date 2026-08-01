#pragma once

#include <cstddef>
#include <deque>
#include <vector>

#include "pricetime/types.hpp"

namespace pricetime {

// Arena plus free list for Order nodes. std::deque allocates in stable chunks,
// so Order* handed out here stay valid for the pool's lifetime while the arena
// grows. Released nodes are recycled LIFO (warm cache).
class OrderPool {
 public:
  Order* alloc() {
    if (!free_.empty()) {
      Order* o = free_.back();
      free_.pop_back();
      *o = Order{};
      return o;
    }
    return &storage_.emplace_back();
  }

  void release(Order* o) { free_.push_back(o); }

  [[nodiscard]] std::size_t allocated() const noexcept { return storage_.size(); }

 private:
  std::deque<Order> storage_;
  std::vector<Order*> free_;
};

}  // namespace pricetime
