# Run output

Unedited output from the runs behind the numbers in the main README, kept so
the claims can be checked against the tools that produced them. Only the
absolute path of the input file was rewritten to `./`.

The inputs are NASDAQ's published TotalView-ITCH files for 2019-12-30
(8.25 GB unpacked) and 2020-01-30 (12.95 GB). They are not in this repository;
download them from `emi.nasdaq.com/ITCH/Nasdaq ITCH/` and `gunzip` them.

| file | what it shows | command |
|---|---|---|
| `real-day-replay.txt` | full-day validation: 268,744,780 messages, 8,892 symbols, zero unknown order references, no crossed books, conservation in every book, plus the intraday message-rate profile | `replay_itch 12302019.NASDAQ_ITCH50 --minute-profile` |
| `real-day-open-addressed-routing.txt` | same replay with the open-addressed order-to-book index: 0.40 M msg/s | `replay_itch 12302019.NASDAQ_ITCH50 --ladder map` |
| `real-day-pooled-ladder.txt` | same replay with the pooled ladder as well: 0.45 M msg/s, the published rate | `replay_itch 12302019.NASDAQ_ITCH50 --ladder pooled` |
| `aapl-queue-model.txt` | market maker on real AAPL flow with exact queue position: 4,391 fills, +130 ticks/share captured, $1,317 net loss, markout curve from -136 at 1 ms to -182 at 1 s | `mm_sandbox 12302019.NASDAQ_ITCH50 --symbol AAPL --half-spread 100` |
| `aapl-optimistic-model.txt` | the same day under the any-print-fills-us model: 24,145 fills, 5.5x the queue model | `mm_sandbox 12302019.NASDAQ_ITCH50 --symbol AAPL --half-spread 100 --fill-model optimistic` |
| `second-day-replay.txt` | the same validation on a different session: 423,285,709 messages, 8,900 symbols, zero unknown references, no crossed books, conservation everywhere, with no code change | `replay_itch 01302020.NASDAQ_ITCH50 --minute-profile` |
| `second-day-statistics.txt` | the same distributions on the second session, which is what makes findings 7 to 9 properties of the market rather than of one file | `book_stats 01302020.NASDAQ_ITCH50 --out day2` |
| `day2-*.csv` | machine-readable form of the above | (written by the command above) |
| `real-day-statistics.txt` | the distributions behind findings 7 to 9: active levels, price span, order lifetime, order size, and cancel-to-trade by symbol | `book_stats 12302019.NASDAQ_ITCH50 --out stats` |
| `stats-*.csv` | the same data in machine-readable form; `tools/plot_stats.py` turns these into the README's charts | (written by the command above) |
| `self-trade-benchmark.txt` | what self-trade prevention costs when enabled but not firing: 181.0 vs 182.3 ns p50 | `pricetime_bench --ops 1000000 --ladder map --no-adversarial --no-idmap-compare` |

The replay throughput figures come from one machine (Intel Core Ultra 5 225U
laptop, 15.5 GB RAM) and are working-set-bound at this scale, so they say more
about the memory hierarchy than about the parser. `real-day-replay.txt` is an
earlier memory-mapped run at 0.34 M msg/s and is kept because it carries the
intraday profile; the two rows above it are the same file replayed after the
routing-index and ladder changes.

Full book latency benchmarks are otherwise not included here. The most recent
local run was taken on a thermally throttled machine (timer overhead p50 of 35 ns against
13 ns for the published run), so its numbers are not comparable to the table
in the README and publishing them alongside would be misleading. Reproduce
them with `pricetime_bench --ops 1000000`. The self-trade run above is the
exception: it was taken with a timer overhead of 13.0 ns p50, matching the
conditions of the published table, and its header records that.
