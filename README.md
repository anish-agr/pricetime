# pricetime

A price-time-priority limit order book and matching engine in C++20, built to be
replayed against real full-day NASDAQ TotalView-ITCH data — with latency numbers
measured honestly enough to defend in an interview.

## Headline numbers (M1)

Dense-array ladder, 500k samples per scenario, exact percentiles:

| operation | p50 | p99 | p99.9 | throughput |
|---|---|---|---|---|
| add (passive, book deepening to 500k orders) | 373 ns | 976 ns | 12.0 µs | 2.14 M/s |
| add (steady state, ~100k resting orders) | 537 ns | 981 ns | 1.24 µs | 1.07 M/s* |
| cancel (steady state) | 1.24 µs | 1.82 µs | 2.25 µs | 1.07 M/s* |
| execute (aggressive add, one full fill) | 1.03 µs | 1.91 µs | 5.84 µs | 0.86 M/s |

\* add and cancel alternate in one loop; the throughput figure is the combined loop's.

**Environment, disclosed:** Intel Core Ultra 5 225U (laptop, **on battery**,
Balanced power plan), Windows 11, MSVC 2022 `/O2`, pinned to a P-core
(auto-selected via `EfficiencyClass`), high process priority. Timer overhead
(~14 ns p50, measured) is included in every sample, not subtracted. Numbers
will be re-baselined on wall power; the value of M1 is the methodology and the
relative comparisons, which are stable run-to-run.

## Why this exists

Every toy order book on GitHub stops at "it matches orders." This one is built
around two harder claims:

1. **Determinism is testable.** The same input stream must produce a
   bit-identical book, fingerprinted by a platform-independent state hash.
   Property tests enforce it before any benchmark number is trusted.
2. **Latency numbers are only as good as their methodology.** Percentiles come
   from full sorted sample sets (no histogram binning error), timed with the
   invariant TSC, calibrated against `steady_clock`, on a pinned core, with the
   timer's own overhead measured and reported.

## Design (M1)

- **Zero external dependencies in the core** — matching and book code is
  header-only C++20, standard library only.
- Hash map from order id → order node: O(1) cancel and replace lookup.
- Intrusive doubly-linked FIFO per price level: queue operations never
  allocate; a partially filled order keeps its time priority.
- Order nodes live in a chunked arena with stable addresses and LIFO free-list
  recycling. (Not `std::deque`: MSVC's deque uses tiny blocks — one heap
  allocation per Order-sized element — which silently puts `operator new` on
  the hot path.)
- **The price ladder is a compile-time policy**, and both interesting answers
  are implemented and benchmarked against each other:
  - `DenseLadder` — per side, a `vector<Level>` indexed by `(price − min)`.
    O(1) lookup, contiguous memory; costs a bounded price range and a scan
    toward worse prices when the best level empties.
  - `MapLadder` — per side, a red-black tree keyed by price. O(log L) in
    active levels, unbounded range, pointer-chasing on every touch.
- Single-threaded on purpose. Concurrency arrives in M3 as an explicit design
  step (lock-free SPSC queues between network and matching threads), not as a
  premature optimization.

### Ladder head-to-head (same seeded workload, same run)

| ladder | operation | p50 ns | p99 ns | p99.9 ns |
|---|---|---|---|---|
| dense | add (passive) | **373** | **976** | 12004 |
| map | add (passive) | 518 | 1237 | 30888 |
| dense | mixed add | **537** | **981** | **1245** |
| map | mixed add | 676 | 1158 | 1547 |
| dense | mixed cancel | **1242** | **1824** | **2253** |
| map | mixed cancel | 1343 | 1932 | 2273 |
| dense | execute (1 fill) | 1034 | 1906 | 5840 |
| map | execute (1 fill) | **973** | **1669** | **5341** |

What the numbers say: the dense array wins wherever levels persist — adds and
steady-state traffic — because a level lookup is one subtract and one indexed
load instead of a tree walk. The map pulls even on the execute workload, where
almost every fill empties a level: dense pays its best-pointer rescan across
empty slots exactly there, the tree pays a cheap `erase` + `begin`. Both books
process identical op streams and end with identical state fingerprints, every
run — the benchmark doubles as a differential test.

Known next bottleneck (M1.5): both ladders spend much of the cancel path in
`std::unordered_map` (bucket-chain walk plus a node allocation per insert);
replacing it with an open-addressing id map is the next measured change.

## Testing

- Property tests (doctest), run against **both** ladder policies: book never
  crossed or locked, share conservation (`added = 2·traded + canceled +
  resting`) maintained through every mixed random stream, cancel-of-unknown
  rejected, FIFO priority under partial fills, replace loses time priority.
- **Differential testing:** dense and map books consume the same seeded stream
  and must produce identical state hashes at every checkpoint.
- **Golden replay:** a seeded 100k-op stream must hash to a pinned constant on
  every platform, compiler, and ladder — matching semantics cannot drift
  silently.

## Building

Requires CMake ≥ 3.21 and a C++20 compiler (MSVC 2022, GCC 13+, Clang 16+).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The benchmark is `build/bench/pricetime_bench`. It picks a performance core
automatically; `--core N` overrides, `--core -1` disables pinning, `--ops N`
sets the per-scenario sample count, `--ladder dense|map|both` selects the
ladder.

## Roadmap

- **M1 — the book:** price-time-priority limit order book (add / cancel /
  replace / execute), property tests, first latency histograms. *(done)*
- **M2 — real data:** NASDAQ TotalView-ITCH 5.0 parser (binary, big-endian,
  memory-mapped), full-day book rebuild for chosen symbols, throughput numbers
  and a flame graph.
- **M3 — the engine as a server:** TCP order gateway, UDP multicast market-data
  feed, lock-free SPSC queues, wire-to-wire latency.
- **M4 — the strategy sandbox:** naive market maker vs. the replayed day; PnL,
  inventory, and adverse-selection analysis.

## License

MIT
