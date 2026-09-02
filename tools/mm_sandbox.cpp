// Runs a naive market maker against a replayed ITCH session.
//
//   mm_sandbox <file.itch> --symbol AAPL [--half-spread N] [--size N]
//              [--max-position N] [--fill-model queue|optimistic] [--tick N]
//
// Two fill models, and the difference between them is itself a result:
//
//  - "optimistic": any trade printing at or through our quote fills us.
//    This is the model most hobby backtests use, silently.
//  - "queue" (default): our order joins the back of the queue at its price,
//    behind every share already resting there. The feed identifies each of
//    those orders, so their departures are tracked exactly; we can only be
//    filled once everyone ahead of us has left. Same strategy, same data, so
//    the gap between the two models measures what the optimistic assumption
//    was worth.
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "pricetime/itch.hpp"
#include "pricetime/itch_reader.hpp"
#include "pricetime/itch_stream.hpp"
#include "pricetime/itch_replay.hpp"
#include "pricetime/ladder_map.hpp"
#include "pricetime/market_maker.hpp"
#include "pricetime/multi_book.hpp"
#include "pricetime/queue_position.hpp"

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

// What one message does to one displayed order, read BEFORE the replayer
// applies it (executions and deletes need the order's pre-event state).
struct OrderEvent {
  bool valid = false;
  bool is_execution = false;
  OrderId id = 0;
  Price price = 0;
  Side resting_side = Side::Bid;
  Qty shares = 0;  // shares leaving the order
};

template <class Books>
OrderEvent peek_event(Books& books, const std::uint8_t* m) {
  OrderEvent ev;
  const char type = static_cast<char>(m[0]);
  if (type != 'E' && type != 'C' && type != 'X' && type != 'D' && type != 'U') return ev;
  const OrderId ref = be64(m + 11);
  auto* book = books.book_for_order(ref);
  if (book == nullptr) return ev;
  const Order* o = book->find_order(ref);
  if (o == nullptr) return ev;
  ev.valid = true;
  ev.id = ref;
  ev.price = o->price;
  ev.resting_side = o->side;
  switch (type) {
    case 'E':
    case 'C':
      ev.is_execution = true;
      ev.shares = be32(m + 19);
      break;
    case 'X':
      ev.shares = be32(m + 19);
      break;
    case 'D':
    case 'U':  // the original order leaves the book entirely
      ev.shares = o->qty;
      break;
    default: break;
  }
  return ev;
}

// Snapshot of (id -> open qty) for every order resting at `price`, the queue
// our simulated order joins behind. Walks best -> worst and stops as soon as
// the target price has been passed.
template <class Book>
std::unordered_map<OrderId, Qty> orders_at_level(const Book& b, Side s, Price p) {
  std::unordered_map<OrderId, Qty> out;
  b.for_each_level(s, [&](const Level& lvl) -> bool {
    const bool past = s == Side::Bid ? lvl.price < p : lvl.price > p;
    if (past) return false;
    if (lvl.price == p) {
      for (const Order* o = lvl.head; o != nullptr; o = o->next) out.emplace(o->id, o->qty);
      return false;
    }
    return true;
  });
  return out;
}

struct SandboxConfig {
  std::string path;
  Symbol symbol{"AAPL"};
  MarketMakerConfig mm;
  bool queue_model = true;
  // The instrument's real price grid, in ITCH ticks: 100 = one cent, the
  // NASDAQ minimum increment for displayed orders in stocks >= $1. Quotes
  // must snap to this grid. The first real-data run quoted at sub-penny
  // prices no displayed order can occupy, and the queue model correctly
  // reported zero fills all day while the optimistic model "filled" 43,827
  // times at prices that cannot exist. Tick alignment is not a detail.
  Price tick = 100;
};

// Bids floor to the grid, asks ceil: snapping must never make a quote more
// aggressive than the strategy asked for.
Price snap_bid(Price p, Price tick) { return p - (((p % tick) + tick) % tick); }
Price snap_ask(Price p, Price tick) {
  const Price r = ((p % tick) + tick) % tick;
  return r == 0 ? p : p + (tick - r);
}

}  // namespace

int main(int argc, char** argv) {
  SandboxConfig cfg;
  cfg.mm.half_spread_ticks = 1;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--symbol") == 0 && i + 1 < argc) {
      cfg.symbol = Symbol(argv[++i]);
    } else if (std::strcmp(argv[i], "--half-spread") == 0 && i + 1 < argc) {
      cfg.mm.half_spread_ticks = std::atoll(argv[++i]);
    } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
      cfg.mm.quote_size = static_cast<Qty>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--max-position") == 0 && i + 1 < argc) {
      cfg.mm.max_position = std::atoll(argv[++i]);
    } else if (std::strcmp(argv[i], "--fill-model") == 0 && i + 1 < argc) {
      cfg.queue_model = std::strcmp(argv[++i], "optimistic") != 0;
    } else if (std::strcmp(argv[i], "--tick") == 0 && i + 1 < argc) {
      cfg.tick = std::atoll(argv[++i]);
    } else if (argv[i][0] != '-') {
      cfg.path = argv[i];
    }
  }
  if (cfg.path.empty()) {
    std::fprintf(stderr,
                 "usage: mm_sandbox <file.itch> --symbol AAPL [--half-spread N] [--size N] "
                 "[--max-position N] [--fill-model queue|optimistic] [--tick N]\n");
    return 2;
  }

  MultiBook<MapLadder> books;
  Replayer<MapLadder> rep(books);
  rep.track_only({cfg.symbol});
  MarketMaker mm(cfg.mm);

  QueuePosition our_bid;
  QueuePosition our_ask;
  std::uint64_t mid_updates = 0;
  std::uint64_t prints_seen = 0;
  std::uint64_t requotes = 0;
  Price last_mid = 0;
  bool have_mid = false;

  const auto t0 = std::chrono::steady_clock::now();
  const ReadResult r = for_each_framed_stream(
      cfg.path, [&](const std::uint8_t* m, std::size_t len) {
        const OrderEvent ev = peek_event(books, m);
        const Price mid_before = last_mid;
        const bool had_mid = have_mid;

        rep.apply(m, len);

        if (ev.valid && had_mid) {
          if (ev.is_execution) ++prints_seen;

          if (cfg.queue_model) {
            // Queue model: an execution at our price and side reaches us
            // only if everyone recorded ahead has already left; otherwise it
            // consumes their shares and moves us forward.
            QueuePosition& q = ev.resting_side == Side::Bid ? our_bid : our_ask;
            if (ev.is_execution && q.active() && q.price() == ev.price && q.at_front()) {
              const Qty fill = q.take_fill(ev.shares);
              if (fill > 0) {
                mm.on_fill(FillEvent{ev.resting_side, ev.price, fill,
                                     rep.stats().last_timestamp, mid_before});
              }
            } else {
              our_bid.on_shares_removed(ev.id, ev.shares);
              our_ask.on_shares_removed(ev.id, ev.shares);
            }
          } else if (ev.is_execution) {
            // Optimistic model: every print at or through our quote fills us.
            const std::uint64_t ts = rep.stats().last_timestamp;
            const Qty fill = ev.shares < cfg.mm.quote_size ? ev.shares : cfg.mm.quote_size;
            const Price obid = snap_bid(mm.bid_quote(mid_before), cfg.tick);
            const Price oask = snap_ask(mm.ask_quote(mid_before), cfg.tick);
            if (ev.resting_side == Side::Bid && mm.wants_bid() && obid >= ev.price) {
              mm.on_fill(FillEvent{Side::Bid, obid, fill, ts, mid_before});
            } else if (ev.resting_side == Side::Ask && mm.wants_ask() && oask <= ev.price) {
              mm.on_fill(FillEvent{Side::Ask, oask, fill, ts, mid_before});
            }
          }
        }

        // Refresh the mid once the book has absorbed the message.
        const auto* book = books.find(cfg.symbol);
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

        // Re-quote: place or move each side to its desired price. Moving
        // means abandoning the old queue spot and joining the back of the
        // new level, exactly the queue cost a real requote pays, which is
        // why requoting at the touch on every tick is not free.
        if (cfg.queue_model) {
          const Price want_bid = snap_bid(mm.bid_quote(mid), cfg.tick);
          if (mm.wants_bid() && (!our_bid.active() || our_bid.price() != want_bid)) {
            our_bid.cancel();
            our_bid.place(want_bid, Side::Bid, cfg.mm.quote_size,
                          orders_at_level(*book, Side::Bid, want_bid));
            ++requotes;
          }
          const Price want_ask = snap_ask(mm.ask_quote(mid), cfg.tick);
          if (mm.wants_ask() && (!our_ask.active() || our_ask.price() != want_ask)) {
            our_ask.cancel();
            our_ask.place(want_ask, Side::Ask, cfg.mm.quote_size,
                          orders_at_level(*book, Side::Ask, want_ask));
            ++requotes;
          }
        }
      });

  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (!r.ok()) {
    std::fprintf(stderr, "warning: parse stopped early at byte %s\n", commas(r.offset).c_str());
  }

  const std::int64_t total = mm.total_pnl_ticks(last_mid);
  const std::int64_t inventory = mm.inventory_value_ticks(last_mid);

  std::printf("symbol             %s\n", cfg.symbol.str().c_str());
  std::printf("fill model         %s\n", cfg.queue_model ? "queue-position (exact)" : "optimistic");
  std::printf("messages replayed  %s in %.2f s\n", commas(rep.stats().messages).c_str(), secs);
  std::printf("trade prints seen  %s\n", commas(prints_seen).c_str());
  std::printf("mid updates        %s   requotes %s\n", commas(mid_updates).c_str(),
              commas(requotes).c_str());
  std::printf("\nstrategy           half-spread %" PRId64 " ticks, size %u, max position %" PRId64
              ", grid %" PRId64 " ticks\n",
              cfg.mm.half_spread_ticks, cfg.mm.quote_size, cfg.mm.max_position, cfg.tick);
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
      "\n  Spread captured minus the adverse move is the edge. A positive\n"
      "  capture alongside a more negative markout means the quotes are\n"
      "  being picked off by better-informed flow.\n");

  if (cfg.queue_model) {
    std::printf(
        "\nremaining model caveats\n"
        "  * Once we reach the front, the aggressor that fills us also fills\n"
        "    the real order it historically hit, so liquidity at our level is\n"
        "    double-counted by our participation. Fixing this needs\n"
        "    counterfactual replay, which changes the question being asked.\n"
        "  * No latency: requotes happen instantly on every book change.\n"
        "  * No fees, rebates, or borrow costs.\n"
        "  Compare against --fill-model optimistic to see what ignoring\n"
        "  queue position is worth.\n");
  } else {
    std::printf(
        "\nfill-model caveats (read before using any number above)\n"
        "  * Queue position is ignored: every print at or through our price\n"
        "    fills us, where a real order waits behind everyone already\n"
        "    resting at that level. Run the default queue model instead.\n"
        "  * No latency, no market impact, no fees or rebates.\n");
  }
  return 0;
}
