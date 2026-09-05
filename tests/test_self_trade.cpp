#include <doctest/doctest.h>

#include <cstdint>
#include <random>
#include <vector>

#include "pricetime/book.hpp"
#include "pricetime/id_map.hpp"
#include "pricetime/ladder_map.hpp"
#include "pricetime/ladder_pooled.hpp"
#include "reference_book.hpp"

using namespace pricetime;
using pricetime::test::ReferenceBook;

namespace {

using Book = OrderBook<MapLadder, OpenAddressIdMap>;

// added == 2*traded + canceled + executed + still resting. Self-trade
// prevention removes quantity, so the shares it takes out have to land in
// canceled_qty or this fails.
template <class B>
std::uint64_t resting_qty(const B& book) {
  std::uint64_t resting = 0;
  for (const Side s : {Side::Bid, Side::Ask}) {
    book.for_each_level(s, [&](const Level& lvl) { resting += lvl.total_qty; });
  }
  return resting;
}

template <class B>
void check_conservation(const B& book) {
  const auto& c = book.counters();
  CHECK(c.added_qty == 2 * c.traded_qty + c.canceled_qty + c.executed_qty + resting_qty(book));
}

}  // namespace

TEST_CASE("self-trade: the default policy leaves matching untouched") {
  Book book;
  REQUIRE(book.self_trade_policy() == SelfTradePolicy::None);
  // Same participant on both sides, and they trade, because nothing asked for
  // prevention. This is what keeps every other test and the golden hash valid.
  REQUIRE(book.add_limit_as(7, 1, Side::Ask, 100, 500) == Result::Ok);
  REQUIRE(book.add_limit_as(7, 2, Side::Bid, 100, 500) == Result::Ok);
  CHECK(book.counters().traded_qty == 500);
  CHECK(book.counters().self_trades_prevented == 0);
  CHECK(book.open_orders() == 0);
  check_conservation(book);
}

TEST_CASE("self-trade: participant zero never triggers prevention") {
  Book book;
  book.set_self_trade_policy(SelfTradePolicy::CancelBoth);
  // Unattributed flow. Two orders with participant 0 are not "the same
  // participant"; they are two orders nobody claimed.
  REQUIRE(book.add_limit(1, Side::Ask, 100, 500) == Result::Ok);
  REQUIRE(book.add_limit(2, Side::Bid, 100, 500) == Result::Ok);
  CHECK(book.counters().traded_qty == 500);
  CHECK(book.counters().self_trades_prevented == 0);
  check_conservation(book);
}

TEST_CASE("self-trade: CancelResting drops the rester and keeps matching") {
  Book book;
  book.set_self_trade_policy(SelfTradePolicy::CancelResting);
  // Our own order is at the front of the queue; someone else is behind it at
  // the same price. The aggressor should skip past ours and fill on theirs.
  REQUIRE(book.add_limit_as(7, 1, Side::Ask, 100, 300) == Result::Ok);
  REQUIRE(book.add_limit_as(9, 2, Side::Ask, 100, 400) == Result::Ok);

  std::vector<Execution> execs;
  REQUIRE(book.add_limit_as(7, 3, Side::Bid, 100, 400,
                            [&](const Execution& e) { execs.push_back(e); }) == Result::Ok);

  CHECK(book.counters().self_trades_prevented == 1);
  REQUIRE(execs.size() == 1);
  CHECK(execs[0].resting_id == 2);  // filled against the other participant only
  CHECK(execs[0].qty == 400);
  CHECK(book.find_order(1) == nullptr);  // ours was removed
  CHECK(book.open_orders() == 0);
  check_conservation(book);
}

TEST_CASE("self-trade: CancelAggressor keeps the rester and kills the incoming order") {
  Book book;
  book.set_self_trade_policy(SelfTradePolicy::CancelAggressor);
  REQUIRE(book.add_limit_as(7, 1, Side::Ask, 100, 300) == Result::Ok);
  REQUIRE(book.add_limit_as(9, 2, Side::Ask, 100, 400) == Result::Ok);

  std::vector<Execution> execs;
  REQUIRE(book.add_limit_as(7, 3, Side::Bid, 100, 400,
                            [&](const Execution& e) { execs.push_back(e); }) == Result::Ok);

  CHECK(book.counters().self_trades_prevented == 1);
  CHECK(execs.empty());                    // never reached the other participant
  CHECK(book.find_order(1) != nullptr);    // ours survived
  CHECK(book.find_order(3) == nullptr);    // the aggressor did not rest
  CHECK(book.open_orders() == 2);
  check_conservation(book);
}

TEST_CASE("self-trade: CancelBoth removes the rester and the aggressor") {
  Book book;
  book.set_self_trade_policy(SelfTradePolicy::CancelBoth);
  REQUIRE(book.add_limit_as(7, 1, Side::Ask, 100, 300) == Result::Ok);
  REQUIRE(book.add_limit_as(9, 2, Side::Ask, 100, 400) == Result::Ok);

  REQUIRE(book.add_limit_as(7, 3, Side::Bid, 100, 400) == Result::Ok);

  CHECK(book.counters().self_trades_prevented == 1);
  CHECK(book.find_order(1) == nullptr);
  CHECK(book.find_order(3) == nullptr);
  CHECK(book.open_orders() == 1);  // only the unrelated participant is left
  CHECK(book.counters().traded_qty == 0);
  check_conservation(book);
}

TEST_CASE("self-trade: prevention only fires between the same participant") {
  Book book;
  book.set_self_trade_policy(SelfTradePolicy::CancelBoth);
  REQUIRE(book.add_limit_as(7, 1, Side::Ask, 100, 500) == Result::Ok);
  REQUIRE(book.add_limit_as(8, 2, Side::Bid, 100, 500) == Result::Ok);
  CHECK(book.counters().self_trades_prevented == 0);
  CHECK(book.counters().traded_qty == 500);
  check_conservation(book);
}

// Fill-or-kill promises all-or-nothing, so its pre-scan has to know which
// liquidity prevention will make unreachable. Getting this wrong is how a FOK
// silently becomes a partial fill.
TEST_CASE("self-trade: fill-or-kill accounts for liquidity prevention removes") {
  SUBCASE("CancelAggressor makes everything behind our own order unreachable") {
    Book book;
    book.set_self_trade_policy(SelfTradePolicy::CancelAggressor);
    REQUIRE(book.add_limit_as(7, 1, Side::Ask, 100, 300) == Result::Ok);   // ours, in front
    REQUIRE(book.add_limit_as(9, 2, Side::Ask, 100, 900) == Result::Ok);   // theirs, behind
    // 1200 shares are resting, but we can never get past our own order, so a
    // 500-share FOK must be rejected rather than partially filled.
    CHECK(book.add_fok_as(7, 3, Side::Bid, 100, 500) == Result::RejectedNoLiquidity);
    CHECK(book.open_orders() == 2);
    CHECK(book.counters().traded_qty == 0);
    check_conservation(book);
  }

  SUBCASE("CancelResting skips our own order and the rest is still reachable") {
    Book book;
    book.set_self_trade_policy(SelfTradePolicy::CancelResting);
    REQUIRE(book.add_limit_as(7, 1, Side::Ask, 100, 300) == Result::Ok);
    REQUIRE(book.add_limit_as(9, 2, Side::Ask, 100, 900) == Result::Ok);
    // Ours is skipped, theirs covers the whole 500.
    CHECK(book.add_fok_as(7, 3, Side::Bid, 100, 500) == Result::Ok);
    CHECK(book.counters().traded_qty == 500);
    check_conservation(book);
  }

  SUBCASE("a rejected fill-or-kill still leaves no trace") {
    Book book;
    book.set_self_trade_policy(SelfTradePolicy::CancelAggressor);
    REQUIRE(book.add_limit_as(7, 1, Side::Ask, 100, 300) == Result::Ok);
    const std::uint64_t before = book.state_hash();
    CHECK(book.add_fok_as(7, 2, Side::Bid, 100, 500) == Result::RejectedNoLiquidity);
    CHECK(book.state_hash() == before);
    CHECK(book.counters().self_trades_prevented == 0);
  }
}

TEST_CASE("self-trade: a replace stays with the same participant") {
  Book book;
  book.set_self_trade_policy(SelfTradePolicy::CancelBoth);
  REQUIRE(book.add_limit_as(7, 1, Side::Ask, 100, 500) == Result::Ok);
  REQUIRE(book.add_limit_as(7, 2, Side::Bid, 90, 500) == Result::Ok);
  // Repricing the bid through the ask would trade with our own order. The
  // replacement has a new id, but it is still ours.
  REQUIRE(book.replace(2, 3, 100, 500) == Result::Ok);
  CHECK(book.counters().self_trades_prevented == 1);
  CHECK(book.counters().traded_qty == 0);
  check_conservation(book);
}

TEST_CASE("self-trade: market orders honour the policy") {
  Book book;
  book.set_self_trade_policy(SelfTradePolicy::CancelResting);
  REQUIRE(book.add_limit_as(7, 1, Side::Ask, 100, 300) == Result::Ok);
  REQUIRE(book.add_limit_as(9, 2, Side::Ask, 101, 300) == Result::Ok);
  REQUIRE(book.add_market_as(7, 3, Side::Bid, 300) == Result::Ok);
  CHECK(book.counters().self_trades_prevented == 1);
  CHECK(book.counters().traded_qty == 300);  // swept past ours onto theirs
  CHECK(book.find_order(1) == nullptr);
  check_conservation(book);
}

// The standard of proof used everywhere else in this suite: agree with a
// model that shares no code, operation by operation, on a stream built to hit
// the interesting cases often.
TEST_CASE("self-trade: differential against the reference model") {
  const SelfTradePolicy policies[] = {SelfTradePolicy::CancelResting,
                                      SelfTradePolicy::CancelAggressor,
                                      SelfTradePolicy::CancelBoth};
  for (const SelfTradePolicy policy : policies) {
    OrderBook<PooledMapLadder, OpenAddressIdMap> fast;
    ReferenceBook ref;
    fast.set_self_trade_policy(policy);
    ref.set_self_trade_policy(policy);

    std::mt19937_64 rng(0x5717ull + static_cast<std::uint64_t>(policy));
    OrderId next_id = 1;
    std::vector<OrderId> live;

    for (int step = 0; step < 20000; ++step) {
      // Only four participants, so self-matches are common rather than rare.
      const ParticipantId actor = static_cast<ParticipantId>(1 + (rng() % 4));
      const Side side = (rng() % 2) == 0 ? Side::Bid : Side::Ask;
      const Price price = 995 + static_cast<Price>(rng() % 11);
      const Qty qty = static_cast<Qty>(1 + (rng() % 400));
      const std::uint64_t roll = rng() % 100;

      Result a{};
      Result b{};
      if (roll < 55) {
        const OrderId id = next_id++;
        a = fast.add_limit_as(actor, id, side, price, qty);
        b = ref.add_limit_as(actor, id, side, price, qty);
        if (a == Result::Ok) live.push_back(id);
      } else if (roll < 65) {
        const OrderId id = next_id++;
        a = fast.add_ioc_as(actor, id, side, price, qty);
        b = ref.add_ioc_as(actor, id, side, price, qty);
      } else if (roll < 72) {
        const OrderId id = next_id++;
        a = fast.add_fok_as(actor, id, side, price, qty);
        b = ref.add_fok_as(actor, id, side, price, qty);
      } else if (roll < 78) {
        const OrderId id = next_id++;
        a = fast.add_market_as(actor, id, side, qty);
        b = ref.add_market_as(actor, id, side, qty);
      } else if (!live.empty()) {
        const std::size_t i = rng() % live.size();
        const OrderId id = live[i];
        if (roll < 92) {
          a = fast.cancel(id);
          b = ref.cancel(id);
          live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
          const OrderId nid = next_id++;
          a = fast.replace(id, nid, price, qty);
          b = ref.replace(id, nid, price, qty);
          live[i] = nid;
        }
      } else {
        continue;
      }

      if (a != b || fast.state_hash() != ref.state_hash()) {
        INFO("policy ", static_cast<int>(policy), " diverged at step ", step);
        REQUIRE(a == b);
        REQUIRE(fast.state_hash() == ref.state_hash());
      }
    }

    INFO("policy ", static_cast<int>(policy));
    CHECK(fast.counters().traded_qty == ref.traded_qty());
    CHECK(fast.counters().canceled_qty == ref.canceled_qty());
    CHECK(fast.counters().self_trades_prevented == ref.self_trades_prevented());
    // The stream has to have actually exercised prevention for any of this to
    // mean anything.
    CHECK(fast.counters().self_trades_prevented > 100);
    check_conservation(fast);
  }
}
