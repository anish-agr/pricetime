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

## 6. Deliberate limitations (M1)

- **Single-threaded.** Concurrency is M3's explicit design problem, not a
  premature optimization here.
- **Single-symbol.** A real feed is multi-symbol; M2 will need a symbol →
  book map, and the per-symbol memory cost is exactly what makes the dense
  ladder's range bound a live question.
- **No in-place modify.** `replace` is ITCH-style cancel-and-reenter, so it
  always loses time priority. An OUCH-style quantity reduction that keeps
  priority is a real order type and is not implemented.
- **Order id 0 is reserved** as the open-addressing empty sentinel, and is
  rejected by the book regardless of which id-map policy is compiled in, so
  the policies stay observationally identical.
- **No self-trade prevention, no auctions, no halts, no odd-lot rules.** Real
  exchanges have all of these.

## 7. Open questions for M2

- Does the ladder comparison change under real ITCH order-flow, where level
  counts and price clustering are set by the market rather than by a uniform
  generator?
- What is the actual active-level count per symbol on a full NASDAQ day, and
  does that make the dense ladder's range bound affordable per symbol?
- How much of the ~175 ns per operation is cache miss versus work? The next
  measurement should be a cache-miss count, not another latency histogram.
