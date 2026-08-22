// Rebuilds order books from a NASDAQ TotalView-ITCH 5.0 file and reports
// throughput plus sanity checks.
//
//   replay_itch <file.itch> [--symbols AAPL,MSFT] [--ladder map|dense]
//               [--depth N] [--progress]
//
// The file is memory-mapped, so a multi-gigabyte day costs no user-space copy
// and the reported message rate reflects parsing and book-building rather
// than I/O buffering. Sample files are published on NASDAQ's public FTP.
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pricetime/itch_reader.hpp"
#include "pricetime/itch_replay.hpp"
#include "pricetime/ladder_dense.hpp"
#include "pricetime/ladder_map.hpp"
#include "pricetime/mmap_file.hpp"
#include "pricetime/multi_book.hpp"

using namespace pricetime;
using namespace pricetime::itch;

namespace {

std::vector<Symbol> parse_symbols(const char* csv) {
  std::vector<Symbol> out;
  std::string cur;
  for (const char* p = csv;; ++p) {
    if (*p == ',' || *p == '\0') {
      if (!cur.empty()) out.push_back(Symbol(cur));
      cur.clear();
      if (*p == '\0') break;
    } else {
      cur.push_back(*p);
    }
  }
  return out;
}

// ITCH timestamps are nanoseconds since midnight US/Eastern.
std::string clock_of(std::uint64_t ns) {
  const std::uint64_t total_s = ns / 1000000000ull;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64, total_s / 3600,
                (total_s / 60) % 60, total_s % 60);
  return buf;
}

std::string commas(std::uint64_t v) {
  std::string s = std::to_string(v);
  for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(s.size()) - 3; i > 0; i -= 3) {
    s.insert(static_cast<std::size_t>(i), ",");
  }
  return s;
}

const char* status_text(ReadStatus s) {
  switch (s) {
    case ReadStatus::Ok: return "ok";
    case ReadStatus::TruncatedPrefix: return "truncated length prefix";
    case ReadStatus::TruncatedMessage: return "truncated message";
    case ReadStatus::UnknownType: return "unknown message type";
    case ReadStatus::LengthMismatch: return "framing length contradicts the spec";
    case ReadStatus::ZeroLength: return "zero-length message";
  }
  return "?";
}

struct Options {
  std::string path;
  std::vector<Symbol> symbols;
  bool dense = false;
  std::size_t depth = 5;
  bool progress = false;
};

// Walks every book and verifies the properties a correctly reconstructed
// exchange book must have. A crossed book is the canonical signal that the
// reconstruction logic is wrong.
template <class Books>
int check_and_report(const Books& books, const Options& opt) {
  int crossed = 0;
  books.for_each_book([&](const Symbol& sym, const auto& book) {
    const Level* bid = book.best(Side::Bid);
    const Level* ask = book.best(Side::Ask);
    const bool bad = bid != nullptr && ask != nullptr && bid->price >= ask->price;
    if (bad) ++crossed;

    std::uint64_t resting = 0;
    for (const Side s : {Side::Bid, Side::Ask}) {
      book.for_each_level(s, [&](const Level& lvl) { resting += lvl.total_qty; });
    }
    const auto& c = book.counters();
    const bool conserved =
        c.added_qty == c.executed_qty + c.canceled_qty + 2 * c.traded_qty + resting;

    std::printf("\n%-8s  orders %s  shares %s%s%s\n", sym.str().c_str(),
                commas(book.open_orders()).c_str(), commas(resting).c_str(),
                bad ? "  [CROSSED BOOK]" : "",
                conserved ? "" : "  [SHARE CONSERVATION VIOLATED]");
    if (bid != nullptr && ask != nullptr) {
      std::printf("          bid %" PRId64 " / ask %" PRId64 "  spread %" PRId64 "\n",
                  bid->price, ask->price, ask->price - bid->price);
    }
    const Depth d = book.depth(opt.depth);
    for (std::size_t i = 0; i < opt.depth; ++i) {
      const bool has_b = i < d.bids.size();
      const bool has_a = i < d.asks.size();
      if (!has_b && !has_a) break;
      std::printf("          ");
      if (has_b) {
        std::printf("%10" PRIu64 " @ %-10" PRId64, d.bids[i].qty, d.bids[i].price);
      } else {
        std::printf("%23s", "");
      }
      std::printf("  |  ");
      if (has_a) std::printf("%-10" PRId64 " x %" PRIu64, d.asks[i].price, d.asks[i].qty);
      std::printf("\n");
    }
  });
  return crossed;
}

template <class Ladder>
int run(const Options& opt, const MmapFile& file, MultiBook<Ladder>& books) {
  Replayer<Ladder> rep(books);
  if (!opt.symbols.empty()) rep.track_only(opt.symbols);

  const auto t0 = std::chrono::steady_clock::now();
  std::uint64_t since_tick = 0;
  const ReadResult r = for_each_framed_message(
      file.data(), file.size(), [&](const std::uint8_t* m, std::size_t len) {
        rep.apply(m, len);
        if (opt.progress && ++since_tick == 5000000) {
          since_tick = 0;
          std::fprintf(stderr, "  ... %s messages\n", commas(rep.stats().messages).c_str());
        }
      });
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  const ReplayStats& s = rep.stats();
  std::printf("file           %s\n", opt.path.c_str());
  std::printf("size           %s bytes\n", commas(file.size()).c_str());
  std::printf("parse status   %s", status_text(r.status));
  if (!r.ok()) {
    std::printf(" at byte %s", commas(r.offset).c_str());
    if (r.status == ReadStatus::LengthMismatch) {
      std::printf(" (type '%c' declared %s, spec says %s)", r.bad_type,
                  commas(r.declared_length).c_str(), commas(r.expected_length).c_str());
    }
  }
  std::printf("\n");
  std::printf("messages       %s\n", commas(s.messages).c_str());
  std::printf("elapsed        %.2f s\n", secs);
  if (secs > 0) {
    std::printf("throughput     %.2f M msg/s  (%.0f MB/s)\n",
                static_cast<double>(s.messages) / secs / 1e6,
                static_cast<double>(file.size()) / secs / (1024.0 * 1024.0));
  }
  std::printf("last timestamp %s  (session state '%c')\n", clock_of(s.last_timestamp).c_str(),
              s.session_state == ' ' ? '-' : s.session_state);
  std::printf("book events    adds %s  execs %s  cancels %s  deletes %s  replaces %s\n",
              commas(s.adds).c_str(), commas(s.executions).c_str(), commas(s.cancels).c_str(),
              commas(s.deletes).c_str(), commas(s.replaces).c_str());
  std::printf("shares         added %s  executed %s\n", commas(s.shares_added).c_str(),
              commas(s.shares_executed).c_str());
  std::printf("tape-only      non-cross trades %s\n", commas(s.trades).c_str());
  std::printf("skipped        symbol-filtered adds %s, unknown references %s\n",
              commas(s.skipped_symbol).c_str(), commas(s.unknown_reference).c_str());
  if (s.clamped > 0) {
    std::printf("WARNING        %s messages tried to remove more shares than were resting\n",
                commas(s.clamped).c_str());
  }
  std::printf("symbols        %s\n", commas(books.symbol_count()).c_str());

  const int crossed = check_and_report(books, opt);
  std::printf("\nsanity         %s\n",
              crossed == 0 ? "no crossed books" : "CROSSED BOOKS FOUND");
  return (r.ok() && crossed == 0) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--symbols") == 0 && i + 1 < argc) {
      opt.symbols = parse_symbols(argv[++i]);
    } else if (std::strcmp(argv[i], "--ladder") == 0 && i + 1 < argc) {
      opt.dense = std::strcmp(argv[++i], "dense") == 0;
    } else if (std::strcmp(argv[i], "--depth") == 0 && i + 1 < argc) {
      opt.depth = static_cast<std::size_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--progress") == 0) {
      opt.progress = true;
    } else if (argv[i][0] != '-') {
      opt.path = argv[i];
    }
  }
  if (opt.path.empty()) {
    std::fprintf(stderr,
                 "usage: replay_itch <file.itch> [--symbols AAPL,MSFT] "
                 "[--ladder map|dense] [--depth N] [--progress]\n");
    return 2;
  }

  MmapFile file;
  if (!file.open(opt.path)) {
    std::fprintf(stderr, "error: %s\n", file.error().c_str());
    return 2;
  }

  if (opt.dense) {
    // A dense ladder needs a bounded range. ITCH prices are 4 implied
    // decimals, so this covers $0.0000 through $200.0000 — fine for most
    // names and deliberately explicit about the limitation.
    MultiBook<DenseLadder> books{Price{0}, Price{2000000}};
    return run(opt, file, books);
  }
  MultiBook<MapLadder> books;
  return run(opt, file, books);
}
