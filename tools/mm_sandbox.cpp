// Runs a naive market maker against a replayed ITCH session.
//
//   mm_sandbox <file.itch> --symbol AAPL [--half-spread N] [--size N]
//              [--max-position N]
//
// What this is: a harness for measuring how a simple quoting strategy fares
// against real order flow, decomposed into spread captured versus adverse
// selection suffered.
//
// What this is NOT: a backtest you should believe. The fill model is
// optimistic in ways that matter, and they are printed with the results
// rather than buried here, because a backtest whose assumptions are invisible
// is worse than no backtest at all.
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pricetime/itch.hpp"
#include "pricetime/itch_reader.hpp"
#include "pricetime/itch_replay.hpp"
#include "pricetime/ladder_map.hpp"
#include "pricetime/market_maker.hpp"
#include "pricetime/mmap_file.hpp"
#include "pricetime/multi_book.hpp"

using namespace pricetime;
using namespace pricetime::itch;

namespace {

std::string commas(std::uint64_t v) {
  std::string s = std::to_string(v);
  for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(s.size()) - 3; i > 0; i -= 3) {
    s.insert(static_cast<std::size_t>(i), ",");
  }
  return s;
}

// ITCH prices carry four implied decimal places.
double dollars(std::int64_t ticks_times_shares) {
  return static_cast<double>(ticks_times_shares) / 10000.0;
}

// What a single execution message tells us about the trade that just printed.
struct TradePrint {
  bool valid = false;
  Price price = 0;
  Side resting_side = Side::Bid;  // the side the PASSIVE order was on
  Qty shares = 0;
};

// An execution message names only an order reference, so the price and side
// of the trade must be read off the resting order BEFORE the replayer
// consumes it.
template <class Books>
TradePrint peek_trade(Books& books, const std::uint8_t* m) {
  TradePrint t;
  const char type = static_cast<char>(m[0]);
  if (type != 'E' && type != 'C') return t;
  const OrderId ref = be64(m + 11);
  auto* book = books.book_for_order(ref);
  if (book == nullptr) return t;
  const Order* o = book->find_order(ref);
  if (o == nullptr) return t;
  t.valid = true;
  t.price = o->price;
  t.resting_side = o->side;
  t.shares = be32(m + 19);
  return t;
}

}  // namespace

int main(int argc, char** argv) {
  std::string path;
  Symbol symbol("AAPL");
  MarketMakerConfig cfg;
  cfg.half_spread_ticks = 1;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--symbol") == 0 && i + 1 < argc) {
      symbol = Symbol(argv[++i]);
    } else if (std::strcmp(argv[i], "--half-spread") == 0 && i + 1 < argc) {
      cfg.half_spread_ticks = std::atoll(argv[++i]);
    } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
      cfg.quote_size = static_cast<Qty>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--max-position") == 0 && i + 1 < argc) {
      cfg.max_position = std::atoll(argv[++i]);
    } else if (argv[i][0] != '-') {
      path = argv[i];
    }
  }
  if (path.empty()) {
    std::fprintf(stderr,
                 "usage: mm_sandbox <file.itch> --symbol AAPL [--half-spread N] "
                 "[--size N] [--max-position N]\n");
    return 2;
  }

  MmapFile file;
  if (!file.open(path)) {
    std::fprintf(stderr, "error: %s\n", file.error().c_str());
    return 2;
  }

  MultiBook<MapLadder> books;
  Replayer<MapLadder> rep(books);
  rep.track_only({symbol});
  MarketMaker mm(cfg);

  std::uint64_t mid_updates = 0;
  std::uint64_t prints_seen = 0;
  Price last_mid = 0;
  bool have_mid = false;

  const auto t0 = std::chrono::steady_clock::now();
  const ReadResult r = for_each_framed_message(
      file.data(), file.size(), [&](const std::uint8_t* m, std::size_t len) {
        // The mid BEFORE this message is what our quotes were resting
        // against, so it is what both the fill and the markout reference.
        const TradePrint print = peek_trade(books, m);
        const Price mid_before = last_mid;
        const bool had_mid = have_mid;

        rep.apply(m, len);

        // --- fill model ---------------------------------------------------
        // A passive quote is filled when a trade prints at or through its
        // price. If the print's resting side was the bid, a seller crossed
        // the spread; our bid at an equal or better price would have been hit
        // first, so we buy. Symmetrically for the ask.
        if (print.valid && had_mid) {
          const std::uint64_t ts = rep.stats().last_timestamp;
          const Qty fill = print.shares < mm.quote_size() ? print.shares : mm.quote_size();
          if (print.resting_side == Side::Bid && mm.wants_bid() &&
              mm.bid_quote(mid_before) >= print.price) {
            mm.on_fill(FillEvent{Side::Bid, mm.bid_quote(mid_before), fill, ts, mid_before});
          } else if (print.resting_side == Side::Ask && mm.wants_ask() &&
                     mm.ask_quote(mid_before) <= print.price) {
            mm.on_fill(FillEvent{Side::Ask, mm.ask_quote(mid_before), fill, ts, mid_before});
          }
          ++prints_seen;
        }

        // Refresh the mid once the book has absorbed the message.
        const auto* book = books.find(symbol);
        if (book == nullptr) return;
        const Level* bid = book->best(Side::Bid);
        const Level* ask = book->best(Side::Ask);
        if (bid == nullptr || ask == nullptr) return;
        const Price mid = (bid->price + ask->price) / 2;
        if (!have_mid || mid != last_mid) {
          mm.on_mid(mid, rep.stats().last_timestamp);
          ++mid_updates;
        }
        last_mid = mid;
        have_mid = true;
      });

  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (!r.ok()) {
    std::fprintf(stderr, "warning: parse stopped early at byte %s\n", commas(r.offset).c_str());
  }

  const std::int64_t total = mm.total_pnl_ticks(last_mid);
  const std::int64_t inventory = mm.inventory_value_ticks(last_mid);

  std::printf("symbol             %s\n", symbol.str().c_str());
  std::printf("messages replayed  %s in %.2f s\n", commas(rep.stats().messages).c_str(), secs);
  std::printf("trade prints seen  %s\n", commas(prints_seen).c_str());
  std::printf("mid updates        %s\n", commas(mid_updates).c_str());
  std::printf("\nstrategy           half-spread %" PRId64 " ticks, size %u, max position %" PRId64
              "\n",
              cfg.half_spread_ticks, cfg.quote_size, cfg.max_position);
  std::printf("fills              %s (%s shares)\n", commas(mm.fills()).c_str(),
              commas(mm.volume()).c_str());
  std::printf("final position     %" PRId64 " shares\n", mm.position());
  std::printf("peak |position|    %" PRId64 " shares\n", mm.peak_abs_position());

  std::printf("\nP&L (inventory marked at mid %" PRId64 ")\n", last_mid);
  std::printf("  cash             %+.2f\n", dollars(mm.cash_ticks()));
  std::printf("  inventory        %+.2f\n", dollars(inventory));
  std::printf("  total            %+.2f\n", dollars(total));
  if (mm.volume() > 0) {
    std::printf("  per share        %+.5f\n", dollars(total) / static_cast<double>(mm.volume()));
  }

  std::printf("\ndecomposition\n");
  std::printf("  spread captured  %+.2f ticks/share\n", mm.captured_ticks_per_share());
  const std::uint64_t horizons[] = {1000000ull, 10000000ull, 100000000ull, 1000000000ull};
  for (const std::uint64_t horizon : horizons) {
    const Markout mk = mm.markout(horizon);
    if (mk.samples == 0) continue;
    std::printf("  markout %6s ms  %+.2f ticks/share over %s fills\n",
                std::to_string(horizon / 1000000ull).c_str(), mk.mean_ticks,
                commas(mk.samples).c_str());
  }
  std::printf(
      "\n  Spread captured minus the adverse move is the real edge. A positive\n"
      "  capture alongside a more negative markout means the quotes are being\n"
      "  picked off by better-informed flow.\n");

  std::printf(
      "\nfill-model caveats (read before believing any number above)\n"
      "  * Queue position is ignored: every print at or through our price\n"
      "    fills us, where a real order waits behind everyone already resting\n"
      "    at that level. This is the largest single source of optimism.\n"
      "  * No latency: quotes reprice instantly on every book change.\n"
      "  * No market impact: our orders never enter the book, so nobody\n"
      "    reacts to them and they never displace the liquidity they imitate.\n"
      "  * No fees, rebates, or borrow costs, which for a real market maker\n"
      "    are frequently the entire margin.\n");
  return 0;
}
