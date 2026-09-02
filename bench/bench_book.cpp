// Latency benchmark: per-op latency histograms for the order book, run
// head-to-head on both ladder policies.
//
// Methodology (also in the README):
//  - thread pinned to one core; invariant-TSC timestamps around each op
//  - TSC calibrated against steady_clock; timer overhead measured and printed,
//    NOT subtracted from the reported numbers
//  - percentiles are exact (full sorted sample sets, no binning)
//  - inputs are generated outside the timed window (splitmix64)
//  - throughput is wall-clock and therefore INCLUDES sampling overhead
//
// Scenarios:
//  - add (passive):        non-crossing adds into a deepening book
//  - cancel (random):      cancel every resting order in shuffled order
//  - mixed steady state:   alternating add/cancel around 100k resting orders
//  - execute (1 fill):     aggressive add exactly consuming the head order
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "affinity.hpp"
#include "histogram.hpp"
#include "pricetime/book.hpp"
#include "pricetime/id_map.hpp"
#include "pricetime/ladder_dense.hpp"
#include "pricetime/ladder_map.hpp"
#include "timing.hpp"

namespace pb = pricetime::bench;
using namespace pricetime;

namespace {

constexpr Price kMid = 100000;
constexpr Price kDenseMin = 0;
constexpr Price kDenseMax = (1 << 17) - 1;
constexpr std::uint64_t kPrefillMixed = 100000;

std::uint64_t splitmix64(std::uint64_t& state) {
  state += 0x9E3779B97F4A7C15ull;
  std::uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

// Bids 10..500 ticks below mid, asks 10..500 above: the sides never cross.
Price passive_price(Side side, std::uint64_t& s) {
  const Price off = static_cast<Price>(10 + splitmix64(s) % 491);
  return side == Side::Bid ? kMid - off : kMid + off;
}

Qty gen_qty(std::uint64_t& s) { return static_cast<Qty>(1 + splitmix64(s) % 100); }

constexpr int kAutoCore = -1000;  // sentinel: pick a performance core at runtime

struct Config {
  std::uint64_t ops = 500000;
  int core = kAutoCore;
  bool run_dense = true;
  bool run_map = true;
  bool run_stdmap_id = true;  // also bench the std::unordered_map id policy
  bool run_adversarial = true;
  bool run_tight = true;  // dense ladder sized to the instrument, not the universe
};

// The wide ladder spans 2^17 ticks: enough for any instrument, and 5 MB per
// side. The tight ladder covers only the band the bench actually trades, so
// the whole ladder is a few tens of KB and stays cache-resident. Comparing
// the two isolates the dense array's memory footprint from its O(1) lookup.
constexpr Price kTightMin = 99000;
constexpr Price kTightMax = 101000;

struct Row {
  std::string ladder;
  std::string op;
  pb::LatencyStats st;
  double mops = 0;
};

double wall_seconds(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

template <class MakeBook>
std::uint64_t run_ladder(const char* ladder_name, const Config& cfg,
                         std::vector<Row>& rows, MakeBook make) {
  std::uint64_t prng = 0x0DDBA11'5EEDull;
  std::uint64_t sink = 0;

  // --- add (passive) + cancel (random), one book ---
  {
    auto book = make();
    book.reserve_orders(cfg.ops);
    std::vector<OrderId> ids;
    ids.reserve(cfg.ops);
    pb::SampleSet add_s(cfg.ops);
    OrderId next_id = 1;
    auto w0 = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < cfg.ops; ++i) {
      const Side side = (i & 1) == 0 ? Side::Bid : Side::Ask;
      const Price price = passive_price(side, prng);
      const Qty qty = gen_qty(prng);
      const OrderId id = next_id++;
      const std::uint64_t t0 = pb::now_ticks();
      book.add_limit(id, side, price, qty);
      const std::uint64_t t1 = pb::now_ticks();
      add_s.add(t1 - t0);
      ids.push_back(id);
    }
    const double add_secs = wall_seconds(w0);
    sink ^= book.state_hash();
    rows.push_back({ladder_name, "add (passive)", add_s.stats(),
                    static_cast<double>(cfg.ops) / add_secs / 1e6});

    // Fisher-Yates shuffle so cancels hit the book in arrival-independent order.
    for (std::size_t i = ids.size(); i > 1; --i) {
      const std::size_t j = static_cast<std::size_t>(splitmix64(prng) % i);
      std::swap(ids[i - 1], ids[j]);
    }
    pb::SampleSet cancel_s(ids.size());
    w0 = std::chrono::steady_clock::now();
    for (const OrderId id : ids) {
      const std::uint64_t t0 = pb::now_ticks();
      book.cancel(id);
      const std::uint64_t t1 = pb::now_ticks();
      cancel_s.add(t1 - t0);
    }
    const double cancel_secs = wall_seconds(w0);
    if (book.open_orders() != 0) std::fprintf(stderr, "BUG: cancel phase left orders\n");
    rows.push_back({ladder_name, "cancel (random order)", cancel_s.stats(),
                    static_cast<double>(ids.size()) / cancel_secs / 1e6});
  }

  // --- mixed steady state around kPrefillMixed resting orders ---
  {
    auto book = make();
    book.reserve_orders(kPrefillMixed + cfg.ops / 2 + 1);
    std::vector<OrderId> live;
    live.reserve(kPrefillMixed + cfg.ops / 2 + 1);
    OrderId next_id = 1;
    for (std::uint64_t i = 0; i < kPrefillMixed; ++i) {
      const Side side = (i & 1) == 0 ? Side::Bid : Side::Ask;
      book.add_limit(next_id, side, passive_price(side, prng), gen_qty(prng));
      live.push_back(next_id++);
    }
    pb::SampleSet mixed_add(cfg.ops / 2 + 1);
    pb::SampleSet mixed_cancel(cfg.ops / 2 + 1);
    const auto w0 = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < cfg.ops; ++i) {
      if ((i & 1) == 0) {
        const Side side = splitmix64(prng) % 2 == 0 ? Side::Bid : Side::Ask;
        const Price price = passive_price(side, prng);
        const Qty qty = gen_qty(prng);
        const OrderId id = next_id++;
        const std::uint64_t t0 = pb::now_ticks();
        book.add_limit(id, side, price, qty);
        const std::uint64_t t1 = pb::now_ticks();
        mixed_add.add(t1 - t0);
        live.push_back(id);
      } else {
        const std::size_t j = static_cast<std::size_t>(splitmix64(prng) % live.size());
        const OrderId id = live[j];
        live[j] = live.back();
        live.pop_back();
        const std::uint64_t t0 = pb::now_ticks();
        book.cancel(id);
        const std::uint64_t t1 = pb::now_ticks();
        mixed_cancel.add(t1 - t0);
      }
    }
    const double secs = wall_seconds(w0);
    sink ^= book.state_hash();
    const double mops = static_cast<double>(cfg.ops) / secs / 1e6;
    rows.push_back({ladder_name, "mixed add", mixed_add.stats(), mops});
    rows.push_back({ladder_name, "mixed cancel", mixed_cancel.stats(), mops});
  }

  // --- execute: aggressive add that exactly consumes the resting head ---
  {
    auto book = make();
    book.reserve_orders(cfg.ops + 1);
    OrderId next_id = 1;
    for (std::uint64_t i = 0; i < cfg.ops; ++i) {
      book.add_limit(next_id++, Side::Ask, passive_price(Side::Ask, prng), gen_qty(prng));
    }
    pb::SampleSet exec_s(cfg.ops);
    const auto w0 = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < cfg.ops; ++i) {
      const Level* best_ask = book.best(Side::Ask);  // peek is untimed
      const Price price = best_ask->price;
      const Qty qty = best_ask->head->qty;
      const OrderId id = next_id++;
      const std::uint64_t t0 = pb::now_ticks();
      book.add_limit(id, Side::Bid, price, qty);
      const std::uint64_t t1 = pb::now_ticks();
      exec_s.add(t1 - t0);
    }
    const double secs = wall_seconds(w0);
    if (book.open_orders() != 0) std::fprintf(stderr, "BUG: exec phase left orders\n");
    sink ^= book.counters().traded_qty;
    rows.push_back({ladder_name, "execute (1 fill)", exec_s.stats(),
                    static_cast<double>(cfg.ops) / secs / 1e6});
  }

  return sink;
}

// The dense ladder's worst case, measured rather than hidden.
//
// The array ladder finds the next-best price by scanning toward worse prices
// when the best level empties. Normal markets cluster orders near the touch,
// so that scan is a step or two. This scenario builds the opposite: a book
// whose only two active levels sit at opposite ends of the configured price
// range, and repeatedly empties the best one. Every fill then walks the whole
// range. The tree ladder does not care — it is O(log L) regardless.
//
// A real deployment bounds this by sizing the price range to the instrument
// rather than to the representable universe, but the pathology is real and
// belongs in the numbers.
template <class MakeBook>
void run_adversarial(const char* ladder_name, const Config& cfg, std::vector<Row>& rows,
                     MakeBook make) {
  auto book = make();
  const std::uint64_t reps = cfg.ops / 10 + 1;  // deliberately expensive; do fewer
  book.reserve_orders(reps * 2 + 16);
  OrderId next_id = 1;

  // A deep anchor at the far end of the range keeps the side alive so the
  // rescan has to traverse the entire span to reach it.
  const Price far_ask = kDenseMax - 1;
  const Price near_ask = kDenseMin + 1;
  book.add_limit(next_id++, Side::Ask, far_ask, 1000000);

  pb::SampleSet s(reps);
  const auto w0 = std::chrono::steady_clock::now();
  for (std::uint64_t i = 0; i < reps; ++i) {
    // Post a lone order at the near end: it becomes the new best ask.
    book.add_limit(next_id++, Side::Ask, near_ask, 10);
    // Consuming it empties that level, forcing a full-range rescan for the
    // next best. Only the consuming op is timed.
    const OrderId aggressor = next_id++;
    const std::uint64_t t0 = pb::now_ticks();
    book.add_limit(aggressor, Side::Bid, near_ask, 10);
    const std::uint64_t t1 = pb::now_ticks();
    s.add(t1 - t0);
  }
  const double secs = wall_seconds(w0);
  rows.push_back({ladder_name, "execute (worst-case rescan)", s.stats(),
                  static_cast<double>(reps) / secs / 1e6});
}

void print_rows(const std::vector<Row>& rows, double tpn) {
  std::printf("\n| ladder | operation             | samples |  p50 ns |  p90 ns |  p99 ns "
              "| p99.9 ns |  max ns | mean ns | Mops/s |\n");
  std::printf("|--------|-----------------------|---------|---------|---------|---------"
              "|----------|---------|---------|--------|\n");
  for (const Row& r : rows) {
    std::printf("| %-6s | %-21s | %7zu | %7.1f | %7.1f | %7.1f | %8.1f | %7.0f | %7.1f "
                "| %6.2f |\n",
                r.ladder.c_str(), r.op.c_str(), r.st.count, r.st.p50 / tpn, r.st.p90 / tpn,
                r.st.p99 / tpn, r.st.p999 / tpn, static_cast<double>(r.st.max) / tpn,
                r.st.mean / tpn, r.mops);
  }
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--ops") == 0 && i + 1 < argc) {
      cfg.ops = static_cast<std::uint64_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (std::strcmp(argv[i], "--core") == 0 && i + 1 < argc) {
      cfg.core = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--ladder") == 0 && i + 1 < argc) {
      const char* v = argv[++i];
      cfg.run_dense = std::strcmp(v, "dense") == 0 || std::strcmp(v, "both") == 0;
      cfg.run_map = std::strcmp(v, "map") == 0 || std::strcmp(v, "both") == 0;
    } else if (std::strcmp(argv[i], "--no-adversarial") == 0) {
      cfg.run_adversarial = false;
    } else if (std::strcmp(argv[i], "--no-idmap-compare") == 0) {
      cfg.run_stdmap_id = false;
    }
  }
  if (cfg.ops == 0) {
    std::fprintf(stderr, "--ops must be > 0\n");
    return 1;
  }

  int core = cfg.core;
  if (core == kAutoCore) {
    core = pb::pick_performance_core();
    if (core < 0) core = 1;
  }
  const bool pinned = pb::pin_current_thread(core);
  const bool prioritized = pb::raise_priority();
  const double tpn = pb::calibrate_ticks_per_ns();

  constexpr int kOverheadSamples = 100001;
  pb::SampleSet overhead(kOverheadSamples);
  for (int i = 0; i < kOverheadSamples; ++i) {
    const std::uint64_t t0 = pb::now_ticks();
    const std::uint64_t t1 = pb::now_ticks();
    overhead.add(t1 - t0);
  }
  const pb::LatencyStats ost = overhead.stats();

  std::printf("cpu: %s\n", pb::cpu_brand().c_str());
  std::printf("pinned to core %d%s: %s\n", core,
              cfg.core == kAutoCore ? " (auto: best EfficiencyClass)" : "",
              pinned ? "yes" : "NO (unpinned)");
  std::printf("high priority: %s\n", prioritized ? "yes" : "no");
  std::printf("tsc calibration: %.3f ticks/ns\n", tpn);
  std::printf("timer overhead (back-to-back rdtsc, included in every sample): p50 %.1f ns, "
              "p99 %.1f ns\n",
              ost.p50 / tpn, ost.p99 / tpn);
  std::printf("ops per scenario: %" PRIu64 "\n", cfg.ops);

  std::vector<Row> rows;
  std::uint64_t sink_dense = 0;
  std::uint64_t sink_map = 0;
  if (cfg.run_dense) {
    sink_dense = run_ladder("dense", cfg, rows, [] {
      return OrderBook<DenseLadder, OpenAddressIdMap>{kDenseMin, kDenseMax};
    });
  }
  if (cfg.run_map) {
    sink_map = run_ladder("map", cfg, rows,
                          [] { return OrderBook<MapLadder, OpenAddressIdMap>{}; });
  }
  // Same ladder, different id map: isolates the id-map change.
  if (cfg.run_stdmap_id) {
    run_ladder("d+std", cfg, rows,
               [] { return OrderBook<DenseLadder, StdIdMap>{kDenseMin, kDenseMax}; });
  }
  // Same code, same id map, only the ladder's price range differs.
  if (cfg.run_tight) {
    run_ladder("tight", cfg, rows, [] {
      return OrderBook<DenseLadder, OpenAddressIdMap>{kTightMin, kTightMax};
    });
  }
  if (cfg.run_adversarial) {
    if (cfg.run_dense) {
      run_adversarial("dense", cfg, rows, [] {
        return OrderBook<DenseLadder, OpenAddressIdMap>{kDenseMin, kDenseMax};
      });
    }
    if (cfg.run_map) {
      run_adversarial("map", cfg, rows,
                      [] { return OrderBook<MapLadder, OpenAddressIdMap>{}; });
    }
  }
  print_rows(rows, tpn);
  std::printf("\nstate fingerprints (optimizer sink): dense %016" PRIx64 ", map %016" PRIx64
              "%s\n",
              sink_dense, sink_map,
              cfg.run_dense && cfg.run_map
                  ? (sink_dense == sink_map ? " — ladders agree on the bench stream"
                                            : " — LADDERS DISAGREE (bug!)")
                  : "");
  return cfg.run_dense && cfg.run_map && sink_dense != sink_map ? 1 : 0;
}
