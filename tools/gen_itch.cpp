// Writes a synthetic NASDAQ ITCH 5.0 BinaryFILE.
//
//   gen_itch <out.itch> [--messages N] [--symbols AAPL,MSFT] [--seed N]
//
// The real thing is a multi-gigabyte download from NASDAQ's public FTP. This
// produces a structurally identical file so the parser, the replay driver,
// and the throughput measurement can be exercised end to end: in CI, on a
// laptop, and before the download finishes.
//
// What it does not reproduce is the statistical character of real order flow:
// arrival clustering, price distributions around the touch, the cancel/trade
// ratio, or the intraday volume profile. Numbers taken against this file
// measure the parser and the book, not the market.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "pricetime/itch_writer.hpp"
#include "pricetime/symbol.hpp"
#include "cli.hpp"

using namespace pricetime;
using namespace pricetime::cli;

namespace {

std::uint64_t next_random(std::uint64_t& state) {
  state += 0x9E3779B97F4A7C15ull;
  std::uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

struct LiveOrder {
  OrderId ref = 0;
  Qty shares = 0;
  std::size_t symbol_index = 0;
  Side side = Side::Bid;
};

}  // namespace

int main(int argc, char** argv) {
  std::string out_path;
  std::uint64_t target_messages = 1000000;
  std::uint64_t seed = 0xA55E7;
  std::vector<Symbol> symbols;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--messages") == 0 && i + 1 < argc) {
      target_messages = std::strtoull(argv[++i], nullptr, 10);
    } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      seed = std::strtoull(argv[++i], nullptr, 10);
    } else if (std::strcmp(argv[i], "--symbols") == 0 && i + 1 < argc) {
      symbols = parse_symbols(argv[++i]);
    } else if (argv[i][0] != '-') {
      out_path = argv[i];
    }
  }
  if (out_path.empty()) {
    std::fprintf(stderr,
                 "usage: gen_itch <out.itch> [--messages N] [--symbols AAPL,MSFT] "
                 "[--seed N]\n");
    return 2;
  }
  if (symbols.empty()) {
    symbols = {Symbol("AAPL"), Symbol("MSFT"), Symbol("TSLA"), Symbol("NVDA")};
  }

  itch::Writer w;
  std::uint64_t ts = 9ull * 3600 * 1000000000ull;  // 09:00:00 in nanoseconds
  w.system_event(ts, 'O');
  for (const Symbol& s : symbols) w.stock_directory(ts, s);
  ts = 9ull * 3600 * 1000000000ull + 1800ull * 1000000000ull;  // 09:30
  w.system_event(ts, 'Q');

  // Each symbol keeps its own mid so the books look independent.
  std::vector<Price> mids(symbols.size());
  for (std::size_t i = 0; i < symbols.size(); ++i) {
    mids[i] = static_cast<Price>(500000 + (i + 1) * 250000);
  }

  std::vector<LiveOrder> live;
  live.reserve(1u << 16);
  OrderId next_ref = 1;
  std::uint64_t match_number = 1;

  // Message mix roughly in the spirit of a real session: far more adds and
  // cancels than trades. Not calibrated to a real day; see the header note.
  while (w.message_count() < target_messages) {
    ts += 1000 + next_random(seed) % 50000;  // microsecond-ish spacing
    const std::uint64_t roll = next_random(seed) % 100;

    if (roll < 45 || live.empty()) {
      const std::size_t si = static_cast<std::size_t>(next_random(seed) % symbols.size());
      const Side side = next_random(seed) % 2 == 0 ? Side::Bid : Side::Ask;
      // Orders cluster near the touch, as real ones do, but never cross:
      // the exchange's own book is never crossed, and a replayed file that
      // produced one would mean the reconstruction was wrong.
      const Price offset = static_cast<Price>(1 + next_random(seed) % 500);
      const Price price = side == Side::Bid ? mids[si] - offset : mids[si] + offset;
      const Qty shares = static_cast<Qty>(100 * (1 + next_random(seed) % 20));
      const OrderId ref = next_ref++;
      if (next_random(seed) % 10 == 0) {
        w.add_order_mpid(ts, ref, side, shares, symbols[si], price);
      } else {
        w.add_order(ts, ref, side, shares, symbols[si], price);
      }
      live.push_back(LiveOrder{ref, shares, si, side});
      continue;
    }

    const std::size_t idx = static_cast<std::size_t>(next_random(seed) % live.size());
    LiveOrder& o = live[idx];

    if (roll < 70) {  // delete: the most common end for a resting order
      w.order_delete(ts, o.ref);
      o = live.back();
      live.pop_back();
    } else if (roll < 82) {  // partial cancel
      const Qty cut = static_cast<Qty>(1 + next_random(seed) % o.shares);
      w.order_cancel(ts, o.ref, cut);
      if (cut >= o.shares) {
        o = live.back();
        live.pop_back();
      } else {
        o.shares -= cut;
      }
    } else if (roll < 94) {  // execution
      const Qty fill = static_cast<Qty>(1 + next_random(seed) % o.shares);
      if (next_random(seed) % 5 == 0) {
        w.order_executed_with_price(ts, o.ref, fill, match_number++, true, mids[o.symbol_index]);
      } else {
        w.order_executed(ts, o.ref, fill, match_number++);
      }
      if (fill >= o.shares) {
        o = live.back();
        live.pop_back();
      } else {
        o.shares -= fill;
      }
    } else if (roll < 98) {  // replace
      const std::size_t si = o.symbol_index;
      const Price offset = static_cast<Price>(1 + next_random(seed) % 500);
      const Qty shares = static_cast<Qty>(100 * (1 + next_random(seed) % 20));
      const OrderId fresh = next_ref++;
      // A replace message carries no side, so the new price must be chosen
      // for the side the original order was on. Pricing every replace on the
      // bid side produced files with crossed books, which the replay tool's
      // sanity check caught, since a real exchange never crosses its own
      // book and a reconstruction that does is either bad input or a bug.
      const Price price = o.side == Side::Bid ? mids[si] - offset : mids[si] + offset;
      w.order_replace(ts, o.ref, fresh, shares, price);
      o.ref = fresh;
      o.shares = shares;
    } else {  // a trade against hidden liquidity: tape only, never the book
      w.trade_non_cross(ts, 0, Side::Bid, static_cast<Qty>(100), symbols[o.symbol_index],
                        mids[o.symbol_index], match_number++);
    }
  }

  ts = 16ull * 3600 * 1000000000ull;  // 16:00
  w.system_event(ts, 'M');
  w.system_event(ts + 1, 'C');

  // ofstream rather than fopen: MSVC deprecates fopen and CI builds with
  // warnings as errors.
  std::ofstream out(out_path, std::ios::binary);
  if (!out) {
    std::fprintf(stderr, "error: cannot write %s\n", out_path.c_str());
    return 2;
  }
  const auto& bytes = w.bytes();
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.close();
  if (!out) {
    std::fprintf(stderr, "error: short write to %s\n", out_path.c_str());
    return 2;
  }

  std::printf("wrote %s: %zu messages, %zu bytes, %zu symbols, %zu orders still open\n",
              out_path.c_str(), w.message_count(), bytes.size(), symbols.size(), live.size());
  return 0;
}
