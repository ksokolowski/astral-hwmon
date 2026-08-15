# Reference data, 2026-08-14 — real-workload baseline

The "sunny day" baseline: a healthy card under a real game, as the reference every future
capture is compared against. Analysed in `docs/GUARD-DESIGN.md`.

Reference machine, ROG Astral RTX 5090 OC `0x89E31043`, chip on `i2c-4` at `0x2b`, driver
`astral-hwmon` 0.1.0 installed via DKMS on kernel 7.0.0-29.

## `gaming-hots-2hz.csv`

Heroes of the Storm, 56.9 minutes, 6834 samples at 2 Hz, read from the driver's hwmon sysfs
alongside `nvidia-smi` telemetry. **Zero plausibility-gate rejections and zero read errors.**

Columns are named in the header row and are **pin 1..6 in order** — unlike
`../2026-08-13/load-decoded.txt`, which is in raw block order. `ma_sum`, `ma_max`, `ma_min`,
`ma_spread` and `mv_min` are derived per row for convenience.

Line endings are CRLF: `csv.writer` defaults to RFC 4180, and this capture was taken before
that was changed. Analysis must strip the trailing `\r` or the last column reads as
one character rather than empty. `analyse.py` handles it; a naive `awk -F,` will not, and
that mistake produced a burst of false alarms while this very session was being recorded.

## Headline numbers

GPU power mean 130 W, p95 179 W, max 244 W. 12VHPWR total mean 11.27 A, max 18.56 A.
Busiest pin 3.34 A — 35% of the 9.5 A per-pin rating.

Per-pin current share, loaded samples (total ≥ 6 A):

| pin | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|
| share | 16.12% | 16.49% | 16.60% | 15.85% | **18.06%** | 16.87% |

Matches the shares decoded from `../2026-08-13/idle-raw-frames.txt` (16.10 / 16.65 / 16.44 /
15.66 / 18.12 / 17.04) within ~0.2 pp — across a day, a reboot, and a different workload.
That stability is what makes the distribution usable as a fingerprint.

Imbalance under load: max/mean ratio mean 1.084, p95 1.103, max 1.155.

**Idle is not usable for share analysis.** Below ~4 A total the 20 mA quantisation step is
~4% of each pin's reading and the shares scatter (max/mean 1.12) without the underlying
imbalance changing.

## Tooling

`log-session.py` — the capture script. Run it under `systemd-run --user` so it outlives the
shell that started it. `--hz 2` is the practical ceiling given the driver's 200 ms cache.

`analyse.py` — per-pin table, imbalance, load bands, and a droop-per-amp resistance proxy.
The droop metric needs burn-level currents to be meaningful; at gaming currents the 8 mV
voltage step is comparable to the whole pin-to-pin spread, so read those columns with
suspicion below ~5 A per pin.

## `heavy-witcher3-2hz.csv`

The Witcher 3, DX12 with ray tracing at 4K, 33.1 min, 3966 samples. Peak **477 W / 38.2 A
total / 6.82 A on the busiest pin**; 41 samples above 25 A. Zero rejections. Written after the
`lineterminator` fix, so this file is **LF, not CRLF**.

Most of the run is idle or menu load - the session was interrupted repeatedly by an NVIDIA
Xid 109 (CTX SWITCH TIMEOUT) that killed the game seven times, on two Proton versions, two
saves and a new game. Worth recording for its own reason: **the sensor path survived all seven
GPU faults with zero dropped reads.** The I2C bus the chip sits on is independent of the
graphics engine, so telemetry keeps working when the GPU's graphics context has died - which
is exactly when a guard would most need it.

Fingerprint in the high-load band (total >= 25 A):

| pin | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|
| share | 16.02% | 16.24% | 16.97% | 16.18% | **17.93%** | 16.66% |

Pin 5's share falls slightly as current rises (18.12% idle -> 18.07% gaming -> 17.93% heavy):
thermal self-balancing, the busiest pin heating and shedding current. Imbalance is therefore
*lowest* at full load - max/mean 1.0695 above 35 A against 1.0960 at idle.

## `burn-2hz.csv` — full power, and the gap closed

The CUDA burn at 2 Hz, 322 samples over 2.7 min, 235 of them at **>= 35 A total**. Peak
**581 W / 48.26 A total / 8.56 A on the busiest pin**, minimum 11.944 V. Zero rejections, zero
read errors. LF line endings.

This is the highest current ever measured on this card, and it closes the gap that every
earlier version of this file and of `docs/GUARD-DESIGN.md` recorded as blocking: the design
had never been checked against a load anywhere near the connector's rating.

**The fingerprint extrapolation holds.** `docs/GUARD-DESIGN.md` §7b derived per-pin shares
from the Witcher 3 heavy-load band (>= 25 A) and projected them to 600 W. Measured at 48.26 A:

| pin | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|
| projected from 25 A band | 16.02% | 16.24% | 16.97% | 16.18% | **17.93%** | 16.66% |
| measured at 48 A | 15.85% | 16.23% | 17.17% | 16.41% | **17.75%** | 16.59% |
| error (pp) | -0.17 | -0.01 | +0.20 | +0.23 | -0.18 | -0.07 |

Every pin lands within 0.23 pp of its projection — at or inside the 0.2 pp noise floor, across
a 1.9x extrapolation in total current. A fingerprint measured at gaming loads predicts
behaviour at full power, which is the property the whole drift metric depends on.

**Headroom at full power.** The busiest pin reached 8.56 A: 90% of the 9.5 A per-pin rating,
93% of ASUS's 9.2 A warning point. The burn drew 581 W against the card's 600 W limit, so
scaling that share to 600 W puts the pin at ~8.84 A, 7.5% under the rating. That is the number
that makes the imbalance metrics worth having - on a *healthy* connector there is not much room
between full load and rated.

**Thermal self-balancing continues.** Pin 5's share falls 18.12% idle -> 18.07% gaming ->
17.93% at 38 A -> 17.75% at 48 A, and max/mean falls with it: 1.0864 idle, 1.0665 at 20-35 A,
**1.0646 above 35 A**. The connector shares *better* the harder it is pushed, so any imbalance
threshold calibrated at idle would be far too loose at the load where it matters.

## `burn-registers.csv` and `regscan.py`

The register survey - see `register-map.md`. `regscan.py` sweeps `0x00`-`0xFF` in two passes
and classifies each chunk live or static; `burn-registers.csv` is 166 samples of the candidate
regions taken across a 583 W burn, which is what identified the eight temperature channels at
`0x60`, the eighth `(mV, mA)` channel at `0xa0`, and the 1 Hz time-in-state counters at `0xc0`.

Both are **read-only**. `regscan.py` has no write path and must not acquire one.
