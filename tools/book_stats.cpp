// Market-microstructure statistics from a NASDAQ TotalView-ITCH 5.0 file.
//
//   book_stats <file.itch> [--sample-every N] [--out PREFIX] [--symbols A,B]
//              [--top N]
//
// replay_itch answers "did reconstruction work". This answers "what does the
// day actually look like", which is a different question and the one the
// design notes kept deferring to. Three of the measurements exist to settle
// open questions the profiling raised:
//
//  - Active price levels per book, and separately the price span between the
//    best and worst of them. A tree ladder pays for the count; an array
//    indexed by price offset pays for the span. Whether the array is viable
//    on real data turns entirely on how far apart those two numbers are, and
//    nothing in the repo knew that until this measured it.
//  - Order lifetime, add to removal. Decides whether an arena that never
//    returns memory is acceptable, and it is the clearest single picture of
//    how transient real quoting is.
//  - Cancel-to-trade ratio per symbol. The whole design rests on the claim
//    that cancels dominate; this checks it against the day rather than
//    against folklore.
//
// Two sampling decisions, both deliberate and both stated in the output so
// the numbers cannot be read as more than they are:
//
//  - Level counts are sampled every --sample-every messages rather than
//    continuously. Walking 8,892 books on every message would dominate the
//    runtime and change nothing about the distribution.
//  - Lifetimes are tracked for one order in 64 (reference number mod 64).
//    Holding a birth timestamp for every live order would add gigabytes to a
//    run that is already working-set-bound, and the day carries 118M adds, so
//    a 1/64 sample still leaves millions of observations.
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "pricetime/itch_replay.hpp"
#include "pricetime/itch_stream.hpp"
#include "pricetime/ladder_pooled.hpp"
#include "pricetime/multi_book.hpp"
#include "cli.hpp"

using namespace pricetime;
using namespace pricetime::itch;
using namespace pricetime::cli;

namespace {

// Log-spaced lifetime buckets. ITCH timestamps are nanoseconds since midnight.
struct LifetimeBucket {
  std::uint64_t upper_ns;
  const char* label;
};

constexpr LifetimeBucket kLifetime[] = {
    {1000ull, "< 1 us"},
    {10000ull, "1-10 us"},
    {100000ull, "10-100 us"},
    {1000000ull, "100 us - 1 ms"},
    {10000000ull, "1-10 ms"},
    {100000000ull, "10-100 ms"},
    {1000000000ull, "100 ms - 1 s"},
    {10000000000ull, "1-10 s"},
    {60000000000ull, "10-60 s"},
    {600000000000ull, "1-10 min"},
    {~0ull, "> 10 min"},
};
constexpr std::size_t kLifetimeBuckets = sizeof(kLifetime) / sizeof(kLifetime[0]);

struct SymbolCounters {
  std::uint64_t adds = 0;
  std::uint64_t removes = 0;   // cancels + deletes + replace-aways
  std::uint64_t executions = 0;
};

// Collects everything the replayer sees without changing what it does.
class StatsObserver {
 public:
  // One order in this many is followed from add to removal.
  static constexpr OrderId kLifetimeSample = 64;

  void on_added(OrderId id, const Symbol& sym, Side, Price, Qty qty, std::uint64_t ts) {
    ++sym_[sym.bits()].adds;
    ++size_hist_[qty];
    total_added_shares_ += qty;
    ++adds_;
    if (id % kLifetimeSample == 0) birth_[id] = ts;
    owner_[id] = sym.bits();
  }

  // A replace is a removal and an add of a new reference. The new order
  // inherits nothing: on a real exchange it goes to the back of the queue,
  // so for lifetime purposes it is genuinely a new order.
  void on_replaced(OrderId old_id, OrderId new_id, Price, Qty qty, std::uint64_t ts) {
    const auto it = owner_.find(old_id);
    const std::uint64_t bits = it == owner_.end() ? 0 : it->second;
    if (bits != 0) {
      ++sym_[bits].adds;
      owner_[new_id] = bits;
    }
    ++size_hist_[qty];
    total_added_shares_ += qty;
    ++adds_;
    ++replaces_;
    if (new_id % kLifetimeSample == 0) birth_[new_id] = ts;
  }

  void on_reduced(OrderId id, Qty qty, bool executed, std::uint64_t) noexcept {
    const auto it = owner_.find(id);
    if (it != owner_.end()) {
      if (executed) {
        ++sym_[it->second].executions;
      }
    }
    if (executed) {
      executed_shares_ += qty;
      ++executions_;
    }
  }

  void on_removed(OrderId id, std::uint64_t ts) {
    const auto it = owner_.find(id);
    if (it != owner_.end()) {
      ++sym_[it->second].removes;
      owner_.erase(it);
    }
    ++removes_;
    const auto b = birth_.find(id);
    if (b == birth_.end()) return;
    // Timestamps are monotone within a session; a wrap would mean a corrupt
    // file, so drop rather than record a negative lifetime.
    if (ts >= b->second) {
      const std::uint64_t life = ts - b->second;
      lifetime_total_ += life;
      ++lifetime_n_;
      lifetimes_.push_back(life);
      for (std::size_t i = 0; i < kLifetimeBuckets; ++i) {
        if (life < kLifetime[i].upper_ns) {
          ++lifetime_hist_[i];
          break;
        }
      }
    }
    birth_.erase(b);
  }

  [[nodiscard]] const std::unordered_map<std::uint64_t, SymbolCounters>& symbols() const {
    return sym_;
  }
  [[nodiscard]] const std::unordered_map<Qty, std::uint64_t>& sizes() const { return size_hist_; }
  [[nodiscard]] const std::uint64_t* lifetime_hist() const { return lifetime_hist_; }
  [[nodiscard]] std::uint64_t lifetime_n() const noexcept { return lifetime_n_; }
  [[nodiscard]] std::uint64_t lifetime_total() const noexcept { return lifetime_total_; }
  [[nodiscard]] std::uint64_t adds() const noexcept { return adds_; }
  [[nodiscard]] std::uint64_t removes() const noexcept { return removes_; }
  [[nodiscard]] std::uint64_t executions() const noexcept { return executions_; }
  [[nodiscard]] std::uint64_t replaces() const noexcept { return replaces_; }
  [[nodiscard]] std::uint64_t total_added_shares() const noexcept { return total_added_shares_; }
  [[nodiscard]] std::uint64_t executed_shares() const noexcept { return executed_shares_; }
  [[nodiscard]] std::vector<std::uint64_t>& lifetimes() noexcept { return lifetimes_; }

 private:
  std::unordered_map<std::uint64_t, SymbolCounters> sym_;
  std::unordered_map<Qty, std::uint64_t> size_hist_;
  std::unordered_map<OrderId, std::uint64_t> birth_;
  // Order reference to symbol bits. Execute/cancel/delete carry no symbol, so
  // per-symbol attribution needs the same routing idea MultiBook uses.
  std::unordered_map<OrderId, std::uint64_t> owner_;
  std::vector<std::uint64_t> lifetimes_;
  std::uint64_t lifetime_hist_[kLifetimeBuckets] = {};
  std::uint64_t lifetime_total_ = 0;
  std::uint64_t lifetime_n_ = 0;
  std::uint64_t adds_ = 0;
  std::uint64_t removes_ = 0;
  std::uint64_t executions_ = 0;
  std::uint64_t replaces_ = 0;
  std::uint64_t total_added_shares_ = 0;
  std::uint64_t executed_shares_ = 0;
};

struct Options {
  const char* path = nullptr;
  std::uint64_t sample_every = 5000000;
  std::string out_prefix;
  std::vector<Symbol> symbols;
  std::size_t top = 15;
};

// Exact percentile from a sorted sample set. No interpolation, no binning:
// the same rule the latency benchmark uses.
std::uint64_t pct(const std::vector<std::uint64_t>& sorted, double q) {
  if (sorted.empty()) return 0;
  std::size_t i = static_cast<std::size_t>(q * static_cast<double>(sorted.size()));
  if (i >= sorted.size()) i = sorted.size() - 1;
  return sorted[i];
}

void write_csv(const std::string& path, const std::string& header,
               const std::vector<std::string>& rows) {
  if (path.empty()) return;
  std::ofstream out(path);
  if (!out) {
    std::fprintf(stderr, "could not write %s\n", path.c_str());
    return;
  }
  out << header << "\n";
  for (const std::string& r : rows) out << r << "\n";
  std::printf("wrote %s (%zu rows)\n", path.c_str(), rows.size());
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    const bool has_next = i + 1 < argc;
    if (std::strcmp(a, "--sample-every") == 0 && has_next) {
      opt.sample_every = std::strtoull(argv[++i], nullptr, 10);
    } else if (std::strcmp(a, "--out") == 0 && has_next) {
      opt.out_prefix = argv[++i];
    } else if (std::strcmp(a, "--symbols") == 0 && has_next) {
      opt.symbols = parse_symbols(argv[++i]);
    } else if (std::strcmp(a, "--top") == 0 && has_next) {
      opt.top = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (a[0] != '-' && opt.path == nullptr) {
      opt.path = a;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a);
      return 2;
    }
  }
  if (opt.path == nullptr) {
    std::fprintf(stderr,
                 "usage: book_stats <file.itch> [--sample-every N] [--out PREFIX] "
                 "[--symbols A,B] [--top N]\n");
    return 2;
  }
  if (opt.sample_every == 0) opt.sample_every = 1;

  using Ladder = PooledMapLadder;
  MultiBook<Ladder> books;
  Replayer<Ladder, OpenAddressIdMap, StatsObserver> rep(books);
  if (!opt.symbols.empty()) rep.track_only(opt.symbols);

  // Level-count samples, one entry per book per sampling tick.
  std::vector<std::uint64_t> level_samples;
  // Slots a dense ladder would need for one side: the span from its best to
  // its worst active price. Level COUNT bounds a tree; only the SPAN bounds an
  // array indexed by price offset, and they are very different numbers when
  // resting interest is scattered.
  std::vector<std::uint64_t> span_samples;
  std::vector<std::uint64_t> touch_orders;   // orders resting at the best level
  std::uint64_t widest_book = 0;
  std::string widest_symbol;
  std::uint64_t widest_span = 0;
  std::string widest_span_symbol;
  std::uint64_t sample_ticks = 0;

  const auto sample_books = [&]() {
    ++sample_ticks;
    books.for_each_book([&](const Symbol& sym, const auto& book) {
      if (book.open_orders() == 0) return;
      std::uint64_t levels = 0;
      for (const Side s : {Side::Bid, Side::Ask}) {
        std::uint64_t side_levels = 0;
        Price best_price = 0;
        Price worst_price = 0;
        book.for_each_level(s, [&](const Level& lvl) {
          if (side_levels == 0) best_price = lvl.price;
          worst_price = lvl.price;
          ++side_levels;
        });
        levels += side_levels;
        if (side_levels > 0) {
          const Price lo = best_price < worst_price ? best_price : worst_price;
          const Price hi = best_price < worst_price ? worst_price : best_price;
          const std::uint64_t span = static_cast<std::uint64_t>(hi - lo) + 1;
          span_samples.push_back(span);
          if (span > widest_span) {
            widest_span = span;
            widest_span_symbol = sym.str();
          }
        }
      }
      level_samples.push_back(levels);
      if (levels > widest_book) {
        widest_book = levels;
        widest_symbol = sym.str();
      }
      const Level* bid = book.best(Side::Bid);
      const Level* ask = book.best(Side::Ask);
      if (bid != nullptr) touch_orders.push_back(bid->order_count);
      if (ask != nullptr) touch_orders.push_back(ask->order_count);
    });
  };

  const auto t0 = std::chrono::steady_clock::now();
  std::uint64_t since_sample = 0;
  const ReadResult r = for_each_framed_stream(opt.path, [&](const std::uint8_t* m, std::size_t len) {
    rep.apply(m, len);
    if (++since_sample >= opt.sample_every) {
      since_sample = 0;
      sample_books();
    }
  });
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  if (!r.ok()) {
    std::fprintf(stderr, "parse failed at offset %" PRIu64 " (type '%c')\n", r.offset, r.bad_type);
    return 1;
  }
  sample_books();  // final state

  StatsObserver& obs = rep.observer();
  const ReplayStats& st = rep.stats();

  std::printf("file            %s\n", opt.path);
  std::printf("messages        %s in %.1f s\n", commas(st.messages).c_str(), secs);
  std::printf("symbols         %s\n", commas(books.symbol_count()).c_str());
  std::printf("level samples   %s books sampled across %s ticks (every %s messages)\n",
              commas(level_samples.size()).c_str(), commas(sample_ticks).c_str(),
              commas(opt.sample_every).c_str());

  // ---- active price levels per book -------------------------------------
  std::sort(level_samples.begin(), level_samples.end());
  std::printf("\nactive price levels per book (both sides, sampled)\n");
  if (!level_samples.empty()) {
    std::uint64_t sum = 0;
    for (std::uint64_t v : level_samples) sum += v;
    std::printf("  mean %.1f   p50 %" PRIu64 "   p90 %" PRIu64 "   p99 %" PRIu64
                "   p99.9 %" PRIu64 "   max %" PRIu64 "\n",
                static_cast<double>(sum) / static_cast<double>(level_samples.size()),
                pct(level_samples, 0.50), pct(level_samples, 0.90), pct(level_samples, 0.99),
                pct(level_samples, 0.999), level_samples.back());
    std::printf("  widest book observed: %s with %s levels\n", widest_symbol.c_str(),
                commas(widest_book).c_str());
  }

  // ---- price span, the number the dense ladder turns on ------------------
  std::sort(span_samples.begin(), span_samples.end());
  if (!span_samples.empty()) {
    std::printf("\ndense-ladder slots needed per side (best to worst active price)\n");
    std::printf("  p50 %s   p90 %s   p99 %s   p99.9 %s   max %s\n",
                commas(pct(span_samples, 0.50)).c_str(), commas(pct(span_samples, 0.90)).c_str(),
                commas(pct(span_samples, 0.99)).c_str(), commas(pct(span_samples, 0.999)).c_str(),
                commas(span_samples.back()).c_str());
    std::printf("  widest span observed: %s at %s slots\n", widest_span_symbol.c_str(),
                commas(widest_span).c_str());
    // 40 bytes per Level is what the array costs per slot, occupied or not.
    const double bytes_per_slot = 40.0;
    std::printf("  at %g bytes per slot that is %.1f MB per side at p50, %.1f GB at p90\n",
                bytes_per_slot,
                static_cast<double>(pct(span_samples, 0.50)) * bytes_per_slot / 1e6,
                static_cast<double>(pct(span_samples, 0.90)) * bytes_per_slot / 1e9);
    // The comparison that matters is span against level count. A tree pays
    // for the levels that exist; an array pays for the distance between the
    // furthest apart of them, and on real data those differ enormously.
    const std::uint64_t median_levels = pct(level_samples, 0.50);
    if (median_levels > 0) {
      std::printf("  the median book holds %s levels inside a span of %s slots: %.0fx\n",
                  commas(median_levels).c_str(),
                  commas(pct(span_samples, 0.50)).c_str(),
                  static_cast<double>(pct(span_samples, 0.50)) /
                      static_cast<double>(median_levels));
    }
  }

  // ---- orders resting at the touch --------------------------------------
  std::sort(touch_orders.begin(), touch_orders.end());
  if (!touch_orders.empty()) {
    std::printf("\norders resting at the best level (queue you join behind)\n");
    std::printf("  p50 %" PRIu64 "   p90 %" PRIu64 "   p99 %" PRIu64 "   max %" PRIu64 "\n",
                pct(touch_orders, 0.50), pct(touch_orders, 0.90), pct(touch_orders, 0.99),
                touch_orders.back());
  }

  // ---- order lifetime ---------------------------------------------------
  std::printf("\norder lifetime, add to removal (1 in %" PRIu64 " sampled, %s observations)\n",
              StatsObserver::kLifetimeSample, commas(obs.lifetime_n()).c_str());
  if (obs.lifetime_n() > 0) {
    const std::uint64_t* h = obs.lifetime_hist();
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < kLifetimeBuckets; ++i) {
      cumulative += h[i];
      const double pctile = 100.0 * static_cast<double>(cumulative) /
                            static_cast<double>(obs.lifetime_n());
      const int bar = static_cast<int>(60.0 * static_cast<double>(h[i]) /
                                       static_cast<double>(obs.lifetime_n()));
      std::printf("  %-14s %12s  %5.1f%%  %s\n", kLifetime[i].label, commas(h[i]).c_str(), pctile,
                  std::string(static_cast<std::size_t>(bar), '#').c_str());
    }
    auto& lf = obs.lifetimes();
    std::sort(lf.begin(), lf.end());
    std::printf("  median %s us   mean %s us\n",
                commas(pct(lf, 0.50) / 1000).c_str(),
                commas(obs.lifetime_total() / obs.lifetime_n() / 1000).c_str());
  }

  // ---- order size -------------------------------------------------------
  std::vector<std::pair<Qty, std::uint64_t>> sizes(obs.sizes().begin(), obs.sizes().end());
  std::sort(sizes.begin(), sizes.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  std::printf("\nmost common order sizes\n");
  std::uint64_t round_lots = 0;
  std::uint64_t all_orders = 0;
  for (const auto& kv : obs.sizes()) {
    all_orders += kv.second;
    if (kv.first % 100 == 0) round_lots += kv.second;
  }
  for (std::size_t i = 0; i < sizes.size() && i < 10; ++i) {
    std::printf("  %8" PRIu32 " shares  %12s  %5.1f%%\n", sizes[i].first,
                commas(sizes[i].second).c_str(),
                100.0 * static_cast<double>(sizes[i].second) / static_cast<double>(all_orders));
  }
  if (all_orders > 0) {
    std::printf("  round lots (multiple of 100): %.1f%% of all orders\n",
                100.0 * static_cast<double>(round_lots) / static_cast<double>(all_orders));
  }

  // ---- cancel-to-trade --------------------------------------------------
  std::printf("\ncancel-to-trade\n");
  std::printf("  adds %s   removals %s   executions %s\n", commas(obs.adds()).c_str(),
              commas(obs.removes()).c_str(), commas(obs.executions()).c_str());
  if (obs.executions() > 0) {
    std::printf("  ratio %.1f removals per execution (feed-wide)\n",
                static_cast<double>(obs.removes()) / static_cast<double>(obs.executions()));
  }
  if (obs.total_added_shares() > 0) {
    std::printf("  %.2f%% of posted shares ever traded\n",
                100.0 * static_cast<double>(obs.executed_shares()) /
                    static_cast<double>(obs.total_added_shares()));
  }

  struct Row {
    Symbol sym;
    SymbolCounters c;
  };
  std::vector<Row> rows;
  rows.reserve(obs.symbols().size());
  for (const auto& kv : obs.symbols()) rows.push_back(Row{Symbol::from_bits(kv.first), kv.second});
  std::sort(rows.begin(), rows.end(),
            [](const Row& a, const Row& b) { return a.c.adds > b.c.adds; });
  std::printf("\ntop %zu symbols by order count\n", opt.top);
  std::printf("  %-8s %14s %14s %12s %10s\n", "symbol", "adds", "removals", "executions",
              "cancel/trade");
  for (std::size_t i = 0; i < rows.size() && i < opt.top; ++i) {
    const double ratio = rows[i].c.executions == 0
                             ? 0.0
                             : static_cast<double>(rows[i].c.removes) /
                                   static_cast<double>(rows[i].c.executions);
    std::printf("  %-8s %14s %14s %12s %10.1f\n", rows[i].sym.str().c_str(),
                commas(rows[i].c.adds).c_str(), commas(rows[i].c.removes).c_str(),
                commas(rows[i].c.executions).c_str(), ratio);
  }

  // ---- machine-readable output -----------------------------------------
  if (!opt.out_prefix.empty()) {
    std::printf("\n");
    std::vector<std::string> lv;
    // Histogram of the level-count samples, capped so the tail does not
    // produce one row per distinct wide book.
    std::unordered_map<std::uint64_t, std::uint64_t> lhist;
    for (std::uint64_t v : level_samples) ++lhist[v > 400 ? 400 : v];
    std::vector<std::pair<std::uint64_t, std::uint64_t>> lrows(lhist.begin(), lhist.end());
    std::sort(lrows.begin(), lrows.end());
    for (const auto& kv : lrows) {
      lv.push_back(std::to_string(kv.first) + "," + std::to_string(kv.second));
    }
    write_csv(opt.out_prefix + "-levels.csv", "levels,book_samples", lv);

    std::vector<std::string> sp;
    std::unordered_map<std::uint64_t, std::uint64_t> shist;
    for (std::uint64_t v : span_samples) {
      // Power-of-two buckets: the tail runs to hundreds of thousands of ticks.
      std::uint64_t bucket = 1;
      while (bucket * 2 <= v) bucket *= 2;
      ++shist[bucket];
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> srows(shist.begin(), shist.end());
    std::sort(srows.begin(), srows.end());
    for (const auto& kv : srows) {
      sp.push_back(std::to_string(kv.first) + "," + std::to_string(kv.second));
    }
    write_csv(opt.out_prefix + "-spans.csv", "slots_at_least,side_samples", sp);

    std::vector<std::string> lt;
    for (std::size_t i = 0; i < kLifetimeBuckets; ++i) {
      lt.push_back(std::string(kLifetime[i].label) + "," +
                   std::to_string(obs.lifetime_hist()[i]));
    }
    write_csv(opt.out_prefix + "-lifetimes.csv", "bucket,orders", lt);

    std::vector<std::string> sz;
    for (std::size_t i = 0; i < sizes.size() && i < 40; ++i) {
      sz.push_back(std::to_string(sizes[i].first) + "," + std::to_string(sizes[i].second));
    }
    write_csv(opt.out_prefix + "-sizes.csv", "shares,orders", sz);

    std::vector<std::string> sy;
    for (std::size_t i = 0; i < rows.size() && i < 200; ++i) {
      sy.push_back(std::string(rows[i].sym.str()) + "," + std::to_string(rows[i].c.adds) + "," +
                   std::to_string(rows[i].c.removes) + "," +
                   std::to_string(rows[i].c.executions));
    }
    write_csv(opt.out_prefix + "-symbols.csv", "symbol,adds,removals,executions", sy);
  }

  return 0;
}
