#!/usr/bin/env python3
"""Summarize opt-in PORT_FRAME_TIMES CSV over a selected gameplay interval."""
import argparse
import csv
import json
import math
from pathlib import Path


def summarize(path, start=0.0, end=math.inf):
    samples = []
    with Path(path).open() as stream:
        for row in csv.DictReader(stream):
            t_ms, dt = float(row['elapsed_ms']), float(row['interval_ms'])
            # Include only intervals fully inside the requested window.
            if math.isfinite(t_ms) and math.isfinite(dt) and dt > 0 and t_ms - dt + 0.000001 >= start * 1000 and t_ms <= end * 1000 + 0.000001:
                samples.append(dt)
    if not samples:
        raise ValueError('no complete frame intervals in the selected window')
    ordered = sorted(samples)
    def percentile(q):
        return ordered[max(0, math.ceil(q * len(ordered)) - 1)]
    return dict(frames=len(samples), seconds=sum(samples)/1000,
                average_fps=1000*len(samples)/sum(samples),
                median_ms=percentile(.5), p95_ms=percentile(.95), p99_ms=percentile(.99),
                max_ms=ordered[-1], over_33_333_ms=sum(x > 1000/30 for x in samples))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('csv', type=Path)
    p.add_argument('--start', type=float, default=0, help='seconds since first guest frame')
    p.add_argument('--end', type=float, default=math.inf)
    a = p.parse_args()
    if a.start < 0 or a.end <= a.start:
        p.error('require 0 <= start < end')
    try:
        print(json.dumps(summarize(a.csv, a.start, a.end), indent=2))
    except (OSError, ValueError, KeyError) as e:
        p.error(str(e))


if __name__ == '__main__':
    main()
