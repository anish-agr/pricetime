# pricetime

A price-time-priority limit order book and matching engine in C++20, built to be
replayed against real full-day NASDAQ TotalView-ITCH data — with latency numbers
measured honestly enough to defend in an interview.

## Headline numbers

*Numbers land with the M1 benchmark run. Placeholder until then.*

| operation | p50 | p99 | p99.9 | throughput |
|---|---|---|---|---|
| add (passive) | — | — | — | — |
| cancel | — | — | — | — |
| add (aggressive, 1 fill) | — | — | — | — |
| mixed add/cancel steady state | — | — | — | — |

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
- Order nodes live in a stable-address arena with LIFO free-list recycling.
- Single-threaded on purpose. Concurrency arrives in M3 as an explicit design
  step (lock-free SPSC queues between network and matching threads), not as a
  premature optimization.

## Testing

- Property tests (doctest): book never crossed or locked, share conservation
  across adds/fills/cancels, cancel-of-unknown rejected, FIFO priority under
  partial fills, replay determinism via state hash.
- Golden replay fixtures: a seeded random operation stream must hash to a
  pinned constant on every platform and compiler.

## Building

Requires CMake ≥ 3.21 and a C++20 compiler (MSVC 2022, GCC 13+, Clang 16+).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The benchmark binary is `build/bench/pricetime_bench` (pass `--core N` to pick
the pinned core, `--core -1` to disable pinning).

## Roadmap

- **M1 — the book:** price-time-priority limit order book (add / cancel /
  replace / execute), property tests, first latency histograms. *(current)*
- **M2 — real data:** NASDAQ TotalView-ITCH 5.0 parser (binary, big-endian,
  memory-mapped), full-day book rebuild for chosen symbols, throughput numbers
  and a flame graph.
- **M3 — the engine as a server:** TCP order gateway, UDP multicast market-data
  feed, lock-free SPSC queues, wire-to-wire latency.
- **M4 — the strategy sandbox:** naive market maker vs. the replayed day; PnL,
  inventory, and adverse-selection analysis.

## License

MIT
