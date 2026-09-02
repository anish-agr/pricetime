# pricetime design notes

How the book is put together, why each structure was chosen, and what the
measurements showed, including the places where they contradicted the design I
expected to win.

## 1. The problem

A limit order book maintains resting buy and sell orders and matches incoming
orders against them under **price-time priority**: the best price trades first,
and among orders at the same price, the one that arrived first trades first.

Three operations dominate real traffic:

| operation | frequency on a real feed | what it needs |
|---|---|---|
| add | ~40% | find/create a price level, append to its queue |
| cancel | ~50% | find an order **by id**, unlink it |
| execute | ~10% | find the best price level, fill from the queue head |

Cancels outnumber trades by roughly an order of magnitude on modern equity
markets, since most posted liquidity is never hit. That shapes the whole
design: **cancel-by-id is the hot path**, not matching.

## 2. Data structures

```
                    OrderBook<Ladder, IdMap>
                    ┌──────────────────────────────────────────┐
   cancel(id) ─────►│  IdMap:  order id ──────────► Order*     │
                    │                                  │       │
                    │  Ladder: price ──► Level ────────┼──┐    │
                    │                     ▲            │  │    │
                    │  OrderPool: chunked arena of Order nodes │
                    └──────────────────────────────────────────┘
                                          │
   Level (one price):  head ──► Order ◄──► Order ◄──► Order ◄── tail
                                (oldest = next to fill)
```

**Order nodes** live in an arena of fixed-size chunks (`order_pool.hpp`).
Chunks are never moved or freed, so an `Order*` stays valid for the book's
lifetime, which is what lets the id map store raw pointers and lets levels link
nodes intrusively. Freed nodes are chained through their own `next` pointer
into an intrusive free list, so recycling allocates nothing.

**Price levels** hold an intrusive doubly-linked FIFO. Intrusive linkage means
appending and unlinking touch only the node and its neighbours: no container,
no allocation, and cancel is O(1) once you have the pointer. A partial fill
shrinks `qty` in place, which is how a partially filled order correctly keeps
its place in the queue.

**The ladder** maps price to level and tracks the best price. Three policies:

- `DenseLadder`, per side, a `vector<Level>` indexed by `price - min`. Lookup
  is a subtract and an indexed load. Costs: memory proportional to the whole
  configured price range, and when the best level empties, a scan toward worse
  prices to find the next active one.
- `MapLadder`, per side, a `std::map` ordered so `begin()` is the best level.
  O(log L) in active levels, no range bound, a pointer chase per lookup.
- `PooledMapLadder`, the same tree with node recycling through C++17 node
  handles. Added after profiling; see section 4.

**The id map** maps order id to node. Two policies: `std::unordered_map`, and
an open-addressing table with linear probing and backward-shift deletion.

## 3. Why backward-shift deletion

Open addressing needs a story for erase. The usual one is tombstones: mark the
slot deleted so probe chains that ran through it still terminate correctly.
That is simple, and wrong for this workload. An exchange session cancels
millions of orders, and every tombstone permanently lengthens some probe chain
until a full rehash pauses the world.

Backward-shift deletion (Knuth 6.4, Algorithm R) instead repairs the table on
erase. After clearing a slot, scan forward; for each element found, compute its
ideal slot `k`. The element can be pulled back into the hole `i` if `i` lies
cyclically within `[k, j]`, meaning it stays reachable by probing forward from
`k`. Measuring both offsets from `k` reduces that test to one comparison:

```cpp
const std::size_t hole_off = (i - k) & mask_;
const std::size_t cur_off  = (j - k) & mask_;
if (hole_off <= cur_off) break;   // safe to move
```

The table is left exactly as if the erased key had never been inserted. No
tombstones, no degradation, no rehash pauses.

The same structure is reused one layer up. `MultiBook` needs an order-to-book
routing index for the whole feed, and profiling a full day showed
`std::unordered_map` spending about 64 bytes per live order on node and bucket
overhead. `OpenAddressMap<V>` is the id map generalized over its pointer type;
a live order costs 16 bytes at 70% load.

## 4. What the measurements showed

Full numbers and methodology are in the README. Four results, in order of how
much they surprised me.

### The id map dominates; the ladder barely matters

I expected the dense array ladder to win clearly, since it is the folk answer
for matching engines. On this workload and this machine, dense and map land
within noise of each other (175 vs 185 ns p50 on add). Swapping only the id
map, holding everything else constant, moves cancel p50 from 349 ns to 742 ns
and execute p50 from 195 ns to 546 ns.

The reason is that at ~175 ns per operation the cost is dominated by cache
misses on the order nodes and the id-map probe, and the ladder lookup is a
small slice of it. I checked the obvious explanation, that the dense array's
5 MB span was thrashing cache, by adding a tight-range ladder covering only the
traded band so the whole structure fits in L2. It made no difference. The
footprint was not the bottleneck either.

The conclusion: **for this access pattern, ladder choice is not where the time
goes.**

### `unordered_map`'s tail is worse than its median

Node-based hashing costs about 2x at the median, but the tail is where it
shows: p99.9 on passive add is 407 ns for open addressing versus 8761 ns for
`unordered_map`, a 21x difference caused by rehashing pauses that land
unpredictably on individual operations. For a system judged on tail latency,
that is the more damning number.

### The dense ladder has a bad worst case

`bench_book.cpp` includes an adversarial scenario built specifically to defeat
the dense ladder: a book whose only active levels sit at opposite ends of the
price range, with the best one repeatedly emptied so every fill triggers a
full-range rescan. Dense: 81 µs p50. Map: 57 ns p50, a factor of ~1400.

This is not a bug. It is the cost of the design, and a real deployment bounds
it by sizing the range to the instrument. It stays in the published table
because a benchmark that reports only the cases where a design wins is not a
measurement.

### The microbenchmark, the profile, and end-to-end disagree

gprof over a full replay attributes 26% of runtime to `std::map` node churn:
1.47M price-level creations and 722k destructions. The microbenchmark never
showed this, because a steady-state workload keeps levels alive while real
order flow creates a level and empties it again constantly.

The obvious conclusion, that the dense ladder therefore wins replay, is also
wrong and also measured: dense replays at 0.81 to 1.09 M msg/s against the
map's 1.45 to 1.49, because a feed-safe price range costs the dense ladder
about 160 MB per symbol and the resulting page-fault and TLB pressure outweighs
the churn it avoids.

`PooledMapLadder` resolves it. An emptied level's tree node is `extract()`ed,
which detaches it without going through the allocator, parked in a bounded
cache, then re-keyed and spliced back in on the next level creation. Tree
footprint and unbounded price range are kept, level churn allocates nothing
after warmup, and a test with a replaced `operator new` enforces the zero. On
the real full day it is the fastest of the three: 0.45 against 0.40 M msg/s.

Taken together: the array ladder buys nothing measurable here and carries a
1400x worst case. The configuration to ship is the pooled tree ladder with the
open-addressing id map, which is not what I assumed when I started.

## 5. Correctness strategy

Performance claims are worth only as much as the correctness underneath them,
so the test suite is layered deliberately.

**Unit tests** cover the primitives in isolation: FIFO link integrity through
head/middle/tail removal, pool recycling, FNV-1a against published vectors.

**Semantic tests** cover matching rules against hand-computed expectations:
price improvement accrues to the aggressor, partial fills keep queue position,
sweeps cross levels in price order, replace loses time priority, IOC/FOK/market
semantics, and share conservation.

**Invariant checking** walks the entire book mid-stream and asserts the
structural properties simultaneously: sides never crossed, levels sorted
best to worst, every FIFO link consistent in both directions, per-level
aggregates equal to the sum of their members, node count equal to id-map size,
and globally `added = 2·traded + canceled + resting`.

**Differential testing against an independent model.** This one matters most.
The dense-vs-map comparison has a blind spot: both run the *same* matching
loop, so they can only ever disagree about ladder behaviour. A semantic bug in
matching itself would appear identically in both and pass. So
`tests/reference_book.hpp` is a naive O(n) book written straight from the
definition: flat vector, linear scans, nothing shared with the real
implementation. Every policy combination is checked against it op by op,
comparing result codes, the full execution stream, and the state fingerprint
after **every** operation, so a divergence is reported on the op that caused
it.

**Golden replay.** A seeded 100k-op stream must hash to a pinned constant on
every platform, compiler, and policy. The id-map refactor replaced the id map,
rewrote the order pool, and added four order types, and the constant never
moved.

**Deterministic performance tests.** Timing on a shared CI runner is noise, so
CI asserts on allocation behaviour instead: the test binary replaces global
`operator new`/`delete` and counts. A warmed book must run 200k add/cancel
operations with **zero** heap allocations. This caught a real defect: the order
pool's free list was a `std::vector<Order*>`, so the free list itself allocated
while handing out recycled memory. That is now an intrusive chain, and the test
enforces it.

## 6. Deliberate limitations

- **The book itself is single-threaded.** Threading lives in the engine
  (section 8), which keeps the book on one thread by design rather than by
  omission.
- **No in-place modify.** `replace` is ITCH-style cancel-and-reenter, so it
  always loses time priority. An OUCH-style quantity reduction that keeps
  priority is a real order type and is not implemented.
- **Order id 0 is reserved** as the open-addressing empty sentinel, and is
  rejected by the book regardless of which id-map policy is compiled in, so the
  policies stay observationally identical.
- **No self-trade prevention, no auctions, no halts, no odd-lot rules.** Real
  exchanges have all of these.

## 7. Feed reconstruction

Rebuilding a book from ITCH is not the same problem as matching, and
conflating the two is the mistake that makes a replay engine silently wrong.

The exchange has already run its matching engine. Every message describes the
book that *resulted*. So an Add Order message must rest passively even when its
price appears to cross: on a live feed a crossing add generally means the
opposite side was consumed by a message that has not been applied yet, and
re-matching it locally would remove liquidity the real book still had. The book
therefore has a separate entry path, `insert_passive()`, `execute_resting()`,
and `reduce_resting()`, used only by feed replay.

Accounting had to change with it. Internal matching consumes both sides of a
trade, so `added = 2*traded + canceled + resting`. A feed execution consumes
only the resting side, because the aggressor never entered this book at all.
Rather than blur the two, `Counters` tracks `executed_qty` separately from
`traded_qty`, and reconstruction conserves as
`added = executed + canceled + resting`.

Three further details carry real weight:

- **Routing.** Execute, cancel, delete, and replace messages carry only an
  order reference, with no symbol. Reconstruction therefore needs a global
  order-to-book index, which is why `MultiBook` owns one. The alternative,
  searching every symbol's book, is O(symbols) per message on the hottest path
  in the system.
- **Replace has no side.** The message gives an old reference, a new reference,
  a price, and a size. The side must be read off the original order before it
  is destroyed.
- **'P' must not touch the book.** Non-cross trade messages report trades of
  non-displayed liquidity. Applying them double-counts volume that was never in
  the book.

The framing reader advances on the length prefix and uses the spec's length
table only as a cross-check. That ordering means a file containing message
types added to the spec after this code was written still parses instead of
desynchronizing; a disagreement on a *known* type is fatal, because that
indicates genuine misalignment and everything after it would be plausible
nonsense.

`replay_itch` checks two things a correct reconstruction must satisfy: no
crossed book, and share conservation per symbol. The crossed-book check found
its first bug immediately, though not in the replayer: the synthetic generator
was pricing every replace on the bid side regardless of the original order's
side.

### Validation against real data

The pipeline has since been run against NASDAQ's published TotalView-ITCH file
for 2019-12-30: 8.25 GB, 268,744,780 messages, 8,892 symbols. The parse is
clean end to end with zero unknown order references, meaning every execute,
cancel, delete, and replace in the day resolved to a live order. That is strong
evidence the decoder offsets are byte-exact, because one wrong byte produces
garbage references within seconds. No book crossed, and share conservation
holds in all 8,892 books.

The encoder and decoder were written independently from the spec, so their
round-trip agreement was always meaningful evidence, but two independent
implementations can share a misreading of the same document. Real data is what
settles that, and it now has.

The real day also re-ranked the bottlenecks. At roughly 8 GB of live books plus
an 8.25 GB file on a 15.5 GB machine, the run is working-set-bound rather than
parse-bound: the I/O method stops mattering, while the open-addressed routing
index and the pooled ladder move the full-day rate from 0.37 to 0.45 M msg/s.
The symbol-filtered rate of 6.6 to 7.5 M msg/s is the same parser when memory
fits.

## 8. Concurrency

The SPSC queue is the only lock-free code in the repo, and the only component
whose correctness a single-threaded test cannot establish.

Its three load-bearing details are memory ordering (release on publish, acquire
on consume; anything weaker compiles, runs, and tears data on a weakly ordered
machine), cache-line separation of the producer and consumer indices (without
which the two threads ping-pong one line and throughput collapses with no
visible bug), and each side caching the other's index so the common path reads
no shared line at all.

Correctness is argued three ways: a two-thread stress test whose Gauss-sum
checksum detects any loss, duplication, or reordering; a multi-word payload
test where torn publication would appear as mismatched fields; and a
ThreadSanitizer build in CI.

The TSan run was mutation-tested before being relied on. Relaxing every
release/acquire to relaxed makes TSan report a data race at the slot read,
exactly where the reasoning says it must.

### The engine as a server

The engine is four threads joined only by the SPSC queues (recv, match, send,
plus a market-data stage) with the book owned exclusively by the match thread.
No lock ever guards the book because no other thread touches it. This is the
standard exchange architecture in miniature, and the reason the book itself can
stay single-threaded without that being a dead end.

Measurement drove three design changes:

- **Blocking sockets cost ~20 µs of scheduler wakeups per round trip** on
  Windows loopback (34 µs p50 blocking vs 13 µs busy-polling). Busy-polling
  recovers that by burning a core, and on this 2-P-core laptop the tail shows
  what happens when the core budget is not there: spinning threads starve each
  other and p99.9 blows out to ~800 µs while blocking mode holds 72 µs p99.
  Both modes ship as configuration.
- **One syscall per 40-byte message caps everything at the syscall rate.** The
  first throughput run measured 30 k req/s, which was a statement about
  loopback plumbing rather than about the engine. Coalescing queued responses
  into single sends and buffering receives raised it 59x to 1.77 M req/s in and
  2.46 M resp/s out, with ping-pong p50 unchanged, so the latency path paid
  nothing for the throughput fix.
- **Market data shows displayed quantity only.** Emitting the entered quantity
  of a crossing order on its add or replace message is the bug that makes a
  feed-reconstructed book silently diverge from the engine's. The loopback
  tests assert the differential: a book rebuilt purely from the UDP stream
  (real ITCH 5.0 inside MoldUDP64, with sequenced packets, heartbeats and an
  end-of-session marker, produced by the same encoders the tests round-trip)
  must hash identically to the engine's own book, with zero sequence gaps.
  Mold's sequence numbers are the part that matters: UDP loses packets
  silently, and numbering every message turns silent loss into a known gap that
  a re-request channel could fill. `GapTracker` does that arithmetic; the
  re-request channel itself is a documented non-goal.

One flaky test turned out to have a real mechanism behind it. The client
disconnected with requests still in flight, and closing a socket with unread
inbound data sends RST, which discards the undelivered stream and cost the
engine the session's tail. The engine was correct; the test now drains through
a rejected-sentinel flush, the way a real client would quiesce.

## 9. Strategy measurement

The sandbox exists to measure why a naive quoter loses money, not to claim that
one makes money.

P&L is accumulated in integers, cash held as price-ticks times shares, and
converted to dollars only when printed. Across hundreds of thousands of fills a
double loses cents to rounding, and cents are most of the margin in a
market-making strategy. Inventory marks to the mid rather than the last trade,
so a single print on the far side cannot flatter the result, and realized and
unrealized are reported separately, because a strategy that looks profitable
while accumulating inventory is usually just short volatility and has not paid
for it yet.

The measurement that matters is the **markout**: where the mid went 1ms, 10ms,
100ms, and 1s after each fill, signed by the direction of the position taken.
Spread captured minus adverse move is the real edge; reporting capture alone is
how a losing strategy looks busy and profitable.

The first fill model was wrong in a useful way. It filled the strategy when the
*book* crossed its quote, which never happens in a well-formed book, so the
sandbox reported zero fills over three million messages. A passive quote is
filled when a trade *prints* at or through its price, which means reading the
resting order's side and price off an execution message before the replayer
consumes it.

The queue-position model closes the largest remaining gap. When our simulated
order joins a level, every real order already resting there is ahead of us, and
ITCH identifies each one, so their departures (executed, canceled, deleted,
replaced away) are tracked exactly. Only when the recorded set is empty are we
at the front, and only then do prints at our price reach us. On a synthetic
session quoted inside the spread the optimistic model reports 86,153 fills and
the queue model 6. On the real AAPL day the same comparison is 24,145 against
4,391, a factor of 5.5.

Running it on real AAPL flow produced the result the sandbox was built for.
Quoting at the touch all day captures +130 ticks per share of spread and still
loses $1,317 net, because the mid moves 136 ticks against each fill within 1 ms
and 182 ticks by 1 s. A monotone markout curve like that is the signature of
adverse selection by better-informed flow.

One detail came out of the real data that synthetic data could never have
shown. Quoting a fixed half-spread off the mid produces sub-penny prices that
cannot rest in a penny-quoted book. The queue model correctly reported zero
fills all day while the optimistic model reported 43,827 fills at prices no
displayed order can occupy, so quotes now snap to the instrument's tick grid.

One approximation remains, stated in the tool's own output: once we are at the
front, the historical aggressor that fills us also still fills the real order
it actually hit, so liquidity at our level is double-counted by our
participation. Fixing that requires counterfactual replay, letting the book
diverge from history the moment we participate, which changes the question from
"what happened around our quotes" to "what would have happened".

## 10. Open questions

- The dense ladder's range bound is affordable per symbol only if the active
  level count stays small. The full-day data can answer what that count
  actually is per symbol, which would say whether a windowed dense ladder is
  worth building.
- How much of the ~175 ns per operation is cache miss versus work? The next
  measurement should be a cache-miss count, not another latency histogram.
- The full-day replay is working-set-bound. Whether a compact per-symbol book
  representation moves that number is untested.
- Validation covers one venue and one day. A second day, or a second venue with
  a different protocol, would test whether anything here is overfit to
  2019-12-30.
