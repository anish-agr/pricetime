#include <doctest/doctest.h>

#include <vector>

#include "book_test_util.hpp"
#include "pricetime/book.hpp"
#include "pricetime/ladder_dense.hpp"
#include "pricetime/ladder_map.hpp"

using namespace pricetime;
using pricetime::test::check_invariants;
using pricetime::test::make_book;

namespace {

struct ExecRecorder {
  std::vector<Execution> execs;
  void operator()(const Execution& e) { execs.push_back(e); }
};

}  // namespace

TEST_CASE_TEMPLATE("id 0 is reserved and rejected everywhere", L, MapLadder, DenseLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(0, Side::Bid, 1000, 10) == Result::RejectedBadId);
  CHECK(book.add_ioc(0, Side::Bid, 1000, 10) == Result::RejectedBadId);
  CHECK(book.add_fok(0, Side::Bid, 1000, 10) == Result::RejectedBadId);
  CHECK(book.add_market(0, Side::Bid, 10) == Result::RejectedBadId);
  CHECK(book.cancel(0) == Result::RejectedUnknownId);
  CHECK(book.add_limit(1, Side::Bid, 1000, 10) == Result::Ok);
  CHECK(book.replace(1, 0, 1001, 10) == Result::RejectedBadId);
  CHECK(book.open_orders() == 1);  // the rejected replace left order 1 alone
  check_invariants(book);
}

TEST_CASE_TEMPLATE("IOC: fills what it can and kills the rest", L, MapLadder, DenseLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Ask, 1000, 4) == Result::Ok);

  ExecRecorder rec;
  CHECK(book.add_ioc(2, Side::Bid, 1000, 10, rec) == Result::Ok);

  REQUIRE(rec.execs.size() == 1);
  CHECK(rec.execs[0].qty == 4);
  CHECK(book.best(Side::Bid) == nullptr);  // the remaining 6 did not rest
  CHECK(book.open_orders() == 0);

  const auto& c = book.counters();
  CHECK(c.added_qty == 14);
  CHECK(c.traded_qty == 4);
  CHECK(c.canceled_qty == 6);  // killed remainder counts as canceled
  check_invariants(book);
}

TEST_CASE_TEMPLATE("IOC against an empty book trades nothing and rests nothing", L, MapLadder,
                   DenseLadder) {
  auto book = make_book<L>();
  ExecRecorder rec;
  CHECK(book.add_ioc(1, Side::Bid, 1000, 10, rec) == Result::Ok);
  CHECK(rec.execs.empty());
  CHECK(book.open_orders() == 0);
  CHECK(book.counters().canceled_qty == 10);
  check_invariants(book);
}

TEST_CASE_TEMPLATE("IOC does not reach past its limit price", L, MapLadder, DenseLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Ask, 1000, 5) == Result::Ok);
  CHECK(book.add_limit(2, Side::Ask, 1005, 5) == Result::Ok);

  ExecRecorder rec;
  CHECK(book.add_ioc(3, Side::Bid, 1000, 10, rec) == Result::Ok);
  REQUIRE(rec.execs.size() == 1);
  CHECK(rec.execs[0].price == 1000);
  REQUIRE(book.best(Side::Ask) != nullptr);
  CHECK(book.best(Side::Ask)->price == 1005);  // out-of-reach level untouched
  check_invariants(book);
}

TEST_CASE_TEMPLATE("FOK: fills completely or leaves no trace", L, MapLadder, DenseLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Ask, 1000, 4) == Result::Ok);

  SUBCASE("insufficient liquidity -> rejected, book untouched") {
    ExecRecorder rec;
    CHECK(book.add_fok(2, Side::Bid, 1000, 10, rec) == Result::RejectedNoLiquidity);
    CHECK(rec.execs.empty());
    REQUIRE(book.best(Side::Ask) != nullptr);
    CHECK(book.best(Side::Ask)->total_qty == 4);  // resting order untouched
    const auto& c = book.counters();
    CHECK(c.added_qty == 4);  // the killed order was never counted
    CHECK(c.traded_qty == 0);
    CHECK(c.canceled_qty == 0);
  }

  SUBCASE("exactly enough liquidity -> fills") {
    ExecRecorder rec;
    CHECK(book.add_fok(2, Side::Bid, 1000, 4, rec) == Result::Ok);
    REQUIRE(rec.execs.size() == 1);
    CHECK(rec.execs[0].qty == 4);
    CHECK(book.open_orders() == 0);
  }

  SUBCASE("liquidity spread across levels still counts") {
    CHECK(book.add_limit(3, Side::Ask, 1001, 6) == Result::Ok);
    ExecRecorder rec;
    CHECK(book.add_fok(4, Side::Bid, 1001, 10, rec) == Result::Ok);
    REQUIRE(rec.execs.size() == 2);
    CHECK(rec.execs[0].price == 1000);
    CHECK(rec.execs[1].price == 1001);
    CHECK(book.open_orders() == 0);
  }
  check_invariants(book);
}

TEST_CASE_TEMPLATE("FOK ignores liquidity beyond its limit price", L, MapLadder, DenseLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Ask, 1000, 4) == Result::Ok);
  CHECK(book.add_limit(2, Side::Ask, 1010, 100) == Result::Ok);  // plenty, but too expensive

  ExecRecorder rec;
  CHECK(book.add_fok(3, Side::Bid, 1000, 10, rec) == Result::RejectedNoLiquidity);
  CHECK(rec.execs.empty());
  CHECK(book.open_orders() == 2);
  check_invariants(book);
}

TEST_CASE_TEMPLATE("market order sweeps every level and never rests", L, MapLadder,
                   DenseLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Ask, 1000, 5) == Result::Ok);
  CHECK(book.add_limit(2, Side::Ask, 1020, 5) == Result::Ok);
  CHECK(book.add_limit(3, Side::Ask, 1050, 5) == Result::Ok);

  ExecRecorder rec;
  CHECK(book.add_market(4, Side::Bid, 12, rec) == Result::Ok);

  REQUIRE(rec.execs.size() == 3);
  CHECK(rec.execs[0].price == 1000);
  CHECK(rec.execs[1].price == 1020);
  CHECK(rec.execs[2].price == 1050);  // walked far past any sane limit
  CHECK(rec.execs[2].qty == 2);
  CHECK(book.best(Side::Bid) == nullptr);
  REQUIRE(book.best(Side::Ask) != nullptr);
  CHECK(book.best(Side::Ask)->total_qty == 3);
  check_invariants(book);
}

TEST_CASE_TEMPLATE("market order against an empty book is killed, not an error", L, MapLadder,
                   DenseLadder) {
  auto book = make_book<L>();
  ExecRecorder rec;
  CHECK(book.add_market(1, Side::Bid, 10, rec) == Result::Ok);
  CHECK(rec.execs.empty());
  CHECK(book.open_orders() == 0);
  CHECK(book.counters().canceled_qty == 10);
  check_invariants(book);
}

TEST_CASE_TEMPLATE("market order exhausts the book and kills its remainder", L, MapLadder,
                   DenseLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Ask, 1000, 5) == Result::Ok);

  ExecRecorder rec;
  CHECK(book.add_market(2, Side::Bid, 20, rec) == Result::Ok);
  REQUIRE(rec.execs.size() == 1);
  CHECK(rec.execs[0].qty == 5);

  const auto& c = book.counters();
  CHECK(c.added_qty == 25);
  CHECK(c.traded_qty == 5);
  CHECK(c.canceled_qty == 15);
  CHECK(book.open_orders() == 0);
  check_invariants(book);
}

TEST_CASE_TEMPLATE("aggressive orders respect FIFO priority like any other", L, MapLadder,
                   DenseLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Ask, 1000, 5) == Result::Ok);
  CHECK(book.add_limit(2, Side::Ask, 1000, 5) == Result::Ok);

  ExecRecorder rec;
  CHECK(book.add_market(3, Side::Bid, 7, rec) == Result::Ok);
  REQUIRE(rec.execs.size() == 2);
  CHECK(rec.execs[0].resting_id == 1);
  CHECK(rec.execs[1].resting_id == 2);
  CHECK(rec.execs[1].qty == 2);
  check_invariants(book);
}

TEST_CASE_TEMPLATE("L2 depth snapshot aggregates and truncates", L, MapLadder, DenseLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Bid, 1000, 10) == Result::Ok);
  CHECK(book.add_limit(2, Side::Bid, 1000, 5) == Result::Ok);
  CHECK(book.add_limit(3, Side::Bid, 999, 7) == Result::Ok);
  CHECK(book.add_limit(4, Side::Bid, 998, 3) == Result::Ok);
  CHECK(book.add_limit(5, Side::Ask, 1002, 8) == Result::Ok);

  const Depth d = book.depth(2);
  REQUIRE(d.bids.size() == 2);  // truncated to the top 2 of 3 levels
  CHECK(d.bids[0].price == 1000);
  CHECK(d.bids[0].qty == 15);  // both orders aggregated
  CHECK(d.bids[0].orders == 2);
  CHECK(d.bids[1].price == 999);
  CHECK(d.bids[1].qty == 7);
  REQUIRE(d.asks.size() == 1);
  CHECK(d.asks[0].price == 1002);

  const Depth all = book.depth(100);
  CHECK(all.bids.size() == 3);
  CHECK(all.bids[2].price == 998);

  const Depth none = book.depth(0);
  CHECK(none.bids.empty());
  CHECK(none.asks.empty());
}
