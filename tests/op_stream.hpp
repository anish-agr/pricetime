#pragma once

#include <cstdint>
#include <vector>

#include "pricetime/op.hpp"
#include "pricetime/types.hpp"

namespace pricetime::test {

// Generates a deterministic Op sequence with no book involved. Because the
// stream is fully materialized before anything consumes it, every book under
// test provably sees byte-identical input — unlike a generator that reacts to
// a book's own accept/reject decisions.
//
// The mix deliberately includes ops that must be rejected (stale cancels,
// duplicate ids) so rejection paths are exercised too, and every book must
// agree on which ones fail.
struct StreamMix {
  int add_limit = 55;
  int cancel = 22;
  int replace = 10;
  int ioc = 5;
  int fok = 4;
  int market = 4;  // remainder of 100
};

class OpStreamGenerator {
 public:
  OpStreamGenerator(std::uint64_t seed, Price mid, Price band, StreamMix mix = {})
      : state_(seed), mid_(mid), band_(band), mix_(mix) {}

  std::vector<Op> generate(std::size_t count) {
    std::vector<Op> ops;
    ops.reserve(count);
    for (std::size_t i = 0; i < count; ++i) ops.push_back(next());
    return ops;
  }

 private:
  std::uint64_t rng() {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  Side gen_side() { return rng() % 2 == 0 ? Side::Bid : Side::Ask; }
  Qty gen_qty() { return static_cast<Qty>(1 + rng() % 200); }

  // Bids and asks are drawn from overlapping bands so a meaningful fraction of
  // adds cross and execute rather than just resting.
  Price gen_price(Side s) {
    const Price off = static_cast<Price>(rng() % static_cast<std::uint64_t>(band_));
    return s == Side::Bid ? mid_ + band_ / 2 - off : mid_ - band_ / 2 + off;
  }

  // Targets a plausibly-live id, but reaches back far enough that many picks
  // are already gone — those must be rejected identically by every book.
  OrderId gen_target() {
    if (next_id_ <= 1) return 1;
    const std::uint64_t span = next_id_ - 1 < 512 ? next_id_ - 1 : 512;
    return next_id_ - 1 - rng() % span;
  }

  Op next() {
    Op op;
    const std::uint64_t roll = rng() % 100;
    int threshold = mix_.add_limit;
    if (roll < static_cast<std::uint64_t>(threshold)) {
      op.type = OpType::AddLimit;
      op.side = gen_side();
      op.price = gen_price(op.side);
      op.qty = gen_qty();
      op.id = next_id_++;
      return op;
    }
    threshold += mix_.cancel;
    if (roll < static_cast<std::uint64_t>(threshold)) {
      op.type = OpType::Cancel;
      op.id = gen_target();
      return op;
    }
    threshold += mix_.replace;
    if (roll < static_cast<std::uint64_t>(threshold)) {
      op.type = OpType::Replace;
      op.id = gen_target();
      op.side = gen_side();  // drawn for the price band only; the book keeps the real side
      op.price = gen_price(op.side);
      op.qty = gen_qty();
      op.new_id = next_id_++;
      return op;
    }
    threshold += mix_.ioc;
    if (roll < static_cast<std::uint64_t>(threshold)) {
      op.type = OpType::AddIoc;
      op.side = gen_side();
      op.price = gen_price(op.side);
      op.qty = gen_qty();
      op.id = next_id_++;
      return op;
    }
    threshold += mix_.fok;
    if (roll < static_cast<std::uint64_t>(threshold)) {
      op.type = OpType::AddFok;
      op.side = gen_side();
      op.price = gen_price(op.side);
      op.qty = gen_qty();
      op.id = next_id_++;
      return op;
    }
    op.type = OpType::AddMarket;
    op.side = gen_side();
    op.qty = gen_qty();
    op.id = next_id_++;
    return op;
  }

  std::uint64_t state_;
  Price mid_;
  Price band_;
  StreamMix mix_;
  OrderId next_id_ = 1;
};

}  // namespace pricetime::test
