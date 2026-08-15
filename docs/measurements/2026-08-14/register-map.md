# Register map of the chip at `i2c-4` `0x2b`

The driver publishes one 32-byte block from `0x80` because that is the only region whose
meaning was established by measurement. This is a survey of the rest, taken to answer one
question that changes the design rather than merely decorating it:

> **Does this chip carry threshold, limit or status registers of its own?**

If it does, a hwmon-native alarm becomes possible and `../../GUARD-DESIGN.md` §1 has to be
rewritten. If it does not, the guard stays in userspace.

**Answer: it does not.** Detail in [What is not here](#what-is-not-here).

## Method

`regscan.py`, in this directory. Two passes over `0x00`–`0xFF` in 16-byte
`I2C_SMBUS_I2C_BLOCK_DATA` reads, spaced 3 s apart, so each chunk can be classified as live
or static. Then targeted sampling of the live regions against `nvidia-smi` telemetry across a
583 W burn and the cooldown after it.

**Read-only throughout.** `regscan.py` contains no write path and must not grow one. This is a
live GPU's I2C bus shared with the RGB controller; a stray write to an unknown register on an
unknown device is how people destroy hardware. Every number below came from a read.

Captured on kernel 7.0.0-29 with the NVIDIA open kernel module **610.43.02**, after a reboot,
on the same card that produced the 2026-08-13 data. Every register that was static then is
static now, and `0x98` still tracks the pin sum at the same ratio, so the layout is not a
property of one driver version or one boot.

Every chunk in `0x00`–`0xFF` responded. There are no holes in the address space.

## The channel bank at `0x80`

The block the driver reads is not six channels followed by junk — it is a bank of
**`(u16 BE millivolts, u16 BE milliamps)` channels**, of which the driver exposes six:

| offset | channel | idle | 583 W burn | status |
|---|---|---|---|---|
| `0x80`–`0x97` | pins 6…1, reverse order | 12.17 V, 0.36–0.42 A | 11.94–11.96 V, 7.64–8.56 A | **published** as `in0`–`in5`, `curr1`–`curr6` |
| `0x98` | seventh | 12.16 V, 1.16 A | — , 23.9 A mean | unexposed, unexplained |
| `0x9c` | — | all zero | all zero | reserved or unpopulated |
| `0xa0` | eighth | 12.15 V, 0.37 A | 12.10 V, 0.99 A | unexposed, **new** |

### `0x98` — reconfirmed, still unexplained

Across the 583 W burn it read **0.50669 × the pin sum**, against 0.5066 measured on
2026-08-13. A day, a reboot, a major NVIDIA branch change and a fresh burn later, the ratio is
unchanged to four decimal places. Whatever it is, it is not drifting and not noise.

The open question in `../../STATUS.md` is unchanged: why half scale. Nothing found in this
scan answers it, and the cheapest way to settle it is still to boot Windows and see whether
HWiNFO or GPU Tweak III names the channel.

### `0xa0` — an eighth channel, previously unknown

Same `(mV, mA)` form as the pins, and **not** a fixed fraction of anything:

| | 12VHPWR pin sum | `0xa0` | `0xa0` as fraction |
|---|---|---|---|
| idle | 2.32 A | 0.368 A | 15.9% |
| 583 W burn | ~44 A | 0.987 A | 2.2% |

A derived value would hold its fraction; this does not. It is an independent measurement of
something that draws roughly 0.37 A at idle and 1.0 A at full load on a 12.1 V rail — about
12 W at peak.

Leading hypothesis: **the PCIe slot's 12 V rail**, on the grounds that it is the only other
12 V input to the board, its magnitude is right, and the fraction collapsing under load is
exactly what happens when the connector takes over. That is a hypothesis, not a finding — it
is untested, and the same Windows check that would settle `0x98` would settle this too.

Fitting it as a linear function of the connector current gives
`0xa0 ≈ 334 mA + 1.49% × pin_sum`, which reproduces both endpoints. That is a description of
two points, not evidence of a causal relationship; it is recorded so a future capture across
more of the range can refute it.

## `0x60` — eight temperature channels

Eight `u16 BE` values in **0.1 °C** units.

The scale is not assumed. During a 91 s cooldown after the burn, with the card otherwise idle,
`nvidia-smi` core temperature fell 48 → 44 °C while channel 0 fell 50.7 → 46.3 (÷10) — the
same 4 °C, monotonically, sample for sample. A scale of 1 °C or 0.01 °C would put the slope
off by a factor of ten.

| | idle | 583 W burn | rise |
|---|---|---|---|
| ch 0 | 45.3 °C | 49.8 °C | +4.5 |
| ch 1 | 47.6 | 58.2 | +10.6 |
| ch 2 | 47.3 | **63.1** | **+15.8** |
| ch 3 | 46.7 | 57.7 | +11.0 |
| ch 4 | 48.8 | 54.7 | +5.9 |
| ch 5 | 48.1 | 55.6 | +7.5 |
| ch 6 | 42.3 | 50.8 | +8.5 |
| ch 7 | 45.3 | 54.0 | +8.7 |

The burn column is the mean of 158 samples; the idle column is a single pre-burn sample, so
read the rises as approximate. The cooldown series above is the stronger evidence for the
scale, and the ordering held across every sample in both.

They correlate with the pin currents at only r ≈ 0.5, against r ≈ 1.0 for the currents
themselves — thermal mass, exactly as a temperature should behave. They lag on the way up and
decay smoothly on the way down.

**The ordering does not follow the pin currents.** The hottest channel is 2 and the coolest 0
and 6, while the highest-current pin is 5 under any ordering convention. The pattern is
hot-in-the-middle, cool-at-the-ends — spatial, not electrical. So these are **not** per-pin
connector temperatures, and must not be presented as such. Which eight points on the board they
are is unknown.

Nothing here reached anything alarming: 63 °C at 583 W, while the GPU core hit 77 °C.

## `0x15` — a single temperature-like byte

One byte, integer, plausibly °C. Ran 50–69 over the burn and fell 59 → 53 during the cooldown
that took the core from 48 to 44 °C. It tracks load with thermal lag but is not the core
temperature (which peaked at 77 °C) and does not match any `0x60` channel. Source unidentified.

## `0xc0` — three time-in-state counters

Three `u32 BE` fields. The behaviour is unambiguous:

| condition | field 0 | field 1 | field 2 | total |
|---|---|---|---|---|
| idle, GPU flicking between states | +0.797/s | +0.200/s | 0 | 0.997/s |
| 583 W burn | 0 | +0.011/s | +0.988/s | 0.999/s |
| deep idle, 91 s | **+1.000/s** | 0 | 0 | 1.000/s |

**The three rates sum to 1 Hz in every condition.** They are mutually exclusive time-in-state
accumulators at 1 s resolution, and the state is load-dependent: field 0 low, field 2 high.

They are monotonic and they **survive a host reboot** — the counters kept climbing across the
NVIDIA driver upgrade and the restart, so the chip counts whenever the card has power, not
whenever the host is up. At the time of the scan they totalled 13,397,947 s ≈ **155 days** of
powered life, split 47.7 / 72.6 / 34.8 days.

This is a lifetime odometer, and it is the most interesting thing in the scan for any future
work on connector ageing: it dates the card's exposure independently of anything the host
knows. v1 exposes none of it.

## Static regions

Unchanged between passes, across the burn, and across the reboot. Candidates for identity,
calibration or configuration.

```
0x00  00 00 00 00 00 00 00 00 00 03 00 00 00 00 00 01
0x20  15 89 00 00 00 80 ff 32 3c 50 3c 46 50 03 00 00
0x30  00 00 00 00 00 00 00 00 00 00 00 00 00 cc 02 00
0x50  00 00 00 00 00 00 00 00 00 00 00 00 00 01 00 00
0xb0  00 00 00 00 00 00 00 00 00 00 0d c8 00 00 00 00
0xd0  00 00 00 26 00 00 00 0d 00 00 01 45 00 00 00 0d
0xe0  00 00 00 b0 00 00 00 b5 00 00 00 00 00 00 00 00
0xf0  00 00 00 00 00 00 00 00 00 00 00 00 00 1f 01 00
```

`0x09` read `01` before the reboot and `03` after, so `0x00` is configuration rather than a
constant. `0x10` and `0x40` were classified static by a single pass and later moved — `0x15`
is the temperature byte above, and `0x40` differed between two scans. **A region is only
static with respect to the conditions it was observed under**; treat the list as "not observed
to change", not as read-only ROM.

The one suggestive sequence is `0x28`–`0x2c`: `3c 50 3c 46 50` = **60, 80, 60, 70, 80**. Read
as °C that is a plausible alarm or fan-curve ladder, and it sits in the right range for the
`0x60` channels, which idle at 45–49 °C and reached 63 °C under burn. It is a guess. It
concerns temperatures, not pin currents, and confirming it would mean writing to the chip,
which is out of scope here and always will be.

## What is not here

The static space was searched for values shaped like limits — 9500 (the per-pin abort), 9200
(ASUS's Power Detector+ warning point), 600 and 684 (the specified and rated watts), 12000,
11400 and 13200 (12 V and its tolerances), and 57000 (six pins at 9.5 A) — in both byte orders,
at every offset.

**No match anywhere.** Combined with the absence of any register that changed state as the card
went from 0.4 A to 8.56 A per pin, there is no evidence of:

- a programmable current or voltage limit,
- a comparator status or latch bit,
- anything resembling an SMBALERT# source.

This is the finding that matters. `../../GUARD-DESIGN.md` §1 previously argued the guard must
live in userspace on the grounds that *"we have no evidence the chip exposes threshold or
status registers"* — an absence of evidence. It is now a measured absence: the address space
was swept and there is nothing there to marshal. A `curr*_max` in this driver would be a
variable with nothing behind it, exactly as §1 assumed.

## What this does not change

Nothing in the driver. `0x98`, `0xa0`, `0x60`, `0x15` and `0xc0` all stay unexposed, for the
reason already recorded in `CONTRIBUTING.md`: a channel nobody can explain, published as `curr7` or
`temp1`, is a v1 liability rather than a v1 feature. This document exists so that the space is
described rather than unknown, not so that more of it gets published.

Two of them are worth revisiting once someone checks the Windows tooling, because a name would
settle both at once: `0x98` and `0xa0`.
