#!/usr/bin/env python3
"""Render the charts in docs/img from book_stats output.

    python tools/plot_stats.py --stats PREFIX --replay docs/runs/real-day-replay.txt

Writes plain SVG with no dependencies, because a chart in a README should not
require a toolchain to regenerate. Colours are mid-tones that stay legible on
GitHub's light and dark backgrounds, and text colour follows the viewer's
colour scheme through a media query the SVG carries itself.
"""
import argparse
import os
import re

W = 760

INK = "#5B6772"        # readable on both grounds
ACCENT = "#2E8B7A"     # measurement
WARN = "#C2703D"       # the number that contradicts an assumption
GRID = "#B9C2CB"

STYLE = """  <style>
    .t { font: 13px -apple-system, 'Segoe UI', Helvetica, Arial, sans-serif; fill: %s; }
    .lbl { font: 12px 'SF Mono', Consolas, monospace; fill: %s; }
    .num { font: 12px 'SF Mono', Consolas, monospace; fill: %s; }
    .hd { font: 600 15px -apple-system, 'Segoe UI', Helvetica, Arial, sans-serif; fill: %s; }
    .sub { font: 12px -apple-system, 'Segoe UI', Helvetica, Arial, sans-serif; fill: %s; }
    @media (prefers-color-scheme: dark) {
      .t, .lbl, .num, .sub { fill: #98A6B2; }
      .hd { fill: #D6DEE6; }
    }
  </style>
""" % (INK, INK, INK, "#2B3540", INK)


def svg(width, height, body):
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
        'viewBox="0 0 %d %d" role="img">\n%s%s</svg>\n'
        % (width, height, width, height, STYLE, body)
    )


def esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def commas(n):
    return "{:,}".format(int(n))


def read_csv(path):
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, "r", encoding="utf-8") as f:
        header = f.readline()
        del header
        for line in f:
            line = line.strip()
            if line:
                rows.append(line.split(","))
    return rows


# ---------------------------------------------------------------- lifetimes
def chart_lifetimes(rows, out):
    if not rows:
        return False
    labels = [r[0] for r in rows]
    counts = [int(r[1]) for r in rows]
    total = sum(counts) or 1
    top, left, bar_h, gap = 62, 122, 20, 7
    height = top + len(rows) * (bar_h + gap) + 34
    # Room for the widest annotation, which is the one on the modal bucket.
    plot_w = W - left - 168
    peak = max(counts) or 1
    # The finding worth seeing is how much of the day dies almost immediately,
    # so the sub-10ms buckets carry the contrast colour.
    fast = {"< 1 us", "1-10 us", "10-100 us", "100 us - 1 ms", "1-10 ms"}

    b = ['  <text x="0" y="20" class="hd">How long a real order rests before it is removed</text>']
    b.append('  <text x="0" y="40" class="sub">NASDAQ, 2019-12-30. %s sampled orders, add to removal.</text>'
             % commas(total))
    cum = 0
    for i, (lab, n) in enumerate(zip(labels, counts)):
        y = top + i * (bar_h + gap)
        cum += n
        w = max(1.0, plot_w * n / peak)
        pct = 100.0 * n / total
        cpct = 100.0 * cum / total
        colour = WARN if lab.strip() in fast else ACCENT
        b.append('  <text x="%d" y="%d" class="lbl" text-anchor="end">%s</text>'
                 % (left - 10, y + 14, esc(lab)))
        b.append('  <rect x="%d" y="%d" width="%.1f" height="%d" fill="%s" opacity="0.85" rx="1"/>'
                 % (left, y, w, bar_h, colour))
        b.append('  <text x="%.1f" y="%d" class="num">%.1f%% (%.0f%% cum)</text>'
                 % (left + w + 8, y + 14, pct, cpct))
    fast_share = 100.0 * sum(n for lab, n in zip(labels, counts)
                             if lab.strip() in fast) / total
    b.append('  <text x="0" y="%d" class="sub">Orange: %.0f%% of all orders are gone within '
             '10 milliseconds. The median is 1.2 seconds.</text>' % (height - 12, fast_share))
    with open(out, "w", encoding="utf-8") as f:
        f.write(svg(W, height, "\n".join(b) + "\n"))
    return True


# -------------------------------------------------------------------- spans
def chart_spans(rows, out):
    """Slots a dense ladder would need per side, against the naive sizing."""
    if not rows:
        return False
    buckets = [(int(r[0]), int(r[1])) for r in rows]
    total = sum(n for _, n in buckets) or 1
    top, left = 74, 58
    height = 300
    plot_w = W - left - 30
    plot_h = height - top - 54
    peak = max(n for _, n in buckets) or 1
    import math
    lo = math.log10(max(1, buckets[0][0]))
    hi = max(math.log10(2000000.0), math.log10(max(1, buckets[-1][0])))
    span = (hi - lo) or 1.0

    def x_of(v):
        return left + plot_w * (math.log10(max(1, v)) - lo) / span

    b = ['  <text x="0" y="20" class="hd">Why an array-indexed price ladder cannot work on real data</text>']
    b.append('  <text x="0" y="40" class="sub">Slots between the best and worst active price, per book side, '
             'log scale. A tree pays for levels; an array pays for this.</text>')
    # gridlines at powers of ten
    p = int(lo)
    while p <= hi:
        x = x_of(10 ** p)
        b.append('  <line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="%s" stroke-width="1" opacity="0.35"/>'
                 % (x, top, x, top + plot_h, GRID))
        b.append('  <text x="%.1f" y="%d" class="num" text-anchor="middle">10^%d</text>'
                 % (x, top + plot_h + 18, p))
        p += 1
    bar_w = max(2.0, plot_w / (len(buckets) + 2))
    for v, n in buckets:
        x = x_of(v)
        h = plot_h * n / peak
        b.append('  <rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="%s" opacity="0.8" rx="1"/>'
                 % (x - bar_w / 2, top + plot_h - h, bar_w, h, ACCENT))
    # Where the repo's own dense ladder was sized, for scale.
    xn = x_of(2000000)
    b.append('  <line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="%s" stroke-width="2" stroke-dasharray="5 4"/>'
             % (xn, top - 8, xn, top + plot_h, WARN))
    b.append('  <text x="%.1f" y="%d" class="num" text-anchor="middle">this repo\'s dense ladder: 2,000,000 slots</text>'
             % (xn, top - 12))
    b.append('  <text x="0" y="%d" class="sub">Half of all book sides already need more than 255,800 slots, '
             'and a tenth need billions.</text>' % (height - 14))
    with open(out, "w", encoding="utf-8") as f:
        f.write(svg(W, height, "\n".join(b) + "\n"))
    return True


# ------------------------------------------------------------ cancel ratios
def chart_cancel(rows, out, n_show=16):
    if not rows:
        return False
    recs = []
    for r in rows:
        sym, adds, removes, execs = r[0], int(r[1]), int(r[2]), int(r[3])
        if execs > 0 and adds > 50000:
            recs.append((sym, removes / float(execs), adds))
    if not recs:
        return False
    recs.sort(key=lambda t: -t[2])
    recs = recs[:n_show]
    recs.sort(key=lambda t: -t[1])

    top, left, bar_h, gap = 62, 74, 18, 6
    height = top + len(recs) * (bar_h + gap) + 32
    plot_w = W - left - 92
    import math
    peak = math.log10(max(r[1] for r in recs) or 10)

    b = ['  <text x="0" y="20" class="hd">Cancels per trade, by symbol</text>']
    b.append('  <text x="0" y="40" class="sub">Order removals per execution across the day. '
             'Log scale; the feed-wide average is 20.4.</text>')
    for i, (sym, ratio, _) in enumerate(recs):
        y = top + i * (bar_h + gap)
        w = max(2.0, plot_w * math.log10(max(1.0, ratio)) / (peak or 1))
        colour = WARN if ratio > 100 else ACCENT
        b.append('  <text x="%d" y="%d" class="lbl" text-anchor="end">%s</text>'
                 % (left - 10, y + 13, esc(sym)))
        b.append('  <rect x="%d" y="%d" width="%.1f" height="%d" fill="%s" opacity="0.85" rx="1"/>'
                 % (left, y, w, bar_h, colour))
        b.append('  <text x="%.1f" y="%d" class="num">%s : 1</text>'
                 % (left + w + 8, y + 13, ("%.0f" % ratio) if ratio >= 10 else ("%.1f" % ratio)))
    with open(out, "w", encoding="utf-8") as f:
        f.write(svg(W, height, "\n".join(b) + "\n"))
    return True


# ---------------------------------------------------------------- intraday
def chart_intraday(replay_path, out):
    """Message rate by time of day, from replay_itch --minute-profile output.

    The profile prints only non-empty buckets and appends an ASCII bar to the
    busy ones, so bars are placed by clock time rather than by line number.
    Reading it positionally would silently chart the quiet periods only.
    """
    if not os.path.exists(replay_path):
        return False
    by_bucket = {}
    with open(replay_path, "r", encoding="utf-8", errors="replace") as f:
        started = False
        for line in f:
            if "intraday message rate" in line:
                started = True
                continue
            if not started:
                continue
            m = re.match(r"\s*(\d{2}):(\d{2})\s+([\d,]+)\s*#*\s*$", line)
            if not m:
                if line.strip() == "":
                    continue
                if started and by_bucket:
                    break
                continue
            hh, mm = int(m.group(1)), int(m.group(2))
            by_bucket[hh * 12 + mm // 5] = int(m.group(3).replace(",", ""))
    if len(by_bucket) < 10:
        return False

    # Trim to the range that carries traffic; a full 24 hours is mostly empty.
    lo_b, hi_b = min(by_bucket), max(by_bucket)
    buckets = [(b, by_bucket.get(b, 0)) for b in range(lo_b, hi_b + 1)]

    top, left = 66, 74
    height = 320
    plot_w = W - left - 24
    plot_h = height - top - 52
    peak = max(v for _, v in buckets) or 1
    total = sum(v for _, v in buckets)
    nonzero = sum(1 for _, v in buckets if v > 0) or 1
    mean = total / float(nonzero)
    peak_b = max(buckets, key=lambda t: t[1])[0]

    b = ['  <text x="0" y="20" class="hd">Message rate across the trading day</text>']
    b.append('  <text x="0" y="40" class="sub">%s messages in 5-minute buckets. The busiest bucket '
             'carries %.0fx the active-hours mean.</text>' % (commas(total), peak / mean))
    for frac in (0.0, 0.5, 1.0):
        y = top + plot_h - plot_h * frac
        b.append('  <line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s" stroke-width="1" opacity="0.35"/>'
                 % (left, y, left + plot_w, y, GRID))
        b.append('  <text x="%d" y="%.1f" class="num" text-anchor="end">%s</text>'
                 % (left - 8, y + 4, commas(int(peak * frac))))
    bw = plot_w / float(len(buckets))
    for bucket, v in buckets:
        if v == 0:
            continue
        h = plot_h * v / peak
        x = left + (bucket - lo_b) * bw
        # The open and the close are the two structural features worth seeing.
        colour = WARN if v > peak * 0.45 else ACCENT
        b.append('  <rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" fill="%s" opacity="0.85"/>'
                 % (x, top + plot_h - h, max(1.0, bw - 0.5), h, colour))
    for bucket in range(lo_b, hi_b + 1):
        if bucket % 12 != 0 or (bucket - lo_b) % 24 != 0:
            continue
        x = left + (bucket - lo_b) * bw
        b.append('  <text x="%.1f" y="%d" class="num" text-anchor="middle">%02d:00</text>'
                 % (x, top + plot_h + 18, bucket // 12))
    b.append('  <text x="0" y="%d" class="sub">Orange: the opening cross at 09:30 and the closing '
             'cross at 16:00, in Eastern time.</text>' % (height - 12))
    del peak_b
    with open(out, "w", encoding="utf-8") as f:
        f.write(svg(W, height, "\n".join(b) + "\n"))
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stats", required=True, help="prefix passed to book_stats --out")
    ap.add_argument("--replay", default="docs/runs/real-day-replay.txt")
    ap.add_argument("--outdir", default="docs/img")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    made = []
    if chart_lifetimes(read_csv(args.stats + "-lifetimes.csv"),
                       os.path.join(args.outdir, "order-lifetime.svg")):
        made.append("order-lifetime.svg")
    if chart_spans(read_csv(args.stats + "-spans.csv"),
                   os.path.join(args.outdir, "ladder-slots.svg")):
        made.append("ladder-slots.svg")
    if chart_cancel(read_csv(args.stats + "-symbols.csv"),
                    os.path.join(args.outdir, "cancel-ratio.svg")):
        made.append("cancel-ratio.svg")
    if chart_intraday(args.replay, os.path.join(args.outdir, "intraday-rate.svg")):
        made.append("intraday-rate.svg")

    for m in made:
        print("wrote %s/%s" % (args.outdir, m))
    if not made:
        raise SystemExit("no charts written; check the --stats prefix")


if __name__ == "__main__":
    main()
