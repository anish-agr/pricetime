# pricetime

A price-time-priority limit order book and matching engine in C++20, built to be
replayed against real full-day NASDAQ TotalView-ITCH data — with latency numbers
measured honestly enough to defend in an interview.

## Headline numbers

Tree ladder + open-addressing id map, 1M samples per scenario, exact percentiles,
serialized timestamps:

| operation | p50 | p99 | p99.9 | throughput |
|---|---|---|---|---|
| add (passive, book deepening to 1M orders) | 185 ns | 366 ns | 1.06 µs | 4.03 M/s |
| add (steady state, ~100k resting orders) | 182 ns | 322 ns | 518 ns | 3.66 M/s* |
| cancel (steady state) | 292 ns | 527 ns | 1.08 µs | 3.66 M/s* |
| execute (aggressive add, one full fill) | 231 ns | 419 ns | 637 ns | 3.61 M/s |

\* add and cancel alternate in one loop; the throughput figure is the combined loop's.

**Methodology, in full:** Intel Core Ultra 5 225U, on AC power, Windows 11,
MSVC 2022 `/O2`, pinned to a P-core (auto-selected via `EfficiencyClass`),
high process priority. Every sample is bracketed by `LFENCE`-serialized `RDTSC`
reads, calibrated against `steady_clock`. **Timer overhead (13.0 ns p50,
13.7 ns p99) is included in every number, not subtracted.** Percentiles are
exact — full sorted sample sets, no histogram binning. This is a laptop, not a
tuned server: treat the relative comparisons as the result and the absolute
numbers as an upper bound.

Reproduce with:

```bash
./build/bench/pricetime_bench --ops 1000000
```

## Why this exists

Every toy order book on GitHub stops at "it matches orders." This one is built
around two harder claims:

1. **Determinism is testable.** The same input stream must produce a
   bit-identical book, fingerprinted by a platform-independent state hash, and
   the fast implementation must agree op-for-op with an independently written
   naive model.
2. **Latency numbers are only as good as their methodology** — and a benchmark
   that only reports the cases where your design wins is marketing, not
   measurement. The worst case of the design I expected to win is in the table
   below.

Full design rationale and the measurement post-mortem are in
[ARCHITECTURE.md](ARCHITECTURE.md).

## What the measurements showed (including where I was wrong)

The book is parameterized on two compile-time policies — the **price ladder**
and the **id map** — so both choices could be measured rather than assumed.

**Finding 1: the id map dominates; the ladder barely matters.** I expected the
dense array ladder to win clearly; it is the folk-wisdom answer for matching
engines. Holding everything else constant and swapping only the id map:

| configuration | add p50 | cancel p50 | execute p50 | add p99.9 |
|---|---|---|---|---|
| dense ladder + open addressing | 176 ns | 349 ns | 195 ns | 407 ns |
| dense ladder + `std::unordered_map` | 210 ns | **742 ns** | **546 ns** | **8761 ns** |
| tree ladder + open addressing | 185 ns | 348 ns | 231 ns | 1065 ns |

Ladder choice moves the median by a few percent. Id-map choice moves cancel by
2.1x, execute by 2.8x, and the p99.9 tail on add by **21x** — that last one is
`unordered_map`'s rehashing landing unpredictably on individual operations.

At ~175 ns/op the time goes to cache misses on order nodes and the id-map
probe; the ladder lookup is a small slice of it. I tested the obvious
explanation — that the dense array's 5 MB span was thrashing cache — by adding
a tight-range ladder sized to the traded band so it fits in L2. It made no
difference. The footprint was not the bottleneck either.

**Finding 2: the dense ladder has a 1400x pathology.** The array finds the next
best price by scanning toward worse prices when the best level empties. A
scenario built to defeat that — only two active levels, at opposite ends of the
range, best one repeatedly emptied:

| ladder | execute p50 (worst case) | vs. normal workload |
|---|---|---|
| dense array | 81.4 µs | 420x slower |
| tree map | 57 ns | unchanged |

Not a bug — the documented cost of the layout, bounded in practice by sizing
the range to the instrument. It stays in the table because a benchmark that
hides its own worst case is not a benchmark.

**Conclusion:** the array ladder buys nothing measurable on this workload and
carries a 1400x worst case. The configuration I would ship today is the tree
ladder with the open-addressing id map — which is not what I assumed when I
started. Whether that survives real ITCH order flow is M2's job.

## Design

- **Zero external dependencies in the core** — matching and book code is
  header-only C++20, standard library only.
- Order id → node map: O(1) cancel and replace. Cancels outnumber trades ~10:1
  on real equity feeds, so this is the true hot path, not matching.
- Intrusive doubly-linked FIFO per price level: queue operations never
  allocate, and a partially filled order keeps its time priority.
- Order nodes live in a chunked arena with stable addresses and an **intrusive**
  free list. (Not `std::deque` — MSVC's deque allocates once per Order-sized
  element. Not a `vector` free list either; see the allocation test below.)
- **Open-addressing id map** with linear probing and **backward-shift deletion**
  (Knuth Algorithm R) rather than tombstones — an exchange session cancels
  millions of orders, and tombstones would degrade every probe chain until a
  rehash pauses the world.
- Order types: limit (GTC), IOC, FOK with a liquidity pre-scan, and market
  orders, all sharing one matching path via a compile-time `HasLimit` switch.
- Single-threaded on purpose. Concurrency is M3's explicit design step.

## Testing

87 test cases, 7.3M assertions, every suite run against **both** ladder
policies and **both** id-map policies.

- **Differential testing against an independent model.** `tests/reference_book.hpp`
  is a naive O(n) book — flat vector, linear scans, nothing shared with the real
  implementation. Every policy combination is checked against it op by op:
  same result codes, same execution stream, same state fingerprint after
  *every* operation. This exists because dense-vs-map comparison has a blind
  spot — both run the same matching loop, so a semantic bug would appear in
  both and pass.
- **Structural invariants** walked mid-stream: never crossed, levels sorted,
  FIFO links consistent both ways, aggregates equal to member sums, and
  `added = 2·traded + canceled + resting`.
- **Golden replay:** a seeded 100k-op stream must hash to a pinned constant on
  every platform, compiler, and policy. It earned its keep — the M1.5 refactor
  swapped the id map, rewrote the pool, and added four order types, and the
  constant never moved.
- **Deterministic performance tests.** CI timing is noise, so CI asserts on
  allocations instead: the test binary replaces global `operator new` and
  requires a warmed book to run 200k add/cancel ops with **zero** heap
  allocations. This caught a real defect — the pool's free list was a
  `std::vector<Order*>`, so the free list allocated while handing out recycled
  memory. It is an intrusive chain now.
- CI runs Windows + Ubuntu × Debug + Release with warnings-as-errors, plus an
  ASan/UBSan job.

## Building

Requires CMake ≥ 3.21 and a C++20 compiler (MSVC 2022, GCC 13+, Clang 16+).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Benchmark flags: `--ops N` per-scenario samples, `--core N` pin target
(`-1` disables; default auto-selects a performance core), `--ladder
dense|map|both`, `--no-adversarial`, `--no-idmap-compare`.

## Roadmap

- **M1 — the book:** price-time-priority book (add / cancel / replace /
  execute), IOC/FOK/market, property tests, latency histograms. *(done)*
- **M1.5 — measured optimization:** open-addressing id map, intrusive free
  list, allocation guards, reference-model differential testing. *(done)*
- **M2 — real data:** NASDAQ TotalView-ITCH 5.0 parser (binary, big-endian,
  memory-mapped), full-day book rebuild, throughput and a flame graph. The
  `Op` type in `include/pricetime/op.hpp` is the seam: the parser becomes just
  another producer, and tests, replay, and benchmarks consume it unchanged.
- **M3 — the engine as a server:** TCP order gateway, UDP multicast market-data
  feed, lock-free SPSC queues, wire-to-wire latency.
- **M4 — the strategy sandbox:** naive market maker vs. the replayed day; PnL,
  inventory, and adverse-selection analysis.

## License

MIT
