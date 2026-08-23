# pricetime — design notes

How the book is put together, why each structure was chosen, and what the
measurements actually showed — including where they contradicted the design
I expected to win.

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
markets — most posted liquidity is never hit. That shapes the whole design:
**cancel-by-id is the hot path**, not matching.

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
lifetime — which is what lets the id map store raw pointers and lets levels
link nodes intrusively. Freed nodes are chained through their own `next`
pointer into an intrusive free list, so recycling allocates nothing.

**Price levels** hold an intrusive doubly-linked FIFO. Intrusive linkage means
appending and unlinking touch only the node and its neighbours — no container,
no allocation, and cancel is O(1) once you have the pointer. A partial fill
shrinks `qty` in place, which is how a partially filled order correctly keeps
its place in the queue.

**The ladder** maps price → level and tracks the best price. Two policies:

- `DenseLadder` — per side, a `vector<Level>` indexed by `price - min`. Lookup
  is a subtract and an indexed load. Costs: memory proportional to the whole
  configured price range, and when the best level empties, a scan toward worse
  prices to find the next active one.
- `MapLadder` — per side, a `std::map` ordered so `begin()` is the best level.
  O(log L) in active levels, no range bound, a pointer chase per lookup.

**The id map** maps order id → node. Two policies: `std::unordered_map`, and an
open-addressing table with linear probing and backward-shift deletion.

## 3. Why backward-shift deletion

Open addressing needs a story for erase. The usual one is tombstones: mark the
slot deleted so probe chains that ran through it still terminate correctly.
That is simple, and wrong for this workload — an exchange session cancels
millions of orders, and every tombstone permanently lengthens some probe chain
until a full rehash pauses the world.

Backward-shift deletion (Knuth 6.4, Algorithm R) instead repairs the table on
erase. After clearing a slot, scan forward; for each element found, compute
its ideal slot `k`. The element can be pulled back into the hole `i` if `i`
lies cyclically within `[k, j]` — i.e. it stays reachable by probing forward
from `k`. Measuring both offsets from `k` reduces that test to one comparison:

```cpp
const std::size_t hole_off = (i - k) & mask_;
const std::size_t cur_off  = (j - k) & mask_;
if (hole_off <= cur_off) break;   // safe to move
```

The table is left exactly as if the erased key had never been inserted. No
tombstones, no degradation, no rehash pauses.

## 4. What the measurements actually showed

Full numbers and methodology are in the README. Three findings, in order of
how much they surprised me.

### The id map dominates; the ladder barely matters

I expected the dense array ladder to win clearly — it is the folk-wisdom answer
for matching engines. On this workload and this machine, dense and map land
within noise of each other (175 vs 185 ns p50 on add). Swapping only the id
map, holding everything else constant, moves cancel p50 from 349 ns to 742 ns
and execute p50 from 195 ns to 546 ns.

The reason is that at ~175 ns per operation, the cost is dominated by cache
misses on the order nodes and the id-map probe — the ladder lookup is a small
slice of it. I checked the obvious explanation (that the dense array's 5 MB
span was thrashing cache) by adding a tight-range ladder covering only the
traded band, so the whole structure fits in L2. It made no difference. The
footprint was not the bottleneck either.

The honest conclusion: **for this access pattern, ladder choice is not where
the time goes.** That may change with real ITCH data (M2), where the level
count and access distribution are set by the market rather than by my
generator, and it would change at much higher order rates.

### `unordered_map`'s tail is worse than its median

Node-based hashing costs about 2x at the median, but the tail is where it
really shows: p99.9 on passive add is 407 ns for open addressing versus
8761 ns for `unordered_map` — a 21x difference, caused by rehashing pauses
that land unpredictably on individual operations. For a system judged on tail
latency, that is the more damning number.

### The dense ladder has a pathology worth publishing

`bench_book.cpp` includes an adversarial scenario built specifically to defeat
the dense ladder: a book whose only active levels sit at opposite ends of the
price range, with the best one repeatedly emptied so every fill triggers a
full-range rescan. Dense: 81 µs p50. Map: 57 ns p50. That is a factor of
~1400.

This is not a bug — it is the documented cost of the design, and a real
deployment bounds it by sizing the range to the instrument. But a benchmark
that only reports the cases where your design wins is marketing, not
measurement, so it stays in the table.

Taken together: the array ladder buys nothing measurable here and carries a
1400x worst case. If I had to ship one configuration today it would be the
tree ladder with the open-addressing id map — which is *not* what I assumed
when I started.

## 5. Correctness strategy

Performance claims are only worth as much as the correctness underneath them,
so the test suite is layered deliberately.

**Unit tests** cover the primitives in isolation: FIFO link integrity through
head/middle/tail removal, pool recycling, FNV-1a against published vectors.

**Semantic tests** cover matching rules against hand-computed expectations:
price improvement accrues to the aggressor, partial fills keep queue position,
sweeps cross levels in price order, replace loses time priority, IOC/FOK/market
semantics, and share conservation.

**Invariant checking** walks the entire book mid-stream and asserts the
structural properties simultaneously: sides never crossed, levels sorted
best→worst, every FIFO link consistent in both directions, per-level
aggregates equal to the sum of their members, node count equal to id-map size,
and globally `added = 2·traded + canceled + resting`.

**Differential testing against an independent model.** This one matters most.
The dense-vs-map comparison has a blind spot: both run the *same* matching
loop, so they can only ever disagree about ladder behaviour. A semantic bug in
matching itself would appear identically in both and pass. So
`tests/reference_book.hpp` is a naive O(n) book written straight from the
definition — flat vector, linear scans, nothing shared with the real
implementation. Every policy combination is then checked against it op by op,
comparing result codes, the full execution stream, and the state fingerprint
after **every** operation, so a divergence is reported on the op that caused
it.

**Golden replay.** A seeded 100k-op stream must hash to a pinned constant on
every platform, compiler, and policy. This earns its keep: the M1.5 refactor
replaced the id map, rewrote the order pool, and added four order types, and
the constant never moved — positive evidence that none of it changed matching
semantics.

**Deterministic performance tests.** Timing on a shared CI runner is noise, so
CI asserts on allocation behaviour instead: the test binary replaces global
`operator new`/`delete` and counts. A warmed book must run 200k add/cancel
operations with **zero** heap allocations. This is not decoration — it caught a
real defect. The order pool's free list was a `std::vector<Order*>`, so the
free list itself allocated while handing out recycled memory. That is now an
intrusive chain, and the test enforces it.

## 6. Deliberate limitations

- **The book is single-threaded.** The SPSC queue exists (section 8) but the
  matching engine is not yet driven across threads.
- **No in-place modify.** `replace` is ITCH-style cancel-and-reenter, so it
  always loses time priority. An OUCH-style quantity reduction that keeps
  priority is a real order type and is not implemented.
- **Order id 0 is reserved** as the open-addressing empty sentinel, and is
  rejected by the book regardless of which id-map policy is compiled in, so
  the policies stay observationally identical.
- **No self-trade prevention, no auctions, no halts, no odd-lot rules.** Real
  exchanges have all of these.

## 7. Feed reconstruction (M2)

Rebuilding a book from ITCH is not the same problem as matching, and
conflating the two is the mistake that makes a replay engine silently wrong.

The exchange has already run its matching engine. Every message describes the
book that *resulted*. So an Add Order message must rest passively even when
its price appears to cross: on a live feed a crossing add generally means the
opposite side was consumed by a message that has not been applied yet, and
re-matching it locally would remove liquidity the real book still had. The
book therefore grew a separate entry path — `insert_passive()`,
`execute_resting()`, `reduce_resting()` — used only by feed replay.

Accounting had to change with it. Internal matching consumes both sides of a
trade, so `added = 2*traded + canceled + resting`. A feed execution consumes
only the resting side, because the aggressor never entered this book at all.
Rather than blur the two, `Counters` tracks `executed_qty` separately from
`traded_qty`, and reconstruction conserves as
`added = executed + canceled + resting`.

Three further details carry real weight:

- **Routing.** Execute, cancel, delete, and replace messages carry only an
  order reference — no symbol. Reconstruction therefore needs a global
  order-to-book index, which is why `MultiBook` owns one. The alternative,
  searching every symbol's book, is O(symbols) per message on the hottest path
  in the system.
- **Replace has no side.** The message gives an old reference, a new
  reference, a price, and a size. The side must be read off the original order
  before it is destroyed.
- **'P' must not touch the book.** Non-cross trade messages report trades of
  non-displayed liquidity. Applying them double-counts volume that was never
  in the book.

The framing reader advances on the length prefix and uses the spec's length
table only as a cross-check. That ordering means a file containing message
types added to the spec after this code was written still parses instead of
desynchronizing; a disagreement on a *known* type is fatal, because that
indicates genuine misalignment and everything after it would be plausible
nonsense.

`replay_itch` checks two things a correct reconstruction must satisfy: no
crossed book, and share conservation per symbol. The crossed-book check
immediately earned its place by failing — not on the replayer, but on the
synthetic generator, which was pricing every replace on the bid side
regardless of the original order's side.

**The honest gap:** none of this has been run against a real NASDAQ capture
yet. The encoder and decoder were written independently from the published
spec, so their agreement is meaningful evidence — but two independent
implementations can still share a misreading of the same document, and only
real data settles it.

## 8. Concurrency (M3)

The SPSC queue is the only lock-free code in the repo, and the only component
whose correctness a single-threaded test genuinely cannot establish.

Its three load-bearing details are memory ordering (release on publish,
acquire on consume — anything weaker compiles, runs, and tears data on a
weakly ordered machine), cache-line separation of the producer and consumer
indices (without which the two threads ping-pong one line and throughput
collapses with no visible bug), and each side caching the other's index so the
common path reads no shared line at all.

Correctness is argued three ways: a two-thread stress test whose Gauss-sum
checksum detects any loss, duplication, or reordering; a multi-word payload
test where torn publication would appear as mismatched fields; and a
ThreadSanitizer build in CI.

The third of those is only worth anything because it was mutation-tested.
Relaxing every release/acquire to relaxed makes TSan report a data race at the
slot read — exactly where the reasoning says it must. A sanitizer that has
never been observed failing for the right reason is not evidence.

### The engine as a server

The full M3 shape is four threads joined only by the SPSC queues — recv →
match → send, plus a market-data stage — with the book owned exclusively by
the match thread. No lock ever guards the book because no other thread
touches it; this is the standard exchange architecture in miniature, and the
reason M1 could stay single-threaded without that being a dead end.

Measurement drove three design changes worth recording:

- **Blocking sockets cost ~20 µs of scheduler wakeups per round trip** on
  Windows loopback (34 µs p50 blocking vs 13 µs busy-polling). Busy-polling
  recovers it by burning a core — and on this 2-P-core laptop, the tail shows
  what happens when the core budget is not there: spinning threads starve
  each other and p99.9 blows out to ~800 µs while blocking mode holds 72 µs
  p99. Both modes ship as configuration; the trade is measured, not asserted.
- **One syscall per 40-byte message caps everything at the syscall rate.**
  The first throughput run measured 30 k req/s — a statement about loopback
  plumbing, not the engine. Coalescing queued responses into single sends and
  buffering receives raised it 59× to 1.77 M req/s in / 2.46 M resp/s out,
  with ping-pong p50 unchanged: batch-of-one costs the latency path nothing.
- **Market data shows displayed quantity only.** Emitting the entered
  quantity of a crossing order on its add/replace message is the bug that
  makes a feed-reconstructed book silently diverge from the engine's. The
  loopback tests assert the differential: a book rebuilt purely from the UDP
  datagrams (which are real ITCH 5.0, produced by the same encoder the tests
  round-trip) must hash identically to the engine's own book.

One flaky test earned its keep by having a real mechanism: the client
disconnected with requests still in flight, and closing a socket with unread
inbound data sends RST — which discards the undelivered stream, costing the
engine the session's tail. The engine was correct; the test now drains
through a rejected-sentinel flush, the way a real client would quiesce.

## 9. Strategy measurement (M4)

The sandbox exists to measure *why* a naive quoter loses money, not to claim
one makes money.

P&L is accumulated in integers — cash in price-ticks times shares — and
converted to dollars only when printed. Across hundreds of thousands of fills
a double loses cents to rounding, and cents are the entire margin of a
market-making strategy. Inventory marks to the mid rather than the last trade,
so a single print on the far side cannot flatter the result, and realized and
unrealized are reported separately, because a strategy that looks profitable
while accumulating inventory is usually just short volatility and has not paid
for it yet.

The measurement that actually matters is the **markout**: where the mid went
1ms, 10ms, 100ms, and 1s after each fill, signed by the direction of the
position taken. Spread captured minus adverse move is the real edge. Reporting
capture alone is precisely how a losing strategy looks busy and profitable.

The first fill model was wrong in an instructive way: it filled the strategy
when the *book* crossed its quote, which never happens in a well-formed book,
so the sandbox reported zero fills over three million messages. A passive
quote is filled when a trade *prints* at or through its price — which means
reading the resting order's side and price off an execution message before the
replayer consumes it.

The queue-position model closes the largest remaining gap. When our
simulated order joins a level, every real order already resting there is
ahead of us — and ITCH identifies each one, so their departures (executed,
canceled, deleted, replaced away) are tracked exactly. Only when the recorded
set is empty are we at the front, and only then do prints at our price reach
us. On the same synthetic session the optimistic model reports 86,153 fills
and the queue model reports 6: four orders of magnitude of backtest inflation
from one silent assumption, now measured instead of made.

One approximation remains, stated in the tool's own output: once we are at
the front, the historical aggressor that fills us also still fills the real
order it actually hit, so liquidity at our level is double-counted by our
participation. Fixing that requires counterfactual replay — letting the book
diverge from history the moment we participate — which changes the question
the sandbox answers from "what happened around our quotes" to "what would
have happened", a genuinely different (and harder) epistemic claim.

## 10. Open questions

- Does the ladder comparison change under real ITCH order flow, where level
  counts and price clustering are set by the market rather than by a uniform
  generator? This is the first thing to check once a real capture is in hand.
- What is the actual active-level count per symbol on a full NASDAQ day, and
  does that make the dense ladder's range bound affordable per symbol?
- How much of the ~175 ns per operation is cache miss versus work? The next
  measurement should be a cache-miss count, not another latency histogram.
