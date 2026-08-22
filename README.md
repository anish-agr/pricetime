# pricetime

A price-time-priority limit order book, NASDAQ ITCH replay engine, and
strategy sandbox in C++20 — with latency numbers measured honestly enough to
defend in an interview, and the worst cases published alongside the good ones.

## Headline numbers

Tree ladder + open-addressing id map, 1M samples per scenario, exact
percentiles, `LFENCE`-serialized timestamps:

| operation | p50 | p99 | p99.9 | throughput |
|---|---|---|---|---|
| add (passive, book deepening to 1M orders) | 185 ns | 366 ns | 1.06 µs | 4.03 M/s |
| add (steady state, ~100k resting orders) | 182 ns | 322 ns | 518 ns | 3.66 M/s* |
| cancel (steady state) | 292 ns | 527 ns | 1.08 µs | 3.66 M/s* |
| execute (aggressive add, one full fill) | 231 ns | 419 ns | 637 ns | 3.61 M/s |
| **ITCH replay** (parse + full book reconstruction) | — | — | — | **1.4 M msg/s** |

\* add and cancel alternate in one loop; the throughput figure is the combined loop's.

**Methodology, in full:** Intel Core Ultra 5 225U, on AC power, Windows 11,
MSVC 2022 `/O2`, pinned to a P-core (auto-selected via `EfficiencyClass`),
high process priority. Every sample is bracketed by `LFENCE`-serialized
`RDTSC` reads, calibrated against `steady_clock`. **Timer overhead (13.0 ns
p50) is included in every number, not subtracted.** Percentiles are exact —
full sorted sample sets, no histogram binning. This is a laptop, not a tuned
server: treat the relative comparisons as the result and the absolute numbers
as an upper bound. The replay figure is against a *synthetic* ITCH file (see
caveat below).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release --parallel
./build/bench/pricetime_bench --ops 1000000
```

## Why this exists

Every toy order book on GitHub stops at "it matches orders." This one is built
around three harder claims:

1. **Determinism is testable.** The same input stream must produce a
   bit-identical book, and the fast implementation must agree op-for-op with
   an independently written naive model.
2. **A benchmark that only reports where your design wins is marketing.** The
   1400× worst case of the data structure I expected to win is in the table
   below.
3. **A backtest whose assumptions are invisible is worse than none.** The
   strategy sandbox prints its fill-model caveats with every run.

Full design rationale and measurement post-mortem: [ARCHITECTURE.md](ARCHITECTURE.md).

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

Ladder choice moves the median a few percent. Id-map choice moves cancel by
2.1×, execute by 2.8×, and the p99.9 tail on add by **21×** — that last one is
`unordered_map` rehashing landing unpredictably on individual operations.

At ~175 ns/op the time goes to cache misses on order nodes and the id-map
probe; the ladder lookup is a small slice. I tested the obvious explanation —
that the dense array's 5 MB span was thrashing cache — by adding a tight-range
ladder sized to the traded band so it fits in L2. **It made no difference.**
The footprint was not the bottleneck either.

**Finding 2: the dense ladder has a 1400× pathology.** The array finds the
next best price by scanning toward worse prices when the best level empties. A
scenario built to defeat that — two active levels at opposite ends of the
range, best one repeatedly emptied:

| ladder | execute p50 (worst case) |
|---|---|
| dense array | 81.4 µs |
| tree map | 57 ns |

Not a bug — the documented cost of the layout, bounded in practice by sizing
the range to the instrument. It stays in the table because a benchmark that
hides its own worst case is not a benchmark.

**Conclusion:** the array ladder buys nothing measurable on this workload and
carries a 1400× worst case. The configuration I would ship today is the tree
ladder with the open-addressing id map — not what I assumed when I started.

## Components

**The book** — price-time matching (limit/IOC/FOK/market), O(1) cancel by id,
intrusive per-level FIFO, chunked arena with an intrusive free list,
open-addressing id map with **backward-shift deletion** (Knuth Algorithm R)
rather than tombstones, which would degrade every probe chain over a session
that cancels millions of orders.

**ITCH 5.0** — big-endian decoders, a framed BinaryFILE reader, an independent
encoder, and multi-symbol reconstruction. The central semantic point:
**reconstruction is not matching.** The exchange already matched, so feed adds
rest passively via `insert_passive()`; routing them through `add_limit()` would
re-match orders that never crossed and diverge on the first busy symbol.
Executions consume one side only, so conservation is
`added = executed + canceled + resting`.

**SPSC queue** — wait-free, cache-line padded, release/acquire published; the
M3 seam between network and matching threads.

**Strategy sandbox** — inventory-skewed quoter with integer P&L accounting and
markout-based adverse-selection measurement.

## Tools

```bash
./build/tools/gen_itch day.itch --messages 3000000      # synthetic ITCH file
./build/tools/replay_itch day.itch --depth 5            # rebuild books, check sanity
./build/tools/mm_sandbox day.itch --symbol AAPL         # run the strategy
```

`replay_itch` exits non-zero on a parse failure or any crossed book, so it
doubles as an assertion. `gen_itch` exists so the whole pipeline is testable
without the multi-gigabyte NASDAQ download — **but it does not reproduce the
statistical character of real order flow** (arrival clustering, price
distributions, the true cancel/trade ratio, intraday volume profile). Numbers
taken against it measure the parser and the book, never the market.

## Testing

134 test cases, 7.3M assertions, run against **both** ladder policies and
**both** id-map policies, on GCC 15 and MSVC 2022 with warnings as errors.

- **Differential testing against an independent model.**
  `tests/reference_book.hpp` is a naive O(n) book — flat vector, linear scans,
  nothing shared with the real implementation. Every policy combination is
  checked against it op by op: same result codes, same execution stream, same
  state fingerprint after *every* operation. This exists because dense-vs-map
  comparison has a blind spot — both run the same matching loop, so a semantic
  bug would appear in both and pass.
- **Structural invariants** walked mid-stream: never crossed, levels sorted,
  FIFO links consistent both ways, aggregates equal to member sums, share
  conservation.
- **Golden replay:** a seeded 100k-op stream must hash to a pinned constant on
  every platform, compiler, and policy. It earned its keep — the M1.5 refactor
  swapped the id map, rewrote the pool, and added four order types, and the
  constant never moved.
- **Deterministic performance tests.** CI timing is noise, so CI asserts on
  allocations instead: the test binary replaces global `operator new` and
  requires a warmed book to run 200k add/cancel ops with **zero** heap
  allocations. This caught a real defect — the pool's free list was a
  `std::vector<Order*>`, so the free list allocated while handing out recycled
  memory.
- **Mutation-tested concurrency.** The SPSC queue passes under
  ThreadSanitizer; relaxing its release/acquire pairing makes TSan report a
  race at exactly the predicted line. A green sanitizer run means nothing
  until you have watched it go red for the right reason.
- CI: Windows + Ubuntu × Debug + Release, an ASan/UBSan job, a TSan job, and
  an end-to-end pipeline job (generate → replay → strategy).

## Roadmap

- **M1 — the book** *(done)*
- **M1.5 — measured optimization:** open-addressing id map, intrusive free
  list, allocation guards, reference-model differential testing *(done)*
- **M2 — real data:** ITCH 5.0 parser, multi-symbol reconstruction,
  memory-mapped replay *(done — pending validation against a real NASDAQ file)*
- **M3 — the engine as a server:** SPSC queue *(done)*; TCP order gateway, UDP
  multicast feed, and wire-to-wire latency still to build
- **M4 — the strategy sandbox:** quoter, P&L, markouts *(done)*; queue-position
  modelling is the honest next step

## Known limitations

- The ITCH pipeline has **not yet been validated against a real NASDAQ file** —
  only against synthetic data produced by this repo's own encoder. Round-trip
  agreement between an independently written encoder and decoder is good
  evidence, but it is not the same as parsing a real capture.
- No in-place modify (OUCH-style quantity reduction keeping priority), no
  self-trade prevention, no auctions or halts.
- The sandbox's fill model ignores queue position, which is the single biggest
  determinant of a passive strategy's real P&L.
- `Order` is ~48 bytes unpacked; ~32 is reachable with tighter types.

## License

MIT
