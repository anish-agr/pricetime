#include <doctest/doctest.h>

#include <vector>

#include "book_test_util.hpp"
#include "pricetime/book.hpp"
#include "pricetime/ladder_dense.hpp"
#include "pricetime/ladder_map.hpp"

using namespace pricetime;
using pricetime::test::make_book;

namespace {

struct ExecRecorder {
  std::vector<Execution> execs;
  void operator()(const Execution& e) { execs.push_back(e); }
};

}  // namespace

TEST_CASE_TEMPLATE("exact cross fills fully and rests nothing", L, MapLadder, DenseLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Ask, 1000, 10) == Result::Ok);

  ExecRecorder rec;
  CHECK(book.add_limit(2, Side::Bid, 1000, 10, rec) == Result::Ok);

  REQUIRE(rec.execs.size() == 1);
  CHECK(rec.execs[0].resting_id == 1);
  CHECK(rec.execs[0].aggressor_id == 2);
  CHECK(rec.execs[0].price == 1000);
  CHECK(rec.execs[0].qty == 10);
  CHECK(book.open_orders() == 0);
  CHECK(book.best(Side::Bid) == nullptr);
  CHECK(book.best(Side::Ask) == nullptr);
  pricetime::test::check_invariants(book);
}

TEST_CASE_TEMPLATE("price improvement: trade happens at the resting price", L, MapLadder,
                   DenseLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Ask, 1000, 10) == Result::Ok);

  ExecRecorder rec;
  CHECK(book.add_limit(2, Side::Bid, 1010, 10, rec) == Result::Ok);  // willing to pay 1010

  REQUIRE(rec.execs.size() == 1);
  CHECK(rec.execs[0].price == 1000);  // fills at the ask, not the bid limit
  pricetime::test::check_invariants(book);
}

TEST_CASE_TEMPLATE("partial fill: remainder rests at the limit price", L, MapLadder,
                   DenseLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Ask, 1000, 4) == Result::Ok);

  ExecRecorder rec;
  CHECK(book.add_limit(2, Side::Bid, 1002, 10, rec) == Result::Ok);

  REQUIRE(rec.execs.size() == 1);
  CHECK(rec.execs[0].qty == 4);
  REQUIRE(book.best(Side::Bid) != nullptr);
  CHECK(book.best(Side::Bid)->price == 1002);  // remainder resting
  CHECK(book.best(Side::Bid)->total_qty == 6);
  CHECK(book.best(Side::Ask) == nullptr);
  pricetime::test::check_invariants(book);
}

TEST_CASE_TEMPLATE("partial fill of a rester: it keeps price and queue position", L, MapLadder,
                   DenseLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Ask, 1000, 10) == Result::Ok);
  CHECK(book.add_limit(2, Side::Ask, 1000, 10) == Result::Ok);

  ExecRecorder rec;
  CHECK(book.add_limit(3, Side::Bid, 1000, 4, rec) == Result::Ok);  // bites into order 1

  REQUIRE(rec.execs.size() == 1);
  CHECK(rec.execs[0].resting_id == 1);
  CHECK(rec.execs[0].qty == 4);

  // Next aggressor must keep filling order 1 first: it stayed at the head.
  ExecRecorder rec2;
  CHECK(book.add_limit(4, Side::Bid, 1000, 8, rec2) == Result::Ok);
  REQUIRE(rec2.execs.size() == 2);
  CHECK(rec2.execs[0].resting_id == 1);
  CHECK(rec2.execs[0].qty == 6);  // order 1's remainder
  CHECK(rec2.execs[1].resting_id == 2);
  CHECK(rec2.execs[1].qty == 2);
  pricetime::test::check_invariants(book);
}

TEST_CASE_TEMPLATE("FIFO at a level: fills strictly in arrival order", L, MapLadder,
                   DenseLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Ask, 1000, 5) == Result::Ok);
  CHECK(book.add_limit(2, Side::Ask, 1000, 5) == Result::Ok);
  CHECK(book.add_limit(3, Side::Ask, 1000, 5) == Result::Ok);

  ExecRecorder rec;
  CHECK(book.add_limit(4, Side::Bid, 1000, 12, rec) == Result::Ok);

  REQUIRE(rec.execs.size() == 3);
  CHECK(rec.execs[0].resting_id == 1);
  CHECK(rec.execs[0].qty == 5);
  CHECK(rec.execs[1].resting_id == 2);
  CHECK(rec.execs[1].qty == 5);
  CHECK(rec.execs[2].resting_id == 3);
  CHECK(rec.execs[2].qty == 2);
  REQUIRE(book.best(Side::Ask) != nullptr);
  CHECK(book.best(Side::Ask)->total_qty == 3);  // order 3's remainder
  CHECK(book.best(Side::Bid) == nullptr);       // aggressor fully filled
  pricetime::test::check_invariants(book);
}

TEST_CASE_TEMPLATE("aggressor sweeps multiple levels in price order", L, MapLadder,
                   DenseLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Ask, 1002, 5) == Result::Ok);
  CHECK(book.add_limit(2, Side::Ask, 1000, 5) == Result::Ok);
  CHECK(book.add_limit(3, Side::Ask, 1001, 5) == Result::Ok);
  CHECK(book.add_limit(4, Side::Ask, 1004, 5) == Result::Ok);  // beyond the limit; survives

  ExecRecorder rec;
  CHECK(book.add_limit(5, Side::Bid, 1002, 100, rec) == Result::Ok);

  REQUIRE(rec.execs.size() == 3);
  CHECK(rec.execs[0].resting_id == 2);  // best price first
  CHECK(rec.execs[0].price == 1000);
  CHECK(rec.execs[1].resting_id == 3);
  CHECK(rec.execs[1].price == 1001);
  CHECK(rec.execs[2].resting_id == 1);
  CHECK(rec.execs[2].price == 1002);

  REQUIRE(book.best(Side::Ask) != nullptr);
  CHECK(book.best(Side::Ask)->price == 1004);
  REQUIRE(book.best(Side::Bid) != nullptr);
  CHECK(book.best(Side::Bid)->price == 1002);  // remainder rests below the surviving ask
  CHECK(book.best(Side::Bid)->total_qty == 85);
  pricetime::test::check_invariants(book);
}

TEST_CASE_TEMPLATE("replace loses time priority even at the same price", L, MapLadder,
                   DenseLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Ask, 1000, 5) == Result::Ok);
  CHECK(book.add_limit(2, Side::Ask, 1000, 5) == Result::Ok);

  // Order 1 is replaced (same price, same qty) -> goes to the back of the queue.
  CHECK(book.replace(1, 10, 1000, 5) == Result::Ok);

  ExecRecorder rec;
  CHECK(book.add_limit(3, Side::Bid, 1000, 5, rec) == Result::Ok);
  REQUIRE(rec.execs.size() == 1);
  CHECK(rec.execs[0].resting_id == 2);  // order 2 now has priority
  pricetime::test::check_invariants(book);
}

TEST_CASE_TEMPLATE("replace priced through the book executes", L, MapLadder, DenseLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Ask, 1000, 10) == Result::Ok);
  CHECK(book.add_limit(2, Side::Bid, 990, 10) == Result::Ok);

  ExecRecorder rec;
  CHECK(book.replace(2, 3, 1000, 10, rec) == Result::Ok);  // bid repriced onto the ask

  REQUIRE(rec.execs.size() == 1);
  CHECK(rec.execs[0].resting_id == 1);
  CHECK(rec.execs[0].aggressor_id == 3);
  CHECK(rec.execs[0].qty == 10);
  CHECK(book.open_orders() == 0);
  pricetime::test::check_invariants(book);
}

TEST_CASE_TEMPLATE("share conservation counters through a mixed sequence", L, MapLadder,
                   DenseLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Ask, 1000, 10) == Result::Ok);
  CHECK(book.add_limit(2, Side::Bid, 1000, 4) == Result::Ok);  // trades 4
  CHECK(book.cancel(1) == Result::Ok);                         // cancels the remaining 6

  const auto& c = book.counters();
  CHECK(c.added_qty == 14);
  CHECK(c.traded_qty == 4);
  CHECK(c.canceled_qty == 6);
  CHECK(book.open_orders() == 0);
  pricetime::test::check_invariants(book);
}
