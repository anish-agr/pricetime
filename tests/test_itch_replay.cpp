#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "pricetime/itch_reader.hpp"
#include "pricetime/itch_writer.hpp"
#include "pricetime/itch_replay.hpp"
#include "pricetime/ladder_dense.hpp"
#include "pricetime/ladder_map.hpp"
#include "pricetime/multi_book.hpp"

using namespace pricetime;
using namespace pricetime::itch;
using pricetime::itch::Writer;

namespace {

// Runs a synthetic file through the framing reader into a replayer.
template <class Ladder>
ReadResult drive(Replayer<Ladder>& rep, const Writer& w) {
  return for_each_framed_message(
      w.bytes().data(), w.bytes().size(),
      [&](const std::uint8_t* m, std::size_t len) { rep.apply(m, len); });
}

const Symbol kAapl{"AAPL"};
const Symbol kMsft{"MSFT"};

}  // namespace

TEST_CASE("replay builds a two-sided book from adds") {
  MultiBook<MapLadder> books;
  Replayer<MapLadder> rep(books);
  Writer w;
  w.system_event(1, 'Q');
  w.add_order(10, 1, Side::Bid, 100, kAapl, 1000000);
  w.add_order(11, 2, Side::Bid, 200, kAapl, 999900);
  w.add_order(12, 3, Side::Ask, 150, kAapl, 1000100);
  REQUIRE(drive(rep, w).ok());

  const auto* book = books.find(kAapl);
  REQUIRE(book != nullptr);
  CHECK(books.symbol_count() == 1);
  CHECK(book->open_orders() == 3);
  REQUIRE(book->best(Side::Bid) != nullptr);
  CHECK(book->best(Side::Bid)->price == 1000000);
  REQUIRE(book->best(Side::Ask) != nullptr);
  CHECK(book->best(Side::Ask)->price == 1000100);
  CHECK(rep.stats().adds == 3);
  CHECK(rep.stats().session_state == 'Q');
}

// The central semantic point of feed reconstruction: the exchange has already
// matched, so a crossing add must rest rather than trade. If this test ever
// fails the replayed book silently diverges from reality.
TEST_CASE("a crossing add rests instead of matching") {
  MultiBook<MapLadder> books;
  Replayer<MapLadder> rep(books);
  Writer w;
  w.add_order(1, 1, Side::Ask, 100, kAapl, 1000000);
  w.add_order(2, 2, Side::Bid, 100, kAapl, 1000000);  // same price, would cross
  REQUIRE(drive(rep, w).ok());

  const auto* book = books.find(kAapl);
  REQUIRE(book != nullptr);
  CHECK(book->open_orders() == 2);  // both still resting
  CHECK(book->counters().traded_qty == 0);
  CHECK(book->counters().executed_qty == 0);
}

TEST_CASE("execution reduces the resting order and removes it when exhausted") {
  MultiBook<MapLadder> books;
  Replayer<MapLadder> rep(books);
  Writer w;
  w.add_order(1, 1, Side::Bid, 100, kAapl, 1000000);
  w.order_executed(2, 1, 40, 5001);
  REQUIRE(drive(rep, w).ok());

  const auto* book = books.find(kAapl);
  REQUIRE(book->best(Side::Bid) != nullptr);
  CHECK(book->best(Side::Bid)->total_qty == 60);
  CHECK(book->counters().executed_qty == 40);
  CHECK(books.tracked_orders() == 1);

  Writer w2;
  w2.order_executed(3, 1, 60, 5002);
  REQUIRE(drive(rep, w2).ok());
  CHECK(book->best(Side::Bid) == nullptr);
  CHECK(book->open_orders() == 0);
  CHECK(books.tracked_orders() == 0);  // index cleaned up too
  CHECK(book->counters().executed_qty == 100);
}

TEST_CASE("executed-with-price affects the book exactly like a plain execution") {
  MultiBook<MapLadder> books;
  Replayer<MapLadder> rep(books);
  Writer w;
  w.add_order(1, 1, Side::Bid, 100, kAapl, 1000000);
  // Non-printable, and priced away from the order's own price: neither
  // changes what happens to the resting quantity.
  w.order_executed_with_price(2, 1, 30, 5001, false, 999000);
  REQUIRE(drive(rep, w).ok());
  const auto* book = books.find(kAapl);
  REQUIRE(book->best(Side::Bid) != nullptr);
  CHECK(book->best(Side::Bid)->total_qty == 70);
  CHECK(book->best(Side::Bid)->price == 1000000);  // unchanged
}

TEST_CASE("partial cancel reduces, delete removes") {
  MultiBook<MapLadder> books;
  Replayer<MapLadder> rep(books);
  Writer w;
  w.add_order(1, 1, Side::Ask, 500, kAapl, 1000100);
  w.order_cancel(2, 1, 200);
  REQUIRE(drive(rep, w).ok());
  const auto* book = books.find(kAapl);
  REQUIRE(book->best(Side::Ask) != nullptr);
  CHECK(book->best(Side::Ask)->total_qty == 300);
  CHECK(book->counters().canceled_qty == 200);

  Writer w2;
  w2.order_delete(3, 1);
  REQUIRE(drive(rep, w2).ok());
  CHECK(book->best(Side::Ask) == nullptr);
  CHECK(book->counters().canceled_qty == 500);
  CHECK(books.tracked_orders() == 0);
}

// A replace message names no side; the replayer has to read it off the
// original order before destroying it.
TEST_CASE("replace preserves the side and applies the new price and size") {
  MultiBook<MapLadder> books;
  Replayer<MapLadder> rep(books);
  Writer w;
  w.add_order(1, 1, Side::Ask, 500, kAapl, 1000100);
  w.order_replace(2, 1, 2, 300, 1000200);
  REQUIRE(drive(rep, w).ok());

  const auto* book = books.find(kAapl);
  CHECK(book->best(Side::Bid) == nullptr);  // did not flip sides
  REQUIRE(book->best(Side::Ask) != nullptr);
  CHECK(book->best(Side::Ask)->price == 1000200);
  CHECK(book->best(Side::Ask)->total_qty == 300);
  CHECK(book->open_orders() == 1);
  CHECK(rep.stats().replaces == 1);

  // The old reference is gone; the new one is routable.
  Writer w2;
  w2.order_delete(3, 2);
  REQUIRE(drive(rep, w2).ok());
  CHECK(book->open_orders() == 0);
}

TEST_CASE("a chain of replaces stays consistent") {
  MultiBook<MapLadder> books;
  Replayer<MapLadder> rep(books);
  Writer w;
  w.add_order(1, 1, Side::Bid, 100, kAapl, 1000000);
  w.order_replace(2, 1, 2, 90, 1000010);
  w.order_replace(3, 2, 3, 80, 1000020);
  w.order_replace(4, 3, 4, 70, 1000030);
  REQUIRE(drive(rep, w).ok());
  const auto* book = books.find(kAapl);
  CHECK(book->open_orders() == 1);
  REQUIRE(book->best(Side::Bid) != nullptr);
  CHECK(book->best(Side::Bid)->price == 1000030);
  CHECK(book->best(Side::Bid)->total_qty == 70);
  CHECK(books.tracked_orders() == 1);
}

// 'P' reports a trade against hidden liquidity. Applying it to the book would
// double-count volume that was never displayed.
TEST_CASE("non-cross trade messages never touch the book") {
  MultiBook<MapLadder> books;
  Replayer<MapLadder> rep(books);
  Writer w;
  w.add_order(1, 1, Side::Bid, 100, kAapl, 1000000);
  w.trade_non_cross(2, 999, Side::Bid, 50, kAapl, 1000000, 7001);
  REQUIRE(drive(rep, w).ok());
  const auto* book = books.find(kAapl);
  REQUIRE(book->best(Side::Bid) != nullptr);
  CHECK(book->best(Side::Bid)->total_qty == 100);  // untouched
  CHECK(book->counters().executed_qty == 0);
  CHECK(rep.stats().trades == 1);
  CHECK(rep.stats().adds == 1);
}

TEST_CASE("administrative messages are skipped without disturbing the book") {
  MultiBook<MapLadder> books;
  Replayer<MapLadder> rep(books);
  Writer w;
  w.stock_directory(1, kAapl);
  w.stock_trading_action(2, kAapl, 'T');
  w.add_order(3, 1, Side::Bid, 100, kAapl, 1000000);
  REQUIRE(drive(rep, w).ok());
  CHECK(rep.stats().messages == 3);
  CHECK(rep.stats().adds == 1);
  CHECK(books.find(kAapl)->open_orders() == 1);
}

TEST_CASE("multi-symbol: books stay independent and ids route correctly") {
  MultiBook<MapLadder> books;
  Replayer<MapLadder> rep(books);
  Writer w;
  w.add_order(1, 1, Side::Bid, 100, kAapl, 1000000);
  w.add_order(2, 2, Side::Bid, 200, kMsft, 4000000);
  w.add_order(3, 3, Side::Ask, 300, kAapl, 1000100);
  // Delete by reference only; no symbol on the wire.
  w.order_delete(4, 2);
  REQUIRE(drive(rep, w).ok());

  CHECK(books.symbol_count() == 2);
  const auto* aapl = books.find(kAapl);
  const auto* msft = books.find(kMsft);
  REQUIRE(aapl != nullptr);
  REQUIRE(msft != nullptr);
  CHECK(aapl->open_orders() == 2);
  CHECK(msft->open_orders() == 0);  // the delete found the right book
  CHECK(aapl->best(Side::Bid)->price == 1000000);
}

TEST_CASE("symbol filter tracks only what was asked for") {
  MultiBook<MapLadder> books;
  Replayer<MapLadder> rep(books);
  rep.track_only({kAapl});
  Writer w;
  w.add_order(1, 1, Side::Bid, 100, kAapl, 1000000);
  w.add_order(2, 2, Side::Bid, 200, kMsft, 4000000);
  w.order_delete(3, 2);  // refers to a filtered-out order
  REQUIRE(drive(rep, w).ok());

  CHECK(books.symbol_count() == 1);
  CHECK(books.find(kMsft) == nullptr);
  CHECK(rep.stats().skipped_symbol == 1);
  // Not an error: a filtered replay legitimately sees references it never
  // recorded.
  CHECK(rep.stats().unknown_reference == 1);
  CHECK(books.find(kAapl)->open_orders() == 1);
}

TEST_CASE("over-consumption is clamped and counted, not allowed to underflow") {
  MultiBook<MapLadder> books;
  Replayer<MapLadder> rep(books);
  Writer w;
  w.add_order(1, 1, Side::Bid, 100, kAapl, 1000000);
  w.order_executed(2, 1, 250, 5001);  // more than is resting
  REQUIRE(drive(rep, w).ok());
  const auto* book = books.find(kAapl);
  CHECK(book->open_orders() == 0);
  CHECK(book->counters().executed_qty == 100);  // clamped to what existed
  CHECK(rep.stats().clamped == 1);
}

TEST_CASE_TEMPLATE("share conservation holds across a replayed session", L, MapLadder,
                   DenseLadder) {
  MultiBook<L> books = [] {
    if constexpr (std::is_same_v<L, DenseLadder>) {
      return MultiBook<L>(Price{0}, Price{2000000});
    } else {
      return MultiBook<L>();
    }
  }();
  Replayer<L> rep(books);

  Writer w;
  w.system_event(1, 'Q');
  std::uint64_t ts = 10;
  OrderId ref = 1;
  std::uint64_t seed = 0x1234;
  auto rng = [&seed]() {
    seed += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = seed;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  };
  std::vector<OrderId> live;
  for (int i = 0; i < 4000; ++i) {
    const std::uint64_t roll = rng() % 100;
    if (roll < 55 || live.empty()) {
      const Side side = rng() % 2 == 0 ? Side::Bid : Side::Ask;
      const Price px = static_cast<Price>(999000 + rng() % 2000);
      w.add_order(ts++, ref, side, static_cast<Qty>(1 + rng() % 500), kAapl, px);
      live.push_back(ref++);
    } else {
      const std::size_t k = static_cast<std::size_t>(rng() % live.size());
      const OrderId target = live[k];
      if (roll < 75) {
        const std::uint64_t match = ts;
        w.order_executed(ts++, target, static_cast<Qty>(1 + rng() % 100), match);
      } else if (roll < 90) {
        w.order_cancel(ts++, target, static_cast<Qty>(1 + rng() % 100));
      } else {
        w.order_delete(ts++, target);
        live[k] = live.back();
        live.pop_back();
      }
    }
  }
  REQUIRE(drive(rep, w).ok());

  const auto* book = books.find(kAapl);
  REQUIRE(book != nullptr);
  std::uint64_t resting = 0;
  for (const Side s : {Side::Bid, Side::Ask}) {
    book->for_each_level(s, [&](const Level& lvl) { resting += lvl.total_qty; });
  }
  const auto& c = book->counters();
  // Feed reconstruction consumes one side only, so executions count once.
  CHECK(c.added_qty == c.executed_qty + c.canceled_qty + resting);
  CHECK(c.traded_qty == 0);  // nothing was ever internally matched
  CHECK(c.executed_qty > 0);
  CHECK(rep.stats().adds > 0);
}
