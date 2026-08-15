# Guard design — alarms and thresholds

Design notes for the **userspace** consumer that `astral-hwmon` exists to feed. Nothing here
changes the driver: v1 publishes twelve read-only channels and enforces nothing, and that
stays true. See the safety rules in `CONTRIBUTING.md`.

Status: design only. Nothing below is implemented.

---

## 1. Why the guard cannot live in the driver

Standard hwmon thresholds (`curr*_max`, `curr*_crit`, `curr*_alarm`) are a marshalling layer
over **chip registers**: userspace writes a limit, the chip compares in hardware, asserts
SMBALERT# or latches a status bit, and the driver reports it and calls
`hwmon_notify_event()`. The comparison happens in silicon, continuously.

This chip has no such registers. That was originally an assumption; it is now a measurement.
The full `0x00`–`0xFF` space was swept read-only on 2026-08-14
(`docs/measurements/2026-08-14/register-map.md`) and searched for values shaped like limits —
9500 and 9200 mA, 600 and 684 W, 12000/11400/13200 mV, in both byte orders, at every offset.
No match anywhere, and no register changed state as the card went from 0.4 A to 8.56 A per pin.
There is no programmable limit, no comparator status bit and no SMBALERT# source to marshal.

So a `curr*_max` here would be a driver variable with nothing behind it, and `curr*_alarm` a
software comparison **evaluated only when someone reads the file**. That is not a guard; it is
a display artefact. Making it protective would mean a permanent polling work item doing I2C
forever inside the kernel.

### The ABI forbids it, and that settles it

The obvious objection is that we now have good criteria — measured on this card, corroborated
by public failure reports — so the driver could simply apply them itself. It cannot, and the
reason is not taste. The hwmon sysfs ABI is explicit:

> *"Alarms are direct indications read from the chips. The drivers do NOT make comparisons of
> readings to thresholds."*
> — [`Documentation/hwmon/sysfs-interface`](https://www.kernel.org/doc/html/latest/hwmon/sysfs-interface.html)

`*_alarm` is a reserved word with a specific meaning: *the silicon caught this, continuously,
whether or not anyone was looking*. A driver that computes the bit produces something a
consumer cannot distinguish from the real thing, which devalues it for every other chip. One
driver's convenience against the whole subsystem's guarantee.

**How a compliant driver does it** — `drivers/nvme/host/hwmon.c`, for a device in this very
machine. The whole alarm implementation:

```c
case hwmon_temp_alarm:
    *val = !!(log->critical_warning & NVME_SMART_CRIT_TEMPERATURE);
```

One bit out of the controller's SMART Critical Warning field. And the limit travels the other
way — `temp1_max` is written *into* the device:

```c
case hwmon_temp_max:
    return nvme_set_temp_thresh(data->ctrl, channel, false, val);
```

The driver is a courier in both directions and never forms an opinion. Note also the
`NVME_QUIRK_NO_TEMP_THRESH_CHANGE` quirk, which drops those limits to `0444` on controllers
that cannot accept a new threshold: the kernel's answer to "the hardware can't do this" is to
**stop offering the knob**, not to emulate it. That is the precedent for our `0444`.

### Demonstrated on this machine

The distinction is observable, so it was observed rather than argued. On the reference
machine's `nct6799` Super I/O, with `beep_enable` at 0, `in0_max` was dropped below the live
Vcore reading and restored:

```
in0_input=888  in0_max=1744  in0_alarm=0    Vcore: 888.00 mV (max = +1.74 V)
in0_input=872  in0_max=800   in0_alarm=1    Vcore: 872.00 mV (max = +0.80 V)  ALARM
in0_input=928  in0_max=1744  in0_alarm=0    Vcore: 928.00 mV (max = +1.74 V)
```

**Nothing read the sensor between the write and the observation.** The limit was written, the
process slept, and the bit was already set. `sensors` contributed nothing but the word: it read
a separate file that the chip had already updated on its own. That reader-independence is the
entire content of the guarantee, and it is precisely what a software comparison in our driver
could not provide — ours would still read 0 at that moment and only "become" 1 when someone
looked.

The same chip also shows the failure mode of publishing a limit carelessly: a dozen of its
channels sit at `alarm=1` permanently because their `max` is 0 and they read 1–3.4 V. The
machine's `/etc/sensors.d/` config `ignore`s them. A limit that is wrong is worse than absent —
the same reasoning as this project's, one layer up.

### Where in-kernel policy does belong — and why not here

The instinct that thresholds belong in the kernel is not wrong in general. The kernel owns
threshold policy in the **thermal subsystem**: trip points, governors, cooling devices,
throttling that happens without asking userspace. That is real in-kernel enforcement of exactly
this shape.

It is also temperature-only. There is no "current zone" equivalent, and our quantity is amps.
So there is no sanctioned in-kernel home for these criteria — not because nobody thought of it,
but because the frameworks that exist do not cover the measurement we have.

### What we would actually gain, and lose

Even setting the ABI aside, the trade is bad. Publishing `curr*_max = 9500` would buy `ALARM`
in `sensors` for the **crudest** part of what we know — a fixed per-pin ampere limit — while
the part with the diagnostic power has no representation in the ABI at all. There is no
`curr1_share_drift`. Section 5's metric is a per-pin deviation from a per-card fingerprint,
gated on total current and evaluated over a rolling window; none of those three ideas is
expressible in a sysfs limit file. We would trade a real signal for a familiar-looking word.

### This conclusion is contingent on a hardware fact, not a principle

Worth stating plainly so it gets revisited rather than inherited. The `CONTRIBUTING.md` rule
says no thresholds because a published limit *"nothing enforces"* misleads userspace. That
rests on the chip having no comparator — which was an assumption until the 2026-08-14 sweep
measured it.

**If a future Astral's sensor chip exposes limit and status registers, publishing `curr*_max`
and `curr*_alarm` becomes the correct thing to do**, and the invariant should be re-litigated
rather than defended. The rule is "don't forge the bit", not "never publish limits".

### Consequences to live with

Because libsensors reads limits from sysfs, `sensors` can never print `ALARM` for this chip and
`sensors -s` has nothing to write. Buying that back would mean writable limit attributes, which
would end the "no write path exists in the code" property that makes the read-only claim
auditable. Not worth it. The guard owns its own configuration.

The interop is worth recovering elsewhere rather than faking. A userspace guard can emit
structured `journald` records, a `node_exporter` textfile metric and a desktop notification —
all consumed by standard tooling, and all able to carry the drift metric that `curr*_alarm`
could not have expressed even if we had been allowed to publish it.

---

## 2. Measured baseline for this card

Reference machine, ROG Astral RTX 5090 OC `0x89E31043`.

**Gaming capture, 2026-08-14** — Heroes of the Storm, 56.9 min, 6834 samples at 2 Hz,
**zero plausibility-gate rejections**.

| | mean | p95 | max |
|---|---|---|---|
| GPU power | 130 W | 179 W | 244 W |
| 12VHPWR total | 11.27 A | — | 18.56 A |
| busiest pin | — | — | 3.34 A |

**Per-pin current share** (loaded samples, total ≥ 6 A):

| pin | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|
| share | 16.12% | 16.49% | 16.60% | 15.85% | **18.06%** | 16.87% |

Even sharing would be 16.67%. **This distribution is the card's fingerprint** and it is
stable: the same shares decoded from raw frames captured on 2026-08-13 — a different day, a
reboot, and a completely different workload (idle vs game) — give 16.10 / 16.65 / 16.44 /
15.66 / 18.12 / 17.04. Agreement within ~0.2 percentage points.

**Imbalance**, loaded samples: max/mean ratio mean **1.084**, p95 1.103, max 1.155.
Pin-to-pin spread mean 274 mA, p95 360 mA, max 520 mA.

**Heavy-load capture, 2026-08-14** — The Witcher 3, DX12 with ray tracing, 33 min, 3966
samples, peak **477 W / 38.2 A total / 6.82 A on the busiest pin**. Also zero rejections.
The high-load band (total ≥ 25 A, n=41) gives the fingerprint where it matters:

| capture | pin1 | pin2 | pin3 | pin4 | pin5 | pin6 |
|---|---|---|---|---|---|---|
| idle, 2026-08-13 | 16.10 | 16.65 | 16.44 | 15.66 | **18.12** | 17.04 |
| gaming ~11 A, HotS | 16.13 | 16.49 | 16.59 | 15.83 | **18.07** | 16.89 |
| heavy ≥ 25 A, Witcher 3 | 16.02 | 16.24 | 16.97 | 16.18 | **17.93** | 16.66 |

Pin 5 dominant throughout, and its share falls slightly as current rises. That is **thermal
self-balancing**: the busiest pin heats most, copper's resistance rises with temperature, and
current shifts away. Confirmed by max/mean being *lowest* at full load — 1.0695 in the ≥ 35 A
band against 1.0960 at idle. Imbalance does not get worse under load on a healthy connector,
which is what makes a fixed alarm threshold viable.

**Only meaningful under load.** At idle the pins carry a few hundred mA and the 20 mA
quantisation step is ~4% of a reading, so shares scatter (max/mean 1.12 at idle vs 1.084
loaded) without the underlying imbalance changing. Every share-based rule below is gated on
total current.

### What the fingerprint implies at full power

Pin 5 carries 18.06% of the total. Total current ≈ P / 12 V. That model predicts 8.46 A on
pin 5 during the 562 W burn of 2026-08-13; the measured value was **8.48 A**. It holds.

Extrapolating with it:

| GPU power | total | pin 5 | vs 9.5 A rating |
|---|---|---|---|
| 244 W (this capture, peak) | 20.3 A | 3.67 A | 39% |
| 562 W (measured burn) | 46.8 A | 8.46 A | 89% |
| **600 W (this card's power limit, default = max)** | 50.0 A | **9.03 A** | **95%** |
| 611 W | 50.9 A | 9.19 A | 97% (ASUS warning point) |
| 631 W | 52.6 A | 9.50 A | 100% |

**At its own maximum power limit this card's busiest pin runs at about 95% of the connector's
per-pin rating**, and ASUS's own 9.2 A warning point would be reached at 611 W — 2% above a
limit the card will not exceed. In other words the card is designed to run right up to the
edge of its own warning, and the connector's 1.14 safety factor (684 W rated vs 600 W
specified) is almost entirely consumed by this card's 8.4% imbalance.

That is the single most important number in this document, and it is why a guard is worth
building: there is no headroom left for a connector that degrades even slightly.

---

## 3. Public reference points

- **9.5 A per pin** — rated current for a 12V-2x6 / Micro-Fit+ power contact.
- **600 W specified / 684 W rated** across 6 power pins = 8.33 A per pin at spec load, safety
  factor **1.14**. The older 8-pin connector's factor was 1.68.
- **9.2 A** — ASUS Power Detector+ warning point, and **11–15 A** — the range documented in
  slow-cook connector failures. Both already recorded in this repo, in the module docstring of
  `src/astral_oracle/load.py`, and used to justify the existing 9.5 A burn abort.
- RTX 40-series and later **tie all six 12V pins together internally**, so the GPU itself
  cannot detect uneven distribution and will not respond to it. Per-pin sensing on the board
  is the only place the imbalance is visible — which is exactly why this card has it and why
  this driver is possible.

### Contact resistance — the quantity that actually fails

- Spec **maximum 5 mΩ** low-level contact resistance for a 12V-2x6 power terminal. Normal
  in-service readings run **5–15 mΩ**, and an increase of **more than 15 mΩ** is the
  conventional failure criterion.
- Degradation is **self-reinforcing**: a contact at 3 mΩ instead of 1.5 mΩ dissipates
  negligible extra power at first, but as current redirects into it, dissipation can rise
  roughly twentyfold over a tiny contact area, which raises its temperature, which raises its
  resistance again. At 50 A a few milliohms is enough to damage insulation.
- Measured on a failed card: **up to 177 mΩ across a 12VHPWR pin, against just over 1 mΩ on a
  healthy reference RTX 5090** — and **over 23 A through a single 16 AWG wire**, against a
  9.5 A rating, because the other five contacts had gone high-resistance and stopped sharing.

### A real imbalance report from this exact sensor

A ROG Astral RTX 5090 owner posted GPU Tweak per-pin readings of roughly **6.9 A on three pins
and 8.2 A on the other three** — about 45 A total. Diagnosis in the thread was lower contact
resistance on the higher three; the fix was **replacing the cable**, after which the
distribution normalised.

Those numbers are worth working out, because they are inconvenient. Shares are 15.2% and
18.1%, and **max/mean is 1.086 — statistically indistinguishable from this healthy card's
1.084**. A case the community judged bad enough to replace a cable over would not have tripped
any absolute imbalance threshold that this card also passes. That single data point is why §5
is built on drift from a per-card baseline rather than on an absolute ratio.

The tiered structure in §4 (sustained-breach windows, limit-before-shutdown, clear-and-release
timings) follows the approach taken by the `12vhpwr-guard` project, credited in `README.md` as
this project's inspiration. Design shape only — no code, and every number above traces to a
public specification or to a measurement in `docs/measurements/`.

---

## 4. Proposed alarm tiers

Absolute per-pin current. Poll at **2 Hz** — the driver caches for 200 ms and publishes that
as `update_interval`, so 5 Hz is the ceiling and 2 Hz leaves headroom.

| tier | trigger, any single pin | response |
|---|---|---|
| 1 — elevated | ≥ 9.5 A sustained 15 s | Cap the GPU (clock floor + minimum power limit). Escalate to shutdown if still ≥ 9.5 A after 10 s. |
| 2 — critical | ≥ 13 A sustained 3 s | Cap immediately. Shutdown if still ≥ 9.5 A after 5 s. |
| 3 — catastrophic | ≥ 16 A on 2 consecutive samples | Cap and shut down immediately. |

Two consecutive samples for tier 3 so that no single bad read can trigger it. Readings above
`ASTRAL_MA_MAX` (30 A) never reach this logic — the driver has already refused them.

**Release:** hold the cap for at least 120 s, and require every pin below threshold for 60 s.
Current collapses the instant clocks do, so releasing on current alone would flap. Restore the
user's exact prior power limit, not the card default. A tier firing again within 600 s of a
release is a persistent fault, not a transient: cap and shut down.

Note what tier 1 means *for this card specifically*: from §2, pin 5 reaches 9.5 A at about
631 W. So tier 1 is at or just above stock full load — appropriate as a warning, but it does
mean an overclocked card could sit near it legitimately. Which is the motivation for §5.

---

## 5. Fingerprint drift — the leading indicator

Absolute current is a **lagging** signal. The 12VHPWR failure mode is contact resistance
rising on one pin, which pushes its current onto the others; by the time any single pin
reaches 9.5 A the fault is well developed.

Pins in parallel across one rail satisfy `I_pin × R_pin ≈ constant`, so **the current split is
an inverse-resistance map of the six contacts**, and it is measured far more precisely than
the voltage side: 20 mA on ~2000 mA is ~1%, whereas the pin-to-pin voltage differences are
only ~16 mV against an 8 mV quantisation step.

**Metric:** for each pin, `|share_now − share_baseline|`, in percentage points, evaluated over
a rolling window of at least 60 s of samples **within the same load band** as the baseline.

### What a drift of N points means physically

For five pins at resistance `R` and one at `kR`, that pin's share is `1/(1 + 5k)`. Inverting
gives the resistance change behind any observed drift:

| pin's share | drift | its contact resistance | reference point |
|---|---|---|---|
| 15.67% | −1.0 pp | +7.6% | **notice** |
| 14.67% | −2.0 pp | +16% | **warn** |
| 13.67% | −3.0 pp | +26% | **act** |
| 9.09% | −7.6 pp | ×2 | |
| 4.76% | −11.9 pp | ×4 | industry failure criterion (5 mΩ → 20 mΩ) |
| 0.11% | −16.6 pp | ×177 | the measured failed card |

A degrading pin's share *falls* and the other five rise by roughly a fifth of the drop each,
so both directions must be watched — the pin carrying 23 A in the failed card above was the
*healthy* one, left carrying everything after the others went high-resistance.

The headline: **a 1 pp drift is a 7.6% change in one contact's resistance.** The conventional
failure criterion is a 300% change. This metric therefore has enormous lead time — provided
the baseline is honest.

### Thresholds

Observed drift across a day, a reboot and three workloads: **≤ 0.2 pp**, which is a ~1.5%
resistance change — the noise floor of the method on this hardware.

| level | drift | meaning and action |
|---|---|---|
| notice | ≥ 1.0 pp | 5× the noise floor. Log and surface. Not a fault. |
| warn | ≥ 2.0 pp | Contact condition has measurably changed. Inspect and reseat. |
| act | ≥ 3.0 pp **and** any pin ≥ 7 A | Treat as tier 1. |

**Compare like with like.** Share moves ~0.2 pp between idle and full load through thermal
self-balancing (§2) — the same size as the noise floor — so a reading must be compared against
a baseline from the same load band, never across bands.

**Reseating the cable legitimately changes the fingerprint,** so drift prompts *re-baselining*,
never a shutdown on its own. That is why the "act" row also requires real current. Baselines
should be re-recorded deliberately, timestamped and kept: the trend over months is the
diagnostic, not any single reading.

### Absolute imbalance is a backstop, not an early warning

An earlier draft proposed max/mean ≥ 1.25 as a baseline-free companion. The real-world Astral
report in §3 kills that idea as an *early* warning: a distribution bad enough to warrant a
cable replacement measured 1.086, against this healthy card's 1.084. **Absolute ratio cannot
separate a bad cable from a normal one**, because healthy cards differ from each other by more
than a developing fault does at the stage you want to catch it.

Keep max/mean ≥ **1.25** only as a catastrophic backstop for a card with no baseline yet — it
would have caught the 23 A failure (ratio ≈ 2.8) and nothing subtler. Anything genuinely early
needs the per-card fingerprint.

---

## 6. Fail-safe rules

1. **Losing sight of the current is a fault, not silence.** When a frame fails the
   plausibility gate every channel returns `-EIO` together. A guard that treats "no reading"
   as "no alarm" is worse than none. Sustained `-EIO` must cap the GPU. Frequency observed in
   6834 consecutive samples under real load: **zero**.
2. **Gate every share metric on total current.** At idle, quantisation noise alone produces a
   max/mean of 1.12. This is precisely how the first version of the session monitor produced
   false alarms.
3. **Check staleness.** Read `update_interval` rather than assuming; act on how old a reading
   can be, not on how recently it was read.
4. **The driver may vanish.** `rmmod nvidia`, a GPU reset, or a driver bug removes the hwmon
   node. Absence of the chip is a fault state.

---

## 7. Gaps before this can be built

- ~~**Thin coverage above 38 A.**~~ **Closed 2026-08-14.** `burn-2hz.csv` reached **581 W /
  48.26 A total / 8.56 A on the busiest pin**, 235 samples above 35 A. The §7b shares projected
  from the 25 A band land within **0.23 pp** of the shares measured at 48 A — at the noise
  floor, across a 1.9× extrapolation. The fingerprint predicts full-power behaviour, which is
  the assumption the whole drift metric rests on, and it is no longer an assumption.
- **One card, one seating.** The fingerprint is a property of this card and this connection.
  Nothing here should be generalised to another Astral without its own baseline.
- **Mitigation is unproven on Linux.** Capping via `nvidia-smi -lgc` / `-pl` needs testing:
  how fast the clocks actually drop, and whether the power limit restores exactly. This is now
  the largest remaining gap: the detection side is measured end to end, the response side is
  not tested at all.
- **The `0x98` block is still unexplained** (`docs/STATUS.md`). Reconfirmed at 0.50669 × the
  pin sum under the 583 W burn, against 0.5066 a day earlier — so it is stable enough to use.
  If it turns out to be an independent total-current measurement, it becomes a cross-check
  against the sum of the six pins — a way to catch the sensor itself lying.
- **Board temperatures exist but are not understood.** The register scan found eight channels
  at `0x60` in 0.1 °C units (45–49 °C idle, 63 °C peak under the burn). Temperature is the
  quantity that actually kills these connectors, so a guard that could read a *connector*
  temperature would be strictly better than one inferring from current alone. These are not
  it — their ordering is spatial, not per-pin — but identifying them is the highest-value
  unknown left in the register space.


---

## 7b. Concrete thresholds for the reference card

Instantiates §4 and §5 for `0x89E31043` with the measured fingerprint. **These numbers are
per-card.** Another Astral needs its own baseline; the method transfers, the constants do not.

### Baseline (heavy-load band, total >= 25 A)

| pin | share | at 600 W (50 A) | headroom to 9.5 A |
|---|---|---|---|
| 1 | 16.02% | 8.01 A | 19% |
| 2 | 16.24% | 8.12 A | 17% |
| 3 | 16.97% | 8.49 A | 12% |
| 4 | 16.18% | 8.09 A | 17% |
| 5 | **17.93%** | **8.97 A** | **5.9%** |
| 6 | 16.66% | 8.33 A | 14% |

### Verified against a 581 W burn

The table above was projected from a 25 A band. `burn-2hz.csv` then measured the real thing at
**48.26 A / 581 W**:

| pin | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|
| projected share | 16.02% | 16.24% | 16.97% | 16.18% | **17.93%** | 16.66% |
| measured at 48 A | 15.85% | 16.23% | 17.17% | 16.41% | **17.75%** | 16.59% |
| error (pp) | -0.17 | -0.01 | +0.20 | +0.23 | -0.18 | -0.07 |
| measured peak | 7.64 A | 7.84 A | 8.30 A | 7.92 A | **8.56 A** | 8.02 A |

Every error is at or inside the 0.2 pp noise floor. Two consequences:

- **The projection method is sound.** A baseline collected at gaming loads predicts full-power
  shares, so a user need not run a burn to calibrate a guard.
- **Headroom at full power is thin, and now measured rather than projected.** Pin 5 stays the
  busiest, though pin 3 gains on it as load rises — thermal self-balancing again. Pin 5 reached
  **8.56 A at 581 W**: 90% of the 9.5 A rating, 93% of ASUS's 9.2 A warning point. The burn did
  not saturate the 600 W limit, so scaling the measured share to 600 W (49.8 A) puts pin 5 at
  **8.84 A — 7.5% below the rating**, against the 5.9% the earlier projection implied. Slightly
  better than feared, still thin. This is the argument for drift detection over absolute
  limits, restated by measurement: a healthy connector at full load already sits close enough
  to the tier-1 threshold that an absolute limit has little room to be wrong in.

### The two alarms agree, from independent directions

For pin 5 to reach the 9.5 A tier-1 threshold at 600 W its share must rise from 17.93% to
19.0% - a drift of **1.07 pp**, which is the "notice" threshold of §5 derived independently
from the measured noise floor. One number comes from the connector specification, the other
from this card's repeatability. They coincide.

They are not redundant: absolute current only fires near full power, while drift fires at any
load above the gating current. A fault developing on a machine that never runs at 600 W would
be invisible to the first and obvious to the second.

### Preferred formulation: expected vs actual

Rather than a fixed per-pin ampere limit, compare each pin against what the fingerprint
predicts for the current total:

    expected_i = share_baseline_i x total_current
    deviation_i = (actual_i - expected_i) / expected_i

This is load-independent, so it works at 100 W and at 600 W with the same constants.
Equivalences on a 16.67% nominal share:

| drift | relative deviation | level |
|---|---|---|
| 0.2 pp | 1.2% | measured noise floor |
| 1.0 pp | 6% | notice |
| 2.0 pp | 12% | warn |
| 3.0 pp | 18% | act (with any pin >= 7 A) |

### Gating and windows

- Share and deviation metrics: only on samples with **total >= 10 A**. Below that,
  quantisation dominates (max/mean 1.096 at idle against 1.070 at full load).
- Compare within the same load band: share moves ~0.2 pp from idle to 38 A through thermal
  self-balancing, which is the same size as the noise floor.
- Rolling window of at least **60 s** of gated samples before any drift conclusion.
- Absolute tiers from §4 need no gating - they are absolute for a reason.

### Sampling

2 Hz. The driver caches for 200 ms and publishes that as `update_interval`, so 5 Hz is the
ceiling; 2 Hz leaves headroom and matched the captures this baseline came from.

---

## 8. Sources

Public specifications and reporting behind §3. Hardware facts in this repository are traced to
one of these or to `docs/measurements/`; none are taken from another project's source.

Kernel interface, behind §1:

- [`Documentation/hwmon/sysfs-interface`](https://www.kernel.org/doc/html/latest/hwmon/sysfs-interface.html)
  — "Alarms are direct indications read from the chips. The drivers do NOT make comparisons of
  readings to thresholds."
- [`drivers/nvme/host/hwmon.c`](https://github.com/torvalds/linux/blob/master/drivers/nvme/host/hwmon.c)
  — a compliant driver: alarm from a hardware status bit, limits written into the device, and
  `0444` where the hardware cannot accept a threshold.
- [The Linux hardware monitoring kernel API](https://docs.kernel.org/hwmon/hwmon-kernel-api.html)
  and [Phoronix on hwmon notification support](https://www.phoronix.com/news/Linux-5.8-HWMON-Notifications)
  — `hwmon_notify_event()`, the push path for drivers that *have* an event to report.

Community and vendor reporting on this exact sensor:

- [ASUS ROG forum — Astral Power Detector+ 12VHPWR warning after sleep](https://rog-forum.asus.com/t5/nvidia-graphics-cards/asus-geforce-rtx-5090-astral-power-detector-12vhpwr-warning/td-p/1090739)
  — Silent_Scone, an ASUS Super Moderator, gives the alert's trigger conditions: *"This message
  generally appears when the conditions are met: the amperage exceeding 9.2A on one pin, or no
  amperage is detected."* In the post marked as the accepted solution, the same moderator
  confirms the post-resume warning is a reproduced software bug: *"The message appearing after
  resuming from sleep states is not directly related to vBIOS. RD has replicated the sleep
  state behaviour and will be resolved in a future update."* Reporters note HWiNFO showed
  normal values at the same moment.
- [ASUS — ROG Equalizer Cable: why per-pin current balance is not the whole story](https://rog-forum.asus.com/t5/technologies-explained/rog-equalizer-cable-why-per-pin-current-balance-is-not-the-whole/ba-p/1151871)
  — the vendor's own caution that current balance is a proxy: 146 °C on a standard cable
  against 73.4 °C on theirs under the same forced imbalance.
- [Thermal Grizzly — WireView Pro II firmware update](https://www.thermal-grizzly.com/en/blog/new-software-and-first-firmware-update-for-wireview-pro-ii)
  — a shipping monitor's imbalance alarm, revised in the field from 30% at >=5 A to 40% at
  >=6 A per pin.
- [LACT issue #906](https://github.com/ilya-zlobintsev/LACT/issues/906) — an independent public
  description of this chip's protocol (`0x2b`, register `0x80`, 6x(u16 BE mV, u16 BE mA),
  reverse pin order) that matches what this project established by measurement, and names the
  part as ITE IT8915FN-56.

12VHPWR and connector failure data:

- [12VHPWR — Wikipedia](https://en.wikipedia.org/wiki/12VHPWR) — 9.5 A per pin, 600 W
  specified against 684 W rated, the 1.14 safety factor, and the internally-tied 12V pins.
- [Molex Micro-Fit+ connector system](https://www.mouser.com/pdfDocs/molex-micro-fit-pcie-prod-pres.pdf)
  — terminal system used for 12V-2x6.
- [Amphenol Minitek Pwr CEM-5 12V-2x6 product spec](https://cdn.amphenol-cs.com/media/wysiwyg/files/documentation/gs-12-1706.pdf)
  — 5 mΩ maximum low-level contact resistance for power terminals.
- [ASUS — How GPU Tweak's Power Detector+ alerts you to abnormal current](https://rog.asus.com/articles/guides/how-gpu-tweaks-power-detector-alerts-you-to-abnormal-current-on-your-rog-astral-graphics-card/)
  — the 9.2 A per-pin warning point, from the vendor.
- [igor'sLAB — smart load balancing vs monitoring with a hard cut](https://www.igorslab.de/en/smart-load-balancing-against-aging-or-monitoring-with-hard-cut-which-is-the-better-solution-for-12vhpwr-and-12v-2x6-connector/4/)
  — the self-reinforcing resistance/temperature/current interaction.
- [Notebookcheck — der8auer repairs a melted RTX 4090 connector](https://www.notebookcheck.net/Der8auer-repairs-melted-RTX-4090-connector-challenges-Nvidia-s-12VHPWR-safety-claims-again.1057324.0.html)
  and [HotHardware — RTX 5090 FE connectors melting](https://hothardware.com/news/rtx-5090-fe-power-connectors-melt)
  — 177 mΩ against ~1 mΩ, and over 23 A on a single wire.
- [Tom's Hardware forum — "Is this 12VHPWR amperage distribution okay?"](https://forums.tomshardware.com/threads/is-this-12vhpwr-amperage-distribution-okay.3891281/)
  — an Astral RTX 5090 owner's 6.9 A / 8.2 A split, resolved by replacing the cable.
