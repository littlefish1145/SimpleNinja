#!/usr/bin/env python3
"""Analyse a sninja timeline CSV (see src/core/timeline.h).

Answers the questions aggregate counters cannot: how much of a build's wall clock
is ramp-up, how much is tail, and how well packed the middle is.  Also reports
per-worker occupancy, which is what makes a stuck worker visible.

Usage:
    sninja -C build -j 4          # with SNINJA_TIMELINE=timeline.csv
    python scripts/timeline.py timeline.csv [-j 4] [--ascii]
"""

import argparse
import sys
from collections import defaultdict

# Must match Timeline::Kind in src/core/timeline.h.
KINDS = {
    0: "edge_start",
    1: "launched",
    2: "exited",
    3: "edge_finish",
    4: "wait_enter",
    5: "wait_exit",
    6: "find_empty",
    7: "stuck",
}


def read(path):
    rows = []
    with open(path, newline="") as f:
        header = f.readline()
        if not header.startswith("t_ms"):
            sys.exit(f"{path}: not a timeline CSV (bad header)")
        for line in f:
            parts = line.rstrip("\n").split(",", 4)
            if len(parts) < 5:
                continue
            rows.append(
                {
                    "t": float(parts[0]),
                    "kind": KINDS.get(int(parts[1]), parts[1]),
                    "worker": int(parts[2]),
                    "key": parts[3],
                    "label": parts[4],
                }
            )
    if not rows:
        sys.exit(f"{path}: no events")
    return rows


def spans(rows, start_kind, end_kind, end_t=None):
    """Pair up start/end events per edge, returning (key, label, start, end, worker).

    Edges whose end never arrived -- because the snapshot came from a build that
    was still running or was killed -- are returned too, running to `end_t` and
    flagged by end == end_t.  Those are exactly the edges worth looking at when
    diagnosing a hang, so dropping them would hide the reason the timeline was
    collected.
    """
    open_starts = {}
    finished = []
    for r in rows:
        if r["kind"] == start_kind:
            open_starts[r["key"]] = r
        elif r["kind"] == end_kind:
            s = open_starts.pop(r["key"], None)
            if s is not None:
                finished.append((r["key"], r["label"], s["t"], r["t"], s["worker"], False))
    if end_t is not None:
        for key, s in open_starts.items():
            finished.append((key, s["label"], s["t"], end_t, s["worker"], True))
    return finished


def percentile(values, p):
    if not values:
        return 0.0
    ordered = sorted(values)
    k = (len(ordered) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(ordered) - 1)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (k - lo)


def occupancy(rows, end_t, width=10.0):
    """Time-weighted count of edges in flight, in `width` ms buckets.

    An edge counts as occupying a worker from edge_start to edge_finish: that is
    the span during which the worker cannot take anything else, CreateProcess
    included.
    """
    buckets = defaultdict(float)
    for key, label, start, end, worker, unfinished in spans(rows, "edge_start", "edge_finish", end_t):
        if end <= start:
            continue
        b0 = int(start / width)
        b1 = int(end / width)
        for b in range(b0, b1 + 1):
            lo = max(start, b * width)
            hi = min(end, (b + 1) * width)
            if hi > lo:
                buckets[b] += hi - lo
    return buckets, width


def ascii_chart(buckets, width, jobs, end_t):
    if not buckets:
        return
    peak = max(buckets.values()) / width
    height = 12
    print()
    print(f"  concurrency over time (each row = {height} units of `-j`)")
    lo_b, hi_b = min(buckets), max(buckets)
    for row in range(height, 0, -1):
        line = []
        threshold = row * (peak / height)
        for b in range(lo_b, hi_b + 1):
            value = buckets.get(b, 0.0) / width
            line.append("#" if value >= threshold else " ")
        print(f"  {row:2d} |" + "".join(line))
    axis = "".join("|" if b % 10 == 0 else " " for b in range(lo_b, hi_b + 1))
    print("     +" + axis)
    ticks = "".join(
        "|" if b % 10 == 0 else ("." if b % 5 == 0 else " ") for b in range(lo_b, hi_b + 1)
    )
    print("      " + ticks)
    span_s = end_t / 1000.0
    label_gap = max(0, int(span_s) - 1)
    print(f"      0{' ' * label_gap}{int(span_s)}s   (peak {peak:.2f}, -j {jobs})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("-j", "--jobs", type=int, default=0,
                    help="nominal parallelism, for the chart's vertical scale")
    ap.add_argument("--ascii", action="store_true", help="draw the concurrency chart")
    args = ap.parse_args()

    rows = read(args.csv)
    end_t = max(r["t"] for r in rows)
    start_t = min(r["t"] for r in rows)
    workers = sorted({r["worker"] for r in rows})
    all_edges = spans(rows, "edge_start", "edge_finish", end_t)

    # Phony and dry-run edges finish at the instant they start, so they occupy no
    # time.  They are real edges in the build but they are not work, and leaving
    # them in corrupts the shape analysis: a phony's completion is by definition
    # the last timestamp, which would put the entire build in the "tail".
    edges = [e for e in all_edges if e[3] > e[2]]
    instant = len(all_edges) - len(edges)
    unfinished_edges = [e for e in edges if e[5]]

    print(f"file        : {args.csv}")
    print(f"events      : {len(rows)}")
    print(f"span        : {end_t - start_t:.1f} ms")
    print(f"workers     : {len(workers)} ({workers})")
    print(f"edges       : {len(all_edges)} seen ({len(edges)} did work,"
          f" {instant} instant/phony, {len(unfinished_edges)} still running"
          f" when the snapshot was taken)")
    if unfinished_edges:
        print()
        print("NOTE: this snapshot was taken from a build that had not finished.")
        print("      Edges below started and never completed -- which is the whole")
        print("      reason a timeline gets collected from a killed or hung build.")
        print(f"      {'worker':>6}  {'started ms':>10}  {'ran for':>9}  edge")
        for key, label, start, end, worker, unfinished in sorted(
            unfinished_edges, key=lambda x: x[2]
        ):
            print(f"      {worker:>6}  {start:>10.1f}  {end - start:>8.1f}ms  {label}")

    if not edges:
        sys.exit("timeline has no edge events at all; it was captured before any work started")

    # ---- ramp -------------------------------------------------------------
    # Ramp is the time until every worker has started something, i.e. until the
    # machine is actually saturated.  Anything before that is startup, not
    # scheduling.
    first_start = {}
    for key, label, start, end, worker, unfinished in edges:
        first_start.setdefault(worker, start)
    ramp_end = max(first_start.values()) if first_start else 0.0

    # ---- tail -------------------------------------------------------------
    # Tail is the gap between the last edge starting and the last edge finishing:
    # the point where workers run out of work and the build drains.  Measured on
    # the *end* of each edge, because that is when a worker becomes free again --
    # using the start would fold the last edge's whole runtime into the tail.
    last_start = max(s for _, _, s, _, _, _ in edges)
    last_finish = max(e for _, _, _, e, _, _ in edges)
    tail = last_finish - last_start

    # An edge whose runtime dominates the end of the build is not really "tail":
    # it is the last edge doing its work.  Report the drain as the time from when
    # the final edge started to the moment only one edge was still in flight.
    # The honest tail: how long the build spent with fewer than all workers busy
    # at the end.  Computed from the completion times rather than the start times,
    # because a long final edge is work, not drain.
    finishes = sorted(e for _, _, _, e, _, _ in edges)
    if len(finishes) >= 2:
        # Everything after the second-to-last completion is serial draining.
        serial = end_t - finishes[-2]
    else:
        serial = tail

    middle = end_t - ramp_end - max(tail, serial)
    child = spans(rows, "edge_start", "exited", end_t)
    child_total = sum(max(0.0, e - s) for _, _, s, e, _, _ in child)
    launch_total = sum(max(0.0, e - s) for _, _, s, e, _, _ in spans(rows, "edge_start", "launched", end_t))

    print()
    print("=== shape ===")
    print(f"  ramp   : {ramp_end:8.1f} ms  ({ramp_end / end_t * 100:4.1f}%)"
          f"   until worker {max(first_start, key=first_start.get)} starts its first edge")
    print(f"  middle : {middle:8.1f} ms  ({middle / end_t * 100:4.1f}%)  all workers busy")
    print(f"  tail   : {serial:8.1f} ms  ({serial / end_t * 100:4.1f}%)"
          f"   after the 2nd-to-last edge finished at {finishes[-2]:.0f} ms")
    print(f"           (last edge started {last_start:.0f} ms, finished {last_finish:.0f} ms;"
          f" that {tail:.0f} ms is the final link, not idle drain)")
    print()
    print("=== per-worker time budget (summed over workers; divide by -j) ===")
    wall = end_t - start_t
    n = len(workers)
    print(f"  wall clock           : {wall:8.1f} ms")
    print(f"  edge occupancy total : {child_total:8.1f} ms"
          f"  -> {child_total / n:7.1f} ms per worker ({child_total / (wall * n) * 100:.1f}% of it)")
    print(f"  of which CreateProcess: {launch_total:7.1f} ms"
          f"  -> {launch_total / n:7.1f} ms per worker ({launch_total / wall * 100:.1f}% of wall)")
    print(f"  effective concurrency : {child_total / wall:8.2f} / {n}")

    # ---- edge durations ---------------------------------------------------
    durs = [e - s for _, _, s, e, _, _ in edges]
    print()
    print("=== edge duration (edge_start -> edge_finish) ===")
    print(f"  n={len(durs)}  min={min(durs):.1f}  p50={percentile(durs, 0.5):.1f}"
          f"  p90={percentile(durs, 0.9):.1f}  max={max(durs):.1f} ms")

    # ---- worker occupancy -------------------------------------------------
    per_worker = defaultdict(lambda: [0.0, 0.0, 0])
    for key, label, start, end, worker, unfinished in edges:
        per_worker[worker][0] += max(0.0, end - start)
        per_worker[worker][1] += max(0.0, end - start) / max(1e-9, wall)
        per_worker[worker][2] += 1
    print()
    print("=== per-worker ===")
    print(f"  {'worker':>6}  {'edges':>5}  {'busy ms':>9}  {'of wall':>8}  {'idle ms':>8}")
    for w in workers:
        busy, frac, count = per_worker[w]
        print(f"  {w:>6}  {count:>5}  {busy:>9.1f}  {frac * 100:>7.1f}%  {wall - busy:>8.1f}")

    waits = defaultdict(float)
    for r in rows:
        if r["kind"] == "wait_enter":
            waits[r["worker"]] -= r["t"]  # placeholder, fixed below
    # Pair wait_enter/wait_exit per worker to get real wait time.
    wait_open = defaultdict(list)
    wait_total = defaultdict(float)
    wait_count = defaultdict(int)
    for r in rows:
        if r["kind"] == "wait_enter":
            wait_open[r["worker"]].append(r["t"])
        elif r["kind"] == "wait_exit" and wait_open[r["worker"]]:
            t0 = wait_open[r["worker"]].pop()
            wait_total[r["worker"]] += r["t"] - t0
            wait_count[r["worker"]] += 1
    unclosed = {w: len(v) for w, v in wait_open.items() if v}
    print()
    print("=== parking (wait_enter -> wait_exit; blocked in the kernel, not spinning) ===")
    for w in workers:
        print(f"  worker {w}: {wait_count[w]:>4} waits, {wait_total[w]:>8.1f} ms total")
    if unclosed:
        print(f"  UNCLOSED waits (worker parked when the timeline ended): {unclosed}")

    if args.ascii:
        buckets, width = occupancy(rows, end_t)
        ascii_chart(buckets, width, args.jobs or n, end_t)


if __name__ == "__main__":
    main()
