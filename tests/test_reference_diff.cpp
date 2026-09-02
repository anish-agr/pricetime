// The strongest correctness test in the repo: the real book, in every policy
// combination, must agree with an independently written naive model on both
// the execution stream and the resulting book state, op by op.
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "op_stream.hpp"
#include "pricetime/book.hpp"
#include "pricetime/id_map.hpp"
#include "pricetime/ladder_dense.hpp"
#include "pricetime/ladder_map.hpp"
#include "pricetime/op.hpp"
#include "reference_book.hpp"

using namespace pricetime;
using pricetime::test::OpStreamGenerator;
using pricetime::test::ReferenceBook;

namespace {

constexpr Price kMid = 1000;
constexpr Price kBand = 40;
constexpr Price kMin = 900;
constexpr Price kMax = 1100;
constexpr std::size_t kOps = 40000;

struct ExecLog {
  std::vector<Execution> execs;
  void operator()(const Execution& e) { execs.push_back(e); }
  void clear() { execs.clear(); }
};

bool same(const Execution& a, const Execution& b) {
  return a.resting_id == b.resting_id && a.aggressor_id == b.aggressor_id &&
         a.price == b.price && a.qty == b.qty;
}

// Runs one op against both books and requires identical observable behaviour:
// same result code, same executions in the same order, same state fingerprint.
template <class Book>
void compare_streams(Book& fast, ReferenceBook& ref, const std::vector<Op>& ops) {
  ExecLog fast_log;
  ExecLog ref_log;
  for (std::size_t i = 0; i < ops.size(); ++i) {
    const Op& op = ops[i];
    fast_log.clear();
    ref_log.clear();
    const Result fast_r = apply(fast, op, fast_log);
    const Result ref_r = apply(ref, op, ref_log);

    if (fast_r != ref_r || fast_log.execs.size() != ref_log.execs.size()) {
      INFO("divergence at op index ", i, " type ", static_cast<int>(op.type), " id ", op.id);
      REQUIRE(fast_r == ref_r);
      REQUIRE(fast_log.execs.size() == ref_log.execs.size());
    }
    for (std::size_t k = 0; k < fast_log.execs.size(); ++k) {
      if (!same(fast_log.execs[k], ref_log.execs[k])) {
        INFO("execution ", k, " differs at op index ", i);
        REQUIRE(same(fast_log.execs[k], ref_log.execs[k]));
      }
    }
    // Hashing every op is expensive but catches a divergence on the op that
    // caused it rather than thousands of ops later.
    if (fast.state_hash() != ref.state_hash()) {
      INFO("state hash diverged after op index ", i);
      REQUIRE(fast.state_hash() == ref.state_hash());
    }
  }
  REQUIRE(fast.open_orders() == ref.open_orders());
  REQUIRE(fast.counters().added_qty == ref.added_qty());
  REQUIRE(fast.counters().traded_qty == ref.traded_qty());
  REQUIRE(fast.counters().canceled_qty == ref.canceled_qty());
}

}  // namespace

TEST_CASE("reference differential: dense ladder + open-address id map") {
  OrderBook<DenseLadder, OpenAddressIdMap> fast{kMin, kMax};
  ReferenceBook ref{kMin, kMax};
  const auto ops = OpStreamGenerator(0xA11CE, kMid, kBand).generate(kOps);
  compare_streams(fast, ref, ops);
  CHECK(fast.open_orders() > 0);
  CHECK(fast.counters().traded_qty > 0);  // the stream really did trade
}

TEST_CASE("reference differential: map ladder + std id map") {
  OrderBook<MapLadder, StdIdMap> fast;
  ReferenceBook ref;
  const auto ops = OpStreamGenerator(0xB0B, kMid, kBand).generate(kOps);
  compare_streams(fast, ref, ops);
}

TEST_CASE("reference differential: dense ladder + std id map") {
  OrderBook<DenseLadder, StdIdMap> fast{kMin, kMax};
  ReferenceBook ref{kMin, kMax};
  const auto ops = OpStreamGenerator(0xC0DE, kMid, kBand).generate(kOps);
  compare_streams(fast, ref, ops);
}

TEST_CASE("reference differential: map ladder + open-address id map") {
  OrderBook<MapLadder, OpenAddressIdMap> fast;
  ReferenceBook ref;
  const auto ops = OpStreamGenerator(0xD00D, kMid, kBand).generate(kOps);
  compare_streams(fast, ref, ops);
}

// A narrow band makes almost every add cross, so the stream is dominated by
// multi-level sweeps, level exhaustion, and best-price recomputation, the
// paths where the dense ladder's rescan and the map's erase behave least alike.
TEST_CASE("reference differential: heavy-crossing stream") {
  OrderBook<DenseLadder, OpenAddressIdMap> fast{kMin, kMax};
  ReferenceBook ref{kMin, kMax};
  const auto ops = OpStreamGenerator(0xFEED, kMid, 4).generate(kOps);
  compare_streams(fast, ref, ops);
  CHECK(fast.counters().traded_qty > 0);
}

// Aggressive orders only: the book is repeatedly emptied and refilled, which
// is where best-price tracking is most likely to go wrong.
TEST_CASE("reference differential: IOC/FOK/market-heavy stream") {
  pricetime::test::StreamMix mix;
  mix.add_limit = 40;
  mix.cancel = 10;
  mix.replace = 5;
  mix.ioc = 15;
  mix.fok = 15;
  mix.market = 15;
  OrderBook<DenseLadder, OpenAddressIdMap> fast{kMin, kMax};
  ReferenceBook ref{kMin, kMax};
  const auto ops = OpStreamGenerator(0x5EED, kMid, kBand, mix).generate(kOps);
  compare_streams(fast, ref, ops);
}
