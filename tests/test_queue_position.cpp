#include <doctest/doctest.h>

#include <unordered_map>

#include "pricetime/queue_position.hpp"

using namespace pricetime;

TEST_CASE("queue position: joins at the back, behind everything resting") {
  QueuePosition qp;
  qp.place(1000, Side::Bid, 100, {{1, 50}, {2, 30}, {3, 20}});
  CHECK(qp.active());
  CHECK(qp.shares_ahead() == 100);
  CHECK_FALSE(qp.at_front());
}

TEST_CASE("queue position: reaches the front only when everyone ahead is gone") {
  QueuePosition qp;
  qp.place(1000, Side::Bid, 100, {{1, 50}, {2, 30}});

  qp.on_shares_removed(1, 50);  // order 1 fully executed
  CHECK(qp.shares_ahead() == 30);
  CHECK_FALSE(qp.at_front());

  qp.on_shares_removed(2, 10);  // order 2 partially canceled
  CHECK(qp.shares_ahead() == 20);
  CHECK_FALSE(qp.at_front());

  qp.on_shares_removed(2, 20);  // order 2 deleted
  CHECK(qp.at_front());
  CHECK(qp.shares_ahead() == 0);
}

TEST_CASE("queue position: removals of unknown orders are ignored") {
  QueuePosition qp;
  qp.place(1000, Side::Bid, 100, {{1, 50}});
  qp.on_shares_removed(99, 1000);  // an order at some other level
  CHECK(qp.shares_ahead() == 50);
  // Over-removal clamps rather than underflowing.
  qp.on_shares_removed(1, 200);
  CHECK(qp.shares_ahead() == 0);
  CHECK(qp.at_front());
}

TEST_CASE("queue position: fills only what remains and deactivates when done") {
  QueuePosition qp;
  qp.place(1000, Side::Ask, 100, {});
  CHECK(qp.at_front());  // empty level: front immediately

  CHECK(qp.take_fill(60) == 60);
  CHECK(qp.remaining() == 40);
  CHECK(qp.active());

  CHECK(qp.take_fill(60) == 40);  // capped at the remainder
  CHECK(qp.remaining() == 0);
  CHECK_FALSE(qp.active());
}

TEST_CASE("queue position: cancel abandons the spot") {
  QueuePosition qp;
  qp.place(1000, Side::Bid, 100, {{1, 10}});
  qp.cancel();
  CHECK_FALSE(qp.active());
  CHECK_FALSE(qp.at_front());
  CHECK(qp.shares_ahead() == 0);
}
