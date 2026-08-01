#pragma once

#include <cstdint>

#include "pricetime/types.hpp"

namespace pricetime {

// A price level: FIFO queue of resting orders (head = oldest = first to fill)
// plus aggregates. Orders are intrusively linked, so queue operations never
// allocate and cancel-by-node is O(1).
struct Level {
  Price price = 0;
  std::uint64_t total_qty = 0;
  std::uint32_t order_count = 0;
  Order* head = nullptr;
  Order* tail = nullptr;

  [[nodiscard]] bool empty() const noexcept { return order_count == 0; }

  void push_back(Order* o) noexcept {
    o->prev = tail;
    o->next = nullptr;
    o->level = this;
    if (tail != nullptr) {
      tail->next = o;
    } else {
      head = o;
    }
    tail = o;
    ++order_count;
    total_qty += o->qty;
  }

  // Partial fill: shrink an order's remaining quantity in place. Keeps queue
  // position (a partially filled order retains its time priority).
  void reduce(Order* o, Qty delta) noexcept {
    o->qty -= delta;
    total_qty -= delta;
  }

  // Unlink an order, subtracting whatever quantity it still carries.
  void remove(Order* o) noexcept {
    if (o->prev != nullptr) {
      o->prev->next = o->next;
    } else {
      head = o->next;
    }
    if (o->next != nullptr) {
      o->next->prev = o->prev;
    } else {
      tail = o->prev;
    }
    --order_count;
    total_qty -= o->qty;
    o->prev = nullptr;
    o->next = nullptr;
    o->level = nullptr;
  }
};

}  // namespace pricetime
