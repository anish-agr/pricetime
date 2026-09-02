#include <doctest/doctest.h>

#include <cstdint>

#include "pricetime/market_maker.hpp"

using namespace pricetime;

TEST_CASE("quotes straddle the mid by the configured half spread") {
  MarketMakerConfig cfg;
  cfg.half_spread_ticks = 5;
  cfg.skew_per_100_shares = 0;  // isolate the spread from the skew
  MarketMaker mm(cfg);

  CHECK(mm.bid_quote(1000) == 995);
  CHECK(mm.ask_quote(1000) == 1005);
  CHECK(mm.position() == 0);
  CHECK(mm.wants_bid());
  CHECK(mm.wants_ask());
}

TEST_CASE("inventory skews quotes to push the position back toward flat") {
  MarketMakerConfig cfg;
  cfg.half_spread_ticks = 5;
  cfg.skew_per_100_shares = 1;
  MarketMaker mm(cfg);

  // Buy 500 shares: now long, so both quotes should drop, making us more
  // likely to sell and less likely to buy again.
  mm.on_fill(FillEvent{Side::Bid, 995, 500, 1, 1000});
  CHECK(mm.position() == 500);
  CHECK(mm.bid_quote(1000) == 990);  // 5 ticks of skew on 500 shares
  CHECK(mm.ask_quote(1000) == 1000);

  // Sell 1000: now short, quotes move up.
  mm.on_fill(FillEvent{Side::Ask, 1005, 1000, 2, 1000});
  CHECK(mm.position() == -500);
  CHECK(mm.bid_quote(1000) == 1000);
  CHECK(mm.ask_quote(1000) == 1010);
}

TEST_CASE("position limits stop us adding to a maxed-out position") {
  MarketMakerConfig cfg;
  cfg.max_position = 1000;
  MarketMaker mm(cfg);

  mm.on_fill(FillEvent{Side::Bid, 995, 1000, 1, 1000});
  CHECK(mm.position() == 1000);
  CHECK_FALSE(mm.wants_bid());  // no more buying
  CHECK(mm.wants_ask());        // still happy to sell

  mm.on_fill(FillEvent{Side::Ask, 1005, 2000, 2, 1000});
  CHECK(mm.position() == -1000);
  CHECK(mm.wants_bid());
  CHECK_FALSE(mm.wants_ask());
}

TEST_CASE("cash and P&L accounting is exact") {
  MarketMaker mm;
  // Buy 100 @ 995, sell 100 @ 1005: a clean 10-tick round trip on 100 shares.
  mm.on_fill(FillEvent{Side::Bid, 995, 100, 1, 1000});
  CHECK(mm.cash_ticks() == -99500);
  CHECK(mm.position() == 100);

  mm.on_fill(FillEvent{Side::Ask, 1005, 100, 2, 1000});
  CHECK(mm.cash_ticks() == 1000);  // 100500 - 99500
  CHECK(mm.position() == 0);
  CHECK(mm.total_pnl_ticks(1000) == 1000);  // flat, so the mark is irrelevant
  CHECK(mm.fills() == 2);
  CHECK(mm.volume() == 200);
}

TEST_CASE("open inventory is marked to the mid, not the last trade") {
  MarketMaker mm;
  mm.on_fill(FillEvent{Side::Bid, 1000, 100, 1, 1000});
  CHECK(mm.cash_ticks() == -100000);

  // Flat P&L when the mid has not moved.
  CHECK(mm.total_pnl_ticks(1000) == 0);
  // Mid rises 10 ticks: the long position is worth 1000 more.
  CHECK(mm.total_pnl_ticks(1010) == 1000);
  CHECK(mm.inventory_value_ticks(1010) == 101000);
  // Mid falls: the loss shows up immediately rather than hiding until exit.
  CHECK(mm.total_pnl_ticks(990) == -1000);
}

TEST_CASE("a short position marks correctly too") {
  MarketMaker mm;
  mm.on_fill(FillEvent{Side::Ask, 1000, 100, 1, 1000});
  CHECK(mm.position() == -100);
  CHECK(mm.cash_ticks() == 100000);
  CHECK(mm.total_pnl_ticks(1000) == 0);
  CHECK(mm.total_pnl_ticks(990) == 1000);   // market fell, short profits
  CHECK(mm.total_pnl_ticks(1010) == -1000);
}

TEST_CASE("captured spread measures edge against the mid at fill time") {
  MarketMaker mm;
  // Bought 5 ticks below the mid and sold 5 above: 5 ticks captured per share.
  mm.on_fill(FillEvent{Side::Bid, 995, 100, 1, 1000});
  mm.on_fill(FillEvent{Side::Ask, 1005, 100, 2, 1000});
  CHECK(mm.captured_ticks_per_share() == doctest::Approx(5.0));

  // A fill at the mid captures nothing, and drags the average down.
  MarketMaker mm2;
  mm2.on_fill(FillEvent{Side::Bid, 995, 100, 1, 1000});
  mm2.on_fill(FillEvent{Side::Ask, 1000, 100, 2, 1000});
  CHECK(mm2.captured_ticks_per_share() == doctest::Approx(2.5));
}

TEST_CASE("peak absolute position tracks the worst exposure, not the final one") {
  MarketMaker mm;
  mm.on_fill(FillEvent{Side::Bid, 1000, 900, 1, 1000});
  mm.on_fill(FillEvent{Side::Ask, 1000, 900, 2, 1000});
  CHECK(mm.position() == 0);          // ends flat
  CHECK(mm.peak_abs_position() == 900);  // but was very long along the way
}

// Adverse selection: we get filled and the market immediately moves against
// us. This is the measurement that distinguishes a real market maker from one
// that merely looks busy.
TEST_CASE("markout detects adverse selection") {
  MarketMaker mm;
  mm.on_mid(1000, 0);
  // We buy at 995 when the mid is 1000, so 5 ticks of apparent edge.
  mm.on_fill(FillEvent{Side::Bid, 995, 100, 100, 1000});
  // The mid then collapses: we were bought into by informed flow.
  mm.on_mid(990, 200);
  mm.on_mid(980, 1100);

  const Markout m = mm.markout(1000);
  CHECK(m.samples == 1);
  CHECK(m.mean_ticks == doctest::Approx(-20.0));  // bought, mid fell 20
  // The strategy captured 5 ticks of spread and lost 20 to the move.
  CHECK(mm.captured_ticks_per_share() == doctest::Approx(5.0));
}

TEST_CASE("markout is positive when the market moves our way") {
  MarketMaker mm;
  mm.on_mid(1000, 0);
  mm.on_fill(FillEvent{Side::Ask, 1005, 100, 100, 1000});  // sold
  mm.on_mid(980, 1100);                                    // market fell
  const Markout m = mm.markout(1000);
  CHECK(m.samples == 1);
  CHECK(m.mean_ticks == doctest::Approx(20.0));  // short and the mid dropped
}

TEST_CASE("markout skips fills without enough future history") {
  MarketMaker mm;
  mm.on_mid(1000, 0);
  mm.on_fill(FillEvent{Side::Bid, 995, 100, 100, 1000});
  // Only 50ns of history after the fill; a 1000ns horizon cannot be evaluated.
  mm.on_mid(1001, 150);
  const Markout m = mm.markout(1000);
  CHECK(m.samples == 0);
  CHECK(m.mean_ticks == doctest::Approx(0.0));
}

TEST_CASE("markout averages across many fills at several horizons") {
  MarketMaker mm;
  mm.on_mid(1000, 0);
  // Two buys; the mid rises after the first and falls after the second.
  mm.on_fill(FillEvent{Side::Bid, 995, 100, 100, 1000});
  mm.on_mid(1010, 1100);
  mm.on_fill(FillEvent{Side::Bid, 1005, 100, 1200, 1010});
  mm.on_mid(1000, 2300);
  mm.on_mid(1000, 5000);

  const Markout m = mm.markout(1000);
  CHECK(m.samples == 2);
  // +10 on the first, -10 on the second: they cancel.
  CHECK(m.mean_ticks == doctest::Approx(0.0));
}
