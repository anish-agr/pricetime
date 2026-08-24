// Rebuilds order books from a NASDAQ TotalView-ITCH 5.0 file and reports
// throughput plus sanity checks.
//
//   replay_itch <file.itch> [--symbols AAPL,MSFT] [--ladder map|pooled|dense]
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
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "pricetime/itch_reader.hpp"
#include "pricetime/itch_replay.hpp"
#include "pricetime/itch_stream.hpp"
#include "pricetime/ladder_dense.hpp"
#include "pricetime/ladder_map.hpp"
#include "pricetime/ladder_pooled.hpp"
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
  bool pooled = false;
  std::size_t depth = 5;
  bool progress = false;
  bool minute_profile = false;
  bool use_mmap = false;  // default: buffered streaming (see itch_stream.hpp)
};

// Walks every book and verifies the properties a correctly reconstructed
// exchange book must have — a crossed book or broken share conservation is
// the canonical signal that reconstruction logic is wrong. EVERY book is
// checked; on a full-day file with thousands of symbols only the most active
// are printed, because a nine-thousand-line dump helps no one.
template <class Books>
int check_and_report(const Books& books, const Options& opt) {
  int failures = 0;
  struct Row {
    Symbol sym;
    std::uint64_t resting = 0;
  };
  std::vector<Row> rows;

  books.for_each_book([&](const Symbol& sym, const auto& book) {
    const Level* bid = book.best(Side::Bid);
    const Level* ask = book.best(Side::Ask);
    if (bid != nullptr && ask != nullptr && bid->price >= ask->price) {
      ++failures;
      std::printf("CROSSED BOOK   %-8s bid %" PRId64 " / ask %" PRId64 "\n", sym.str().c_str(),
                  bid->price, ask->price);
    }
    std::uint64_t resting = 0;
    for (const Side s : {Side::Bid, Side::Ask}) {
      book.for_each_level(s, [&](const Level& lvl) { resting += lvl.total_qty; });
    }
    const auto& c = book.counters();
    if (c.added_qty != c.executed_qty + c.canceled_qty + 2 * c.traded_qty + resting) {
      ++failures;
      std::printf("CONSERVATION VIOLATED  %-8s\n", sym.str().c_str());
    }
    rows.push_back(Row{sym, resting});
  });

  std::size_t show = rows.size();
  if (rows.size() > 24) {
    show = 12;
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.resting > b.resting; });
    std::printf("\n(top %zu of %s books by resting shares; every book was checked)\n", show,
                commas(rows.size()).c_str());
  }
  for (std::size_t r = 0; r < show; ++r) {
    const auto* book = books.find(rows[r].sym);
    if (book == nullptr) continue;
    const Level* bid = book->best(Side::Bid);
    const Level* ask = book->best(Side::Ask);
    std::printf("\n%-8s  orders %s  shares %s\n", rows[r].sym.str().c_str(),
                commas(book->open_orders()).c_str(), commas(rows[r].resting).c_str());
    if (bid != nullptr && ask != nullptr) {
      std::printf("          bid %" PRId64 " / ask %" PRId64 "  spread %" PRId64 "\n",
                  bid->price, ask->price, ask->price - bid->price);
    }
    const Depth d = book->depth(opt.depth);
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
  }
  return failures;
}

template <class Ladder>
int run(const Options& opt, std::uint64_t file_bytes, MultiBook<Ladder>& books) {
  Replayer<Ladder> rep(books);
  if (!opt.symbols.empty()) rep.track_only(opt.symbols);

  const auto t0 = std::chrono::steady_clock::now();
  std::uint64_t since_tick = 0;
  // 24h of minutes; ITCH timestamps are nanoseconds since midnight Eastern.
  std::vector<std::uint64_t> per_minute(24 * 60, 0);

  const auto on_message = [&](const std::uint8_t* m, std::size_t len) {
    rep.apply(m, len);
    if (opt.minute_profile) {
      const std::size_t minute =
          static_cast<std::size_t>(rep.stats().last_timestamp / 60000000000ull);
      if (minute < per_minute.size()) ++per_minute[minute];
    }
    if (opt.progress && ++since_tick == 5000000) {
      since_tick = 0;
      std::fprintf(stderr, "  ... %s messages\n", commas(rep.stats().messages).c_str());
    }
  };

  ReadResult r;
  if (opt.use_mmap) {
    // The mapped path, kept for the measured comparison: on a file larger
    // than RAM it loses badly to streaming (see itch_stream.hpp), and having
    // both switchable is what made that a number instead of an argument.
    MmapFile file;
    if (!file.open(opt.path)) {
      std::fprintf(stderr, "error: %s\n", file.error().c_str());
      return 2;
    }
    constexpr std::size_t kPrefetchChunk = 32u << 20;
    constexpr std::size_t kPrefetchAhead = 64u << 20;
    std::size_t next_prefetch = 0;
    file.prefetch(0, kPrefetchAhead);
    r = for_each_framed_message(file.data(), file.size(),
                                [&](const std::uint8_t* m, std::size_t len) {
                                  const auto pos = static_cast<std::size_t>(m - file.data());
                                  if (pos >= next_prefetch) {
                                    file.prefetch(pos + kPrefetchChunk, kPrefetchAhead);
                                    next_prefetch = pos + kPrefetchChunk;
                                  }
                                  on_message(m, len);
                                });
  } else {
    r = for_each_framed_stream(opt.path, on_message);
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  const ReplayStats& s = rep.stats();
  std::printf("file           %s\n", opt.path.c_str());
  std::printf("io             %s\n", opt.use_mmap ? "mmap + prefetch" : "buffered stream");
  std::printf("size           %s bytes\n", commas(file_bytes).c_str());
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
                static_cast<double>(file_bytes) / secs / (1024.0 * 1024.0));
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

  if (opt.minute_profile) {
    // The intraday shape is a market-microstructure fact worth seeing: the
    // opening burst, the lunchtime trough, the closing-auction wall.
    std::uint64_t peak = 1;
    std::size_t peak_min = 0;
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < per_minute.size(); ++i) {
      total += per_minute[i];
      if (per_minute[i] > peak) {
        peak = per_minute[i];
        peak_min = i;
      }
    }
    std::printf("\nintraday message rate (5-minute buckets, # = %s msgs)\n",
                commas(peak * 5 / 60).c_str());
    for (std::size_t b = 0; b + 5 <= per_minute.size(); b += 5) {
      std::uint64_t bucket = 0;
      for (std::size_t j = b; j < b + 5; ++j) bucket += per_minute[j];
      if (bucket == 0) continue;
      const int bars = static_cast<int>(bucket * 60 / (peak * 5));
      std::printf("  %02zu:%02zu %10s ", b / 60, b % 60, commas(bucket).c_str());
      for (int k = 0; k < bars; ++k) std::printf("#");
      std::printf("\n");
    }
    std::printf("  peak minute %02zu:%02zu with %s messages (%.1fx the day's mean)\n",
                peak_min / 60, peak_min % 60, commas(peak).c_str(),
                static_cast<double>(peak) * 1440.0 / (total > 0 ? total : 1));
  }

  const int failures = check_and_report(books, opt);
  std::printf("\nsanity         %s\n",
              failures == 0 ? "no crossed books, share conservation holds in every book"
                            : "SANITY CHECKS FAILED");
  return (r.ok() && failures == 0) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--symbols") == 0 && i + 1 < argc) {
      opt.symbols = parse_symbols(argv[++i]);
    } else if (std::strcmp(argv[i], "--ladder") == 0 && i + 1 < argc) {
      const char* v = argv[++i];
      opt.dense = std::strcmp(v, "dense") == 0;
      opt.pooled = std::strcmp(v, "pooled") == 0;
    } else if (std::strcmp(argv[i], "--depth") == 0 && i + 1 < argc) {
      opt.depth = static_cast<std::size_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--progress") == 0) {
      opt.progress = true;
    } else if (std::strcmp(argv[i], "--minute-profile") == 0) {
      opt.minute_profile = true;
    } else if (std::strcmp(argv[i], "--io") == 0 && i + 1 < argc) {
      opt.use_mmap = std::strcmp(argv[++i], "mmap") == 0;
    } else if (argv[i][0] != '-') {
      opt.path = argv[i];
    }
  }
  if (opt.path.empty()) {
    std::fprintf(stderr,
                 "usage: replay_itch <file.itch> [--symbols AAPL,MSFT] "
                 "[--ladder map|pooled|dense] [--depth N] [--io stream|mmap] [--minute-profile] [--progress]\n");
    return 2;
  }

  std::uint64_t file_bytes = 0;
  {
    std::ifstream probe(opt.path, std::ios::binary | std::ios::ate);
    if (!probe) {
      std::fprintf(stderr, "error: cannot open %s\n", opt.path.c_str());
      return 2;
    }
    file_bytes = static_cast<std::uint64_t>(probe.tellg());
  }

  if (opt.dense) {
    // A dense ladder needs a bounded range. ITCH prices are 4 implied
    // decimals, so this covers $0.0000 through $200.0000 — fine for most
    // names and deliberately explicit about the limitation.
    MultiBook<DenseLadder> books{Price{0}, Price{2000000}};
    return run(opt, file_bytes, books);
  }
  if (opt.pooled) {
    MultiBook<PooledMapLadder> books;
    return run(opt, file_bytes, books);
  }
  MultiBook<MapLadder> books;
  return run(opt, file_bytes, books);
}
