# pricetime

**A NASDAQ-grade limit order book, ITCH 5.0 replay engine, matching server, and
strategy sandbox in C++20 — where every performance claim carries its
methodology, and the worst cases are published next to the wins.**

[![ci](https://github.com/anish-agr/pricetime/actions/workflows/ci.yml/badge.svg)](https://github.com/anish-agr/pricetime/actions/workflows/ci.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![license](https://img.shields.io/badge/license-MIT-green)
![deps](https://img.shields.io/badge/core%20dependencies-zero-orange)

```
                     ┌─────────────────────────────────────────────────────┐
   NASDAQ ITCH 5.0   │                     pricetime                       │
   full-day file ────┼─► mmap ─► parser ─► MultiBook ─► strategy sandbox   │
   (real, 3.4 GB)    │              │      (per-symbol   (exact queue-      │
                     │              │       books)        position fills)   │
                     │   ┌──────────┴──────────────────────────┐           │
   order entry ──────┼─► │  matching engine (4 threads,        │ ─► ITCH   │
   (TCP, 40-byte     │   │  SPSC queues, lock-free handoff,    │    inside │
    binary protocol) │   │  book owned by ONE thread)          │  MoldUDP64│
                     │   └─────────────────────────────────────┘  over UDP │
                     └─────────────────────────────────────────────────────┘
        core: OrderBook<Ladder, IdMap> — both policies swappable, both measured
```

## Headline numbers

1M samples per scenario, exact percentiles from full sorted sample sets,
`LFENCE`-serialized TSC timestamps, timer overhead (13 ns) included rather
than subtracted:

| path | p50 | p99 | p99.9 | throughput |
|---|---|---|---|---|
| book: add (steady state, ~100k resting orders) | 182 ns | 322 ns | 518 ns | 3.7 M ops/s |
| book: cancel (steady state) | 292 ns | 527 ns | 1.08 µs | 3.7 M ops/s |
| book: execute (aggressive add, one fill) | 231 ns | 419 ns | 637 ns | 3.6 M ops/s |
| ITCH replay: full-day book build, **real NASDAQ data**, all 8,892 symbols | — | — | — | 0.45 M msg/s‡ |
| ITCH replay: parse + route, symbol-filtered (same real day) | — | — | — | 6.6–7.5 M msg/s |
| engine, wire-to-wire (TCP→match→TCP, loopback, busy-poll) | 13 µs | 51 µs | ~800 µs† | — |
| engine, pipelined order flow | — | — | — | 1.77 M req/s in / 2.46 M resp/s out |

Machine: Intel Core Ultra 5 225U laptop (2 P + 8 E cores), Windows 11, MSVC
`/O2`, pinned P-core, AC power. A laptop is not a tuned server: treat the
relative comparisons as the result and the absolute numbers as a floor.
† The wire-to-wire tail is core starvation — busy-polling needs a core budget
this chip does not have; blocking sockets trade the median (34 µs p50) for a
far tighter tail (72 µs p99). Both modes ship; the trade is measured.
‡ Working-set-bound, not parse-bound: ~8 GB of live books plus the 8.25 GB
file streaming through 15.5 GB of RAM. The filtered row is the same parser
when memory fits; the gap between the rows is itself a documented finding.

## Validated against a real NASDAQ trading day

Not a synthetic claim: the pipeline replays NASDAQ's published TotalView-ITCH
file for **December 30, 2019** — 8.25 GB, **268,744,780 messages**, 8,892
symbols — end to end.

- Parse status clean, **zero unknown order references** across the whole day:
  one wrong byte offset in any decoder would have produced garbage references
  within seconds, so a full day of exact routing is strong evidence the
  decoders are byte-exact.
- **No crossed books and share conservation holds in every one of the 8,892
  books** (`added = executed + canceled + resting`, checked per symbol).
- The intraday profile comes out of the replay itself: 3.7 M messages in the
  16:00 closing-auction minute, 19.9× the day's mean.
- And the market-maker sandbox runs against the real AAPL flow — see finding
  6, which is the reason the sandbox exists.

## Six findings

The point of this repo is not that an order book exists — it is what happened
when every folk-wisdom design choice was measured instead of assumed.

**1. The id map dominates; the price ladder barely matters.** The dense-array
ladder is the folk answer for matching engines. Measured with everything else
held constant, ladder choice moves medians a few percent — while swapping
`std::unordered_map` for an open-addressing id map (linear probing,
backward-shift deletion, no tombstones) moves cancel p50 by 2.1×, execute by
2.8×, and the p99.9 add tail by **21×** (rehash pauses landing on individual
operations). Cancels outnumber trades ~10:1 on real feeds; the id map *is*
the hot path.

**2. The dense ladder's worst case is 1400×, and it is in the published
table.** Empty the best level when the next active level is far away and the
array rescans the gap: 81 µs vs the tree's 57 ns, in an adversarial scenario
built to prove it. A benchmark that hides its own worst case is marketing.

**3. The microbenchmark, the profile, and end-to-end disagree — and all three
are right.** Microbench: ladders tie. gprof over full replay: the tree ladder
pays **26% of runtime in level churn** (1.47M creates, 722k destroys) that
steady-state microbenchmarks structurally cannot see. End-to-end: the tree
*still* wins replay, because a feed-safe dense ladder costs ~160 MB per
symbol and the TLB pressure outweighs the churn. The loop then closes: a
third ladder policy recycles its tree nodes through C++17 node handles
(extract/re-key/splice — zero allocator traffic after warmup, enforced by
test), and is **the fastest of the three on the real full day**, 0.45 vs
0.40 M msg/s. Profile → hypothesis → fix → measured win.
([docs/PROFILE.md](docs/PROFILE.md))

**4. Busy-polling halves the median and can destroy the tail.** Blocking
sockets pay ~20 µs of scheduler wakeups per round trip; spinning recovers it
(34 → 13 µs p50) and then blows up p99.9 by 40× when four hot threads fight
over two P-cores. Separately, one syscall per 40-byte message capped
throughput at 30 k/s — batching (coalesced sends, buffered receives) raised
it **59×** while leaving ping-pong p50 untouched. The latency/throughput
lever every gateway has, resolved so the latency path pays nothing.

**5. Ignoring queue position inflates a passive backtest — 5.5× on real
data.** The sandbox's exact model puts our simulated order behind every
identified resting order (ITCH names them all) and fills it only when the
feed shows them all gone. On the real AAPL day, the optimistic
any-print-fills-us model reports 24,145 fills; the queue model reports
4,391. On a synthetic day quoted inside the spread the same gap is four
orders of magnitude (86,153 vs 6). A related lesson came free: quoting off
the instrument's real tick grid produced sub-penny quotes that can never
rest in AAPL's book — the queue model correctly returned **zero fills all
day** while the optimistic model happily "filled" 43,827 times at prices
that cannot exist. Tick alignment is not a detail.

**6. The naive market maker's loss, decomposed on real flow.** Quoting AAPL
at the touch all day: 4,391 fills, **+130 ticks/share of spread captured —
and a net loss of $1,317**, because the mid moves −136 ticks against each
fill within 1 ms, deepening monotonically to −182 by 1 s. That monotone
markout curve is the signature of adverse selection by informed flow, and
measuring it — rather than the fill count — is what separates a market-making
backtest from a fill counter.

## What is inside

- **The book** — price-time priority matching (limit / IOC / FOK / market),
  O(1) cancel by id, intrusive per-level FIFO, chunked arena with an intrusive
  free list, and both structural policies (`Ladder`, `IdMap`) swappable at
  compile time so they could be raced instead of debated.
- **ITCH 5.0** — big-endian decoders, framed BinaryFILE reader, an
  independently written encoder (round-trip agreement is evidence about the
  spec, not two copies of one misreading), multi-symbol reconstruction with a
  global order→book index. Reconstruction is **not** matching: the exchange
  already matched, so feed adds rest passively and conservation becomes
  `added = executed + canceled + resting` — one side per trade, not two.
- **The engine** — recv → match → send + market-data threads joined only by
  wait-free SPSC queues; the book belongs to one thread and no lock ever
  guards it. Compact 40-byte binary order protocol with a magic tail (desync
  fails loudly). Market data leaves as **real ITCH 5.0 inside MoldUDP64** —
  sequenced UDP, heartbeats, end-of-session — and a book rebuilt purely from
  that stream must hash identically to the engine's own. TSC stamps echo
  through every response, so wire-to-wire latency needs no clock sync.
- **The sandbox** — inventory-skewed quoter, integer tick accounting (a
  double loses cents across 100k fills, and cents are the margin), mid-based
  marking, markout curves at 1 ms–1 s horizons for adverse selection, and the
  exact queue-position fill model above.

## How it is tested

148 test cases / 7.3M assertions, all run on GCC 15 and MSVC with warnings as
errors, plus ASan/UBSan and TSan jobs in CI.

- **Differential testing against an independent model.** A deliberately naive
  O(n) reference book — flat vector, linear scans, nothing shared with the
  real implementation — must agree with every policy combination op-for-op:
  result codes, full execution stream, and state fingerprint after *every*
  operation. Fast-vs-fast comparison shares the matching loop and therefore
  its bugs; fast-vs-naive does not.
- **Golden replay.** A seeded 100k-op stream hashes to a pinned constant on
  every platform, compiler, and policy. It has already earned its keep: the
  id map was replaced, the pool rewritten, four order types added — the
  constant never moved.
- **Deterministic performance tests.** CI timing is noise, so CI asserts
  allocations: global `operator new` is replaced and counted, and a warmed
  book must run 200k mixed ops with **zero** heap calls. This caught the
  free list itself allocating.
- **Mutation-tested concurrency.** The SPSC queue passes TSan; relaxing its
  release/acquire pairing makes TSan fail at exactly the predicted line. A
  sanitizer that has never gone red for the right reason proves nothing.
- **Feed integrity.** Engine loopback tests unwrap the MoldUDP64 stream,
  assert zero sequence gaps, and require the stream-rebuilt book to hash
  identically to the engine's.
- Plus structural invariants walked mid-stream (never crossed, FIFO link
  consistency, share conservation) and an end-to-end CI pipeline: generate →
  replay → strategy → engine session over real sockets.

## Quick start

CMake ≥ 3.21 and any C++20 compiler (MSVC 2022 / GCC 13+ / Clang 16+); the
core has zero external dependencies.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure

./build/bench/pricetime_bench --ops 1000000        # book latency histograms
./build/tools/gen_itch day.itch --messages 3000000 # synthetic ITCH day
./build/tools/replay_itch day.itch                 # rebuild books + sanity checks
./build/tools/mm_sandbox day.itch --symbol AAPL    # strategy vs. the replay
./build/tools/engine_server --spin &               # the matching engine
./build/tools/engine_client --spin                 # wire-to-wire latency
```

Real data: NASDAQ publishes full-day ITCH 5.0 samples at
`emi.nasdaq.com/ITCH/Nasdaq ITCH/` (several GB). After `gunzip`:

```bash
./build/tools/replay_itch 12302019.NASDAQ_ITCH50 --minute-profile   # the full day
./build/tools/mm_sandbox 12302019.NASDAQ_ITCH50 --symbol AAPL --half-spread 100
```

## Honest limitations

- Validation is one venue, one day (NASDAQ, 2019-12-30). More days are a
  download away; other venues are other protocols.
- The engine serves one client session, one symbol: the threading
  architecture was the milestone; multi-tenancy is plumbing.
- The Mold feed has sequencing and gap *detection* but no re-request channel.
- The sandbox's queue model still double-counts liquidity at our own level
  (the historical aggressor also fills the order it really hit); fixing that
  is counterfactual replay, a different question.
- No self-trade prevention, auctions, halts, or odd lots.

Design rationale and the full measurement post-mortems:
[ARCHITECTURE.md](ARCHITECTURE.md) · [docs/PROFILE.md](docs/PROFILE.md)

## License

MIT
