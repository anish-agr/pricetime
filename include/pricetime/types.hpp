#pragma once

#include <cstdint>

namespace pricetime {

// Prices are integer ticks. ITCH 5.0 quotes prices as unsigned 32-bit with four
// implied decimal places; int64 covers that with headroom and keeps arithmetic
// (spreads, offsets) safe from underflow.
using Price = std::int64_t;

// Share quantity. uint32 matches the ITCH wire format; aggregates that sum many
// orders use uint64.
using Qty = std::uint32_t;

using OrderId = std::uint64_t;

enum class Side : std::uint8_t { Bid = 0, Ask = 1 };

// Firm or account behind an order, used for self-trade prevention. Zero means
// unattributed and never triggers prevention, so flow that does not care about
// STP behaves exactly as it did before the field existed.
using ParticipantId = std::uint16_t;

// How long an aggressive order is allowed to live.
//  - GoodTillCancel:    any unfilled remainder rests in the book.
//  - ImmediateOrCancel: fill what you can right now, kill the rest.
//  - FillOrKill:        fill the whole quantity immediately or do nothing.
enum class TimeInForce : std::uint8_t { GoodTillCancel = 0, ImmediateOrCancel = 1, FillOrKill = 2 };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
  return s == Side::Bid ? Side::Ask : Side::Bid;
}

struct Level;

// Book order node. Lives in OrderPool; intrusively linked into its price
// level's FIFO queue.
struct Order {
  OrderId id = 0;
  Price price = 0;
  Qty qty = 0;  // remaining open quantity
  Side side = Side::Bid;
  // Sits in the padding that already followed `side`, so self-trade
  // prevention costs nothing per order. The assertion below keeps it that way
  // if the fields are ever reordered.
  ParticipantId participant = 0;
  Order* prev = nullptr;
  Order* next = nullptr;
  Level* level = nullptr;  // owning level; makes cancel O(1)
};

static_assert(sizeof(void*) != 8 || sizeof(Order) == 48,
              "Order should stay 48 bytes on 64-bit: participant must fit in padding");

// One fill: `qty` shares traded at `price` between a resting order and the
// incoming (aggressing) order.
struct Execution {
  OrderId resting_id = 0;
  OrderId aggressor_id = 0;
  Price price = 0;
  Qty qty = 0;
};

}  // namespace pricetime
