# Profiling the replay path

The brief called for a flame graph of full-day replay. This environment (WSL2
without root) has no `perf`, so the profile below is from `gprof`
(`-O2 -g -pg`), which answers the same question — where does replay time go —
with call counts that a flame graph would not give. Reproduction commands for
both are at the bottom.

Workload: 3,000,002-message synthetic ITCH day, 4 symbols, `replay_itch`,
GCC 15 `-O2`, WSL2 on an Intel Core Ultra 5 225U.

## Flat profile (top of it)

```
  %       self   calls      name
 60.8    0.93s  3,000,002   itch::Replayer<MapLadder>::apply          (decode + book work, inlined)
 19.0    0.29s    722,233   std::_Rb_tree::_M_erase_aux               (price level DESTROYED)
  7.2    0.11s  1,470,054   std::_Rb_tree::_M_emplace_hint_unique     (price level CREATED)
  0.7    0.01s  1,349,885   itch::decode_add_order
  0.7    0.01s          8   OpenAddressIdMap::rehash                  (before reserve warms up)
  0.0       ~   1,470,054   OrderPool::alloc                          (arena: invisible, as designed)
```

## What it says

**Finding: ~26% of replay time is `std::map` node churn** — 1.47M price-level
creations and 722k destructions. The M1 microbenchmark never showed this,
because its steady-state workload keeps levels alive; real(istic) order flow
constantly creates levels at new prices and empties them again. The
microbenchmark and the profile disagree, and the profile is measuring the
workload that matters.

**So the dense ladder should win replay?** No — and this is the part worth
remembering. Measured end-to-end on the same file, two runs each:

| ladder | replay throughput |
|---|---|
| tree map | 1.45, 1.49 M msg/s |
| dense array | 0.81, 1.09 M msg/s |

The dense ladder needs a per-symbol price range wide enough for anything the
feed might show — here [0, $200] in ticks — which is 2M levels × 2 sides ×
40 B ≈ **160 MB of ladder per symbol**, 640 MB across four books. The result
is page-fault and TLB pressure that costs more than the map's node churn
saves, and the first dense run (0.81) is visibly slower than the second
(1.09) because it pays the initial page-faulting. On one warm single-symbol
book (the M1 microbench) this cost is invisible; across a multi-symbol feed
it dominates.

**The layered conclusion**, which no single measurement gives:

1. Microbenchmark: ladder choice is a wash (~±5% on medians).
2. Profile: the map pays ~26% of replay in level churn — a real, visible cost.
3. End-to-end: the map still wins replay, because the dense array's
   footprint scales with price range × symbols and that costs more.

The fix that would actually help is neither ladder as-is: a **level-object
pool** for the map ladder (reusing map nodes is what an allocator-aware
`std::map` or an intrusive tree would do), or a dense ladder windowed around
the touch with re-anchoring. Both are measurable follow-ups; neither is
assumed to win.

Also visible, pleasingly: `OrderPool::alloc` — 1.47M calls, no measurable
self time. The arena is doing exactly what it was built to do.

## Reproducing

gprof (works anywhere GCC does):

```bash
g++ -std=c++20 -O2 -g -pg -fno-omit-frame-pointer -I include \
    tools/replay_itch.cpp -o replay_prof
./replay_prof day.itch > /dev/null && gprof -b replay_prof gmon.out | head -60
```

Flame graph proper (needs `perf`, i.e. root or a native Linux box):

```bash
perf record -F 2000 --call-graph dwarf ./build/tools/replay_itch day.itch
perf script | stackcollapse-perf.pl | flamegraph.pl > docs/replay-flame.svg
```

The ladder comparison:

```bash
./build/tools/replay_itch day.itch --ladder map   --depth 0
./build/tools/replay_itch day.itch --ladder dense --depth 0
```

## Postscript: the fixes, measured on the real day

Everything above was measured on the synthetic day; the real 8.25 GB NASDAQ
file (268.7M messages, 8,892 symbols) then re-ranked the bottlenecks twice.

**First surprise: I/O method stopped mattering.** The cold mmap replay ran at
2 MB/s — demand paging with no readahead, fixed 4.3× by chunked
`PrefetchVirtualMemory` — but the streamed reader landed at the *same*
~0.37 M msg/s. The disk streams 200+ MB/s; the replay used 11. The actual
bound was the working set: ~8 GB of live books plus the file fighting over
15.5 GB of RAM. When memory is the wall, how bytes arrive is irrelevant.

**So the fixes went after memory and churn instead:**

| change | full-day rate |
|---|---|
| baseline (std::unordered_map routing, plain map ladder) | 0.37 M msg/s |
| open-addressed order→book routing (~64 B → 16 B per live order) | 0.40 M msg/s |
| + pooled ladder (node handles: extract, re-key, splice) | **0.45 M msg/s** |

The pooled ladder — built because this profile showed 26% level churn — is
the fastest of the three ladders on real data, +12% over the plain tree.
Profile, hypothesis, fix, measured win: the loop this document exists for.

For calibration, the same binary with a symbol filter (one book, small
working set) parses and routes the same 268.7M messages at **6.6–7.5 M
msg/s**: that is the parser's actual speed when memory fits, and the
distance from 0.45 is the price of the full-day working set.
