#!/usr/bin/env python3
"""Averages an in-run A/B test (NX_AB=<name>, nvn_vk.cpp): the per-60-frame lines of each half, after the first
<skip> lines (boot, menus).
usage: tools/play/ab_report.py <nx.log> [skip lines]"""
import re
import sys

lines = open(sys.argv[1], errors="replace").read().splitlines()[int(sys.argv[2]) if len(sys.argv) > 2 else 0:]
acc = {"A": [], "B": []}
for l in lines:
    m = re.match(r"nvn/vk: per frame over \d+: ([\d.]+) fps, [\d.]+ submits, replay ([\d.]+) ms, submit \(encode\) "
                 r"([\d.]+) ms.*ab ([AB])$", l)
    if m:
        acc[m.group(4)].append((float(m.group(1)), float(m.group(2)), float(m.group(3))))
for k, v in acc.items():
    if not v:
        continue
    n = len(v)
    print(f"{k}: {n} windows  fps {sum(x[0] for x in v) / n:5.2f} (min {min(x[0] for x in v):5.1f})  replay "
          f"{sum(x[1] for x in v) / n:5.2f} ms  encode {sum(x[2] for x in v) / n:5.2f} ms")
