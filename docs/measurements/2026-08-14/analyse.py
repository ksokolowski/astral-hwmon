#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Turn a capture into the numbers a threshold design would actually rest on.

Three questions, in order of how much they matter for 12VHPWR safety:

  1. How much current does each pin carry, and how does that scale with load?
  2. How evenly is it shared? Imbalance is the leading indicator - a connector
     failing on one pin pushes its current onto the others long before any
     single pin reaches an absolute limit.
  3. How much does each pin sag per amp it carries? That is a proxy for the
     contact resistance of that pin, and it is the slowest-moving, most
     diagnostic signal available from this sensor set.

Reads the CSV the logger writes, including the CRLF it emits.
"""

from __future__ import annotations

import argparse
import csv
import statistics as st
from pathlib import Path

PINS = 6


def load(path: Path) -> list[dict[str, float]]:
    rows: list[dict[str, float]] = []
    with path.open(newline="") as fh:
        for raw in csv.DictReader(fh):
            # The last column carries a stray CR when the logger used csv's
            # RFC 4180 default; treat it as the empty string it means.
            err = (raw.get("read_error") or "").strip()
            if err:
                continue
            if not raw.get("ma1"):
                continue
            row = {
                "elapsed": float(raw["elapsed"]),
                "sum": float(raw["ma_sum"]),
                "spread": float(raw["ma_spread"]),
                "watts": float(raw["gpu_w"]) if raw.get("gpu_w") else 0.0,
            }
            for p in range(1, PINS + 1):
                row[f"ma{p}"] = float(raw[f"ma{p}"])
                row[f"mv{p}"] = float(raw[f"mv{p}"])
            rows.append(row)
    return rows


def pct(values: list[float], q: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    idx = min(len(ordered) - 1, int(round(q * (len(ordered) - 1))))
    return ordered[idx]


def per_pin_table(rows: list[dict[str, float]], title: str) -> None:
    print(f"\n{title}  (n={len(rows)})")
    if not rows:
        print("  no samples")
        return
    print(f"  {'pin':>4} {'mean mA':>9} {'p50':>7} {'p95':>7} {'max':>7} "
          f"{'share%':>7} {'mean mV':>9} {'min mV':>7}")
    total_mean = sum(st.mean([r[f"ma{p}"] for r in rows]) for p in range(1, PINS + 1))
    for p in range(1, PINS + 1):
        ma = [r[f"ma{p}"] for r in rows]
        mv = [r[f"mv{p}"] for r in rows]
        share = 100 * st.mean(ma) / total_mean if total_mean else 0
        print(f"  {p:>4} {st.mean(ma):>9.0f} {pct(ma, 0.5):>7.0f} {pct(ma, 0.95):>7.0f} "
              f"{max(ma):>7.0f} {share:>7.2f} {st.mean(mv):>9.0f} {min(mv):>7.0f}")


def imbalance(rows: list[dict[str, float]], title: str) -> None:
    """Spread and max/mean ratio - the metrics a guard would actually watch."""
    if not rows:
        return
    ratios = []
    spreads = []
    for r in rows:
        pins = [r[f"ma{p}"] for p in range(1, PINS + 1)]
        mean = sum(pins) / PINS
        if mean <= 0:
            continue
        ratios.append(max(pins) / mean)
        spreads.append(max(pins) - min(pins))
    if not ratios:
        return
    print(f"\n{title}")
    print(f"  max/mean ratio : mean {st.mean(ratios):.4f}  p95 {pct(ratios, 0.95):.4f}  "
          f"max {max(ratios):.4f}")
    print(f"  spread (mA)    : mean {st.mean(spreads):.0f}  p95 {pct(spreads, 0.95):.0f}  "
          f"max {max(spreads):.0f}")


def droop_per_amp(rows: list[dict[str, float]]) -> None:
    """Relative contact-resistance proxy for each pin.

    All six pins feed the same rail, so the difference between the highest-
    reading pin and this one, divided by this pin's current, estimates how much
    more resistive its path is. Only meaningful under load - at idle the
    currents are too small and the 8 mV quantisation dominates.
    """
    loaded = [r for r in rows if r["sum"] >= 6000]
    print(f"\nDroop per amp, relative to the highest-reading pin  (n={len(loaded)}, "
          f"samples with total >= 6 A)")
    if len(loaded) < 30:
        print("  not enough loaded samples to be meaningful")
        return
    print(f"  {'pin':>4} {'mOhm-ish':>10}   (higher = more resistive path)")
    for p in range(1, PINS + 1):
        vals = []
        for r in loaded:
            mv = [r[f"mv{q}"] for q in range(1, PINS + 1)]
            drop = max(mv) - r[f"mv{p}"]
            amps = r[f"ma{p}"] / 1000.0
            if amps > 0.5:
                vals.append(drop / amps)
        if vals:
            print(f"  {p:>4} {st.mean(vals):>10.1f}")


def by_load(rows: list[dict[str, float]]) -> None:
    bands = [
        ("idle      (< 4 A total)", lambda r: r["sum"] < 4000),
        ("light     (4-10 A)", lambda r: 4000 <= r["sum"] < 10000),
        ("medium    (10-20 A)", lambda r: 10000 <= r["sum"] < 20000),
        ("heavy     (20-35 A)", lambda r: 20000 <= r["sum"] < 35000),
        ("full      (>= 35 A)", lambda r: r["sum"] >= 35000),
    ]
    print("\nBy load band")
    print(f"  {'band':<24} {'n':>6} {'mean W':>8} {'max pin mA':>11} "
          f"{'mean spread':>12} {'max/mean':>9}")
    for label, pred in bands:
        sel = [r for r in rows if pred(r)]
        if not sel:
            continue
        ratios = []
        for r in sel:
            pins = [r[f"ma{p}"] for p in range(1, PINS + 1)]
            mean = sum(pins) / PINS
            if mean > 0:
                ratios.append(max(pins) / mean)
        watts = [r["watts"] for r in sel if r["watts"] > 0]
        print(f"  {label:<24} {len(sel):>6} {st.mean(watts) if watts else 0:>8.0f} "
              f"{max(r['spread'] + 0 for r in sel) and max(max(r[f'ma{p}'] for p in range(1, PINS + 1)) for r in sel):>11.0f} "
              f"{st.mean([r['spread'] for r in sel]):>12.0f} "
              f"{st.mean(ratios) if ratios else 0:>9.4f}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", type=Path)
    args = ap.parse_args()

    rows = load(args.csv)
    if not rows:
        print("no usable rows")
        return 1

    dur = rows[-1]["elapsed"] - rows[0]["elapsed"]
    watts = [r["watts"] for r in rows if r["watts"] > 0]
    print(f"{args.csv.name}: {len(rows)} samples over {dur / 60:.1f} min")
    if watts:
        print(f"GPU power: mean {st.mean(watts):.0f} W, p95 {pct(watts, 0.95):.0f} W, "
              f"max {max(watts):.0f} W")
    print(f"12VHPWR total: mean {st.mean([r['sum'] for r in rows]) / 1000:.2f} A, "
          f"max {max(r['sum'] for r in rows) / 1000:.2f} A")

    per_pin_table(rows, "Per pin, whole capture")
    loaded = [r for r in rows if r["sum"] >= 6000]
    if loaded:
        per_pin_table(loaded, "Per pin, loaded samples only (total >= 6 A)")
    imbalance(rows, "Imbalance, whole capture")
    if loaded:
        imbalance(loaded, "Imbalance, loaded samples only")
    by_load(rows)
    droop_per_amp(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
