#include <doctest/doctest.h>

#include "book_test_util.hpp"
#include "pricetime/book.hpp"
#include "pricetime/ladder_dense.hpp"
#include "pricetime/ladder_map.hpp"
#include "pricetime/ladder_pooled.hpp"

using namespace pricetime;
using pricetime::test::make_book;

TEST_CASE_TEMPLATE("add rests and best prices update", L, MapLadder, DenseLadder, PooledMapLadder) {
  auto book = make_book<L>();
  REQUIRE(book.best(Side::Bid) == nullptr);
  REQUIRE(book.best(Side::Ask) == nullptr);

  CHECK(book.add_limit(1, Side::Bid, 1000, 10) == Result::Ok);
  CHECK(book.add_limit(2, Side::Bid, 1001, 20) == Result::Ok);
  CHECK(book.add_limit(3, Side::Ask, 1005, 30) == Result::Ok);
  CHECK(book.add_limit(4, Side::Ask, 1004, 40) == Result::Ok);

  REQUIRE(book.best(Side::Bid) != nullptr);
  CHECK(book.best(Side::Bid)->price == 1001);
  CHECK(book.best(Side::Bid)->total_qty == 20);
  REQUIRE(book.best(Side::Ask) != nullptr);
  CHECK(book.best(Side::Ask)->price == 1004);
  CHECK(book.open_orders() == 4);
  pricetime::test::check_invariants(book);
}

TEST_CASE_TEMPLATE("multiple orders aggregate at one level", L, MapLadder, DenseLadder, PooledMapLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Bid, 1000, 10) == Result::Ok);
  CHECK(book.add_limit(2, Side::Bid, 1000, 15) == Result::Ok);
  CHECK(book.add_limit(3, Side::Bid, 1000, 25) == Result::Ok);
  const Level* lvl = book.best(Side::Bid);
  REQUIRE(lvl != nullptr);
  CHECK(lvl->total_qty == 50);
  CHECK(lvl->order_count == 3);
}

TEST_CASE_TEMPLATE("rejects: duplicate id, zero qty, unknown cancel", L, MapLadder, DenseLadder, PooledMapLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Bid, 1000, 10) == Result::Ok);
  CHECK(book.add_limit(1, Side::Bid, 1001, 10) == Result::RejectedDuplicateId);
  CHECK(book.add_limit(2, Side::Ask, 1005, 0) == Result::RejectedBadQty);
  CHECK(book.cancel(999) == Result::RejectedUnknownId);
  CHECK(book.open_orders() == 1);
  pricetime::test::check_invariants(book);
}

TEST_CASE_TEMPLATE("cancel removes the order and empties the level", L, MapLadder, DenseLadder, PooledMapLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Bid, 1000, 10) == Result::Ok);
  CHECK(book.add_limit(2, Side::Bid, 999, 5) == Result::Ok);

  CHECK(book.cancel(1) == Result::Ok);
  REQUIRE(book.best(Side::Bid) != nullptr);
  CHECK(book.best(Side::Bid)->price == 999);  // best fell back to the next level
  CHECK(book.cancel(1) == Result::RejectedUnknownId);  // double cancel rejected

  CHECK(book.cancel(2) == Result::Ok);
  CHECK(book.best(Side::Bid) == nullptr);
  CHECK(book.open_orders() == 0);
  pricetime::test::check_invariants(book);
}

TEST_CASE_TEMPLATE("a level can empty and be reused by the other side", L, MapLadder,
                   DenseLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Bid, 1000, 10) == Result::Ok);
  CHECK(book.cancel(1) == Result::Ok);
  CHECK(book.add_limit(2, Side::Ask, 1000, 7) == Result::Ok);
  REQUIRE(book.best(Side::Ask) != nullptr);
  CHECK(book.best(Side::Ask)->price == 1000);
  CHECK(book.best(Side::Bid) == nullptr);
  pricetime::test::check_invariants(book);
}

TEST_CASE("dense ladder rejects out-of-range prices without touching the book") {
  OrderBook<DenseLadder> book{100, 200};
  CHECK(book.add_limit(1, Side::Bid, 99, 10) == Result::RejectedBadPrice);
  CHECK(book.add_limit(2, Side::Ask, 201, 10) == Result::RejectedBadPrice);
  CHECK(book.open_orders() == 0);
  CHECK(book.counters().added_qty == 0);
  CHECK(book.add_limit(3, Side::Bid, 100, 10) == Result::Ok);  // boundaries accepted
  CHECK(book.add_limit(4, Side::Ask, 200, 10) == Result::Ok);
}

TEST_CASE_TEMPLATE("replace: new id, new priority, old order gone", L, MapLadder, DenseLadder, PooledMapLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Bid, 1000, 10) == Result::Ok);
  CHECK(book.replace(1, 2, 1001, 15) == Result::Ok);

  CHECK(book.cancel(1) == Result::RejectedUnknownId);  // old id is dead
  REQUIRE(book.best(Side::Bid) != nullptr);
  CHECK(book.best(Side::Bid)->price == 1001);
  CHECK(book.best(Side::Bid)->total_qty == 15);
  CHECK(book.open_orders() == 1);
  pricetime::test::check_invariants(book);
}

TEST_CASE_TEMPLATE("replace rejections leave the original order untouched", L, MapLadder,
                   DenseLadder) {
  auto book = make_book<L>();
  CHECK(book.add_limit(1, Side::Bid, 1000, 10) == Result::Ok);
  CHECK(book.add_limit(2, Side::Bid, 999, 5) == Result::Ok);

  CHECK(book.replace(99, 3, 1001, 5) == Result::RejectedUnknownId);
  CHECK(book.replace(1, 2, 1001, 5) == Result::RejectedDuplicateId);  // id 2 already open
  CHECK(book.replace(1, 3, 1001, 0) == Result::RejectedBadQty);

  // Order 1 must still be fully intact at its original price and quantity.
  REQUIRE(book.best(Side::Bid) != nullptr);
  CHECK(book.best(Side::Bid)->price == 1000);
  CHECK(book.best(Side::Bid)->total_qty == 10);
  CHECK(book.open_orders() == 2);
  pricetime::test::check_invariants(book);
}
