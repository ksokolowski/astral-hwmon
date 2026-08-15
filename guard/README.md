# astral-guard

The userspace consumer the driver deliberately does not contain. `astral-hwmon` publishes
twelve numbers and makes no judgement about them; `astral-guard` is where the judgement
lives, because the hwmon ABI says an `*_alarm` file is a bit read from a chip and this chip
has no such bit — measured, not assumed
(`docs/measurements/2026-08-14/register-map.md`). The full reasoning is in
`docs/GUARD-DESIGN.md` §1.

It is a one-shot C11 program with no dependency beyond POSIX.1-2008. It reads
`/sys/class/hwmon`, applies the rules below to a short burst of samples, prints a verdict and
exits with a monitoring-plugin code: **0** OK, **1** WARNING, **2** CRITICAL, **3** UNKNOWN.
It needs no privileges and knows nothing about any init system — `examples/` holds systemd,
cron and Icinga snippets, none of which is installed.

```
make guard          # build guard/astral-guard
make test-guard     # the rule engine and the sysfs reader, no hardware
make install-guard  # honours DESTDIR and PREFIX
man 1 astral-guard  # the rules, the exit codes and the limitations
```

## The rules

Every rule is evaluated per sample and reported only if it holds in **all** samples of the
run. A single frame never raises anything; that is R6, and it is what makes the tool safe to
run every five minutes on a machine whose load is spiky.

| | rule | trips at | source |
|---|---|---|---|
| R1 | per-pin current | WARN 9.2 A, CRIT 9.5 A | ASUS Power Detector+; the 12V-2x6 per-pin rating |
| R2 | open pin | one pin under 0.5 A with ≥10 A flowing | ASUS Power Detector+ warns at "0 amps" too |
| R3 | min/max imbalance | NOTE 85%, WARN 70%, CRIT 60%, with ≥20 A flowing | Thermal Grizzly's WireView Pro II, original and field-revised |
| R4 | pin voltage floor | WARN 11.4 V, CRIT 11.0 V | contact resistance shows as sag before it shows as heat |
| R6 | persistence | a finding must hold in every sample | — |

R3's NOTE tier is advisory: it appears in both outputs and **cannot** change the exit code.
It exists so the one publicly documented real Astral imbalance — Tom's Hardware, min/max
0.841 — reaches a human without paging anyone.

`min/max` rather than `max/mean` is not a style choice. For a single degraded pin `max/mean`
moves the *wrong way*: a pin at 1.2× the others gives 1.029, and at 1.5× gives 1.059, both
*below* the healthy reference card's own 1.064. A rule built on it would go quiet exactly as
the fault developed.

## What `make test-guard` covers

- Every rule against `tests/data/guard-cases.jsonl`, a corpus where each case names the
  measurement or public report it came from, including the healthy idle and full-load frames
  captured off the reference card.
- That severity is ranked rather than compared numerically — UNKNOWN is 3 and CRITICAL is 2,
  so a raw comparison would let one card's unreadable sensor hide another card's overcurrent.
- That a NOTE is recorded in the findings and still leaves the level at OK.
- Discovery by hwmon `name`, not by index: `hwmon7` is not a stable address, and a tree
  containing only an `nct6799` must yield nothing rather than something.
- The **asymmetric numbering** — `in0`–`in5` against `curr1`–`curr6` — with distinct values
  per channel, so a reader that assumed `in1`–`in6` fails rather than reading a neighbour.
- That a partial read fails the whole sample. A missing channel must not leave a pin at zero,
  which R2 would then report as a dead contact. Same reasoning as the driver's plausibility
  gate: losing sight of a pin must not look like a healthy reading of it.
- The tree vanishing between discovery and read — the `rmmod nvidia` case — without crashing.

`tests/unit/test_guard_cli.py` covers the process boundary the C tier cannot: it runs the
built binary with `--sysfs-root` against a `pytest` tmp tree and checks the exit code the
caller actually sees. `make qa` builds the binary first so it cannot silently skip; `make
test` does not, so the unit tier stays free of a compiler.

The fake trees carry `name`, `update_interval` and the real numbering because fixture
fidelity is not optional here — an over-simplified fake sysfs once made a unit test in this
repo agree with a bug that real hardware rejected.

## Things it deliberately does not check

- **The threshold values.** Tests use them symbolically, exactly as the native tier treats
  `ASTRAL_MV_MIN`. Tuning WARN must not fail the suite. What *is* pinned is the ordering
  (NOTE > WARN > CRIT) and that gating exists at all.
- **Whether the connector is actually safe.** We measure current; the hazard is contact
  heating. A green verdict is not a temperature claim.
- **Post-resume behaviour, beyond S3 on one machine.** The corpus proves the *logic* rejects a
  transient. What this card reports on wake was measured separately on 2026-08-15
  (`docs/measurements/2026-08-15/`) — no false over-current, `-EIO` rather than a decodable
  frame during the window, OK at wake — but that is a manual measurement, not something
  `make test-guard` can run, and it covers S3 only.

## Verifying the tests can fail

Each of these was introduced and confirmed to break the run:

- invert the `min/max` comparison (`lo * 100 < hi * pct` → `>`) — six checks fail
- accept a finding present in 1 of N samples instead of all N
- set `open_gate_ma` to 0, so idle machines report six open pins
- set `ratio_gate_ma` to 0, so a healthy idle card's 0.656 min/max is CRITICAL
- change R1's `>=` boundary to `>`, so a pin at exactly the 9.5 A rating passes
- set CRIT looser than WARN (`pin_crit_ma` 9000 against `pin_warn_ma` 9200)
- read `curr%d_input` with `pin` instead of `pin + 1`
- return OK instead of UNKNOWN when no chip is found — caught by the Python CLI tier
- drift `GUARD_CHIP_NAME` from the driver's `ASTRAL_CHIP_NAME` — caught by the native tier

One trap worth recording, because the native tier's README records its twin. *Deleting* the
open-pin gate rather than zeroing it does not fail a test: it makes `total_ma()` unused and
the build fails on `-Werror`. A mutation that breaks the build proves nothing about the
tests. Mutate the value, not the code that uses it.

## Layout

- `astral_guard.h` — the whole interface, including the thresholds and `GUARD_CHIP_NAME`
- `guard_eval.c` — the rules. Pure: samples in, verdict out, no I/O, integer arithmetic only
  so no locale can put a comma in the JSON
- `guard_sysfs.c` — discovery and reading. The only file that touches the filesystem
- `guard_report.c` — text and JSON, the latter echoing the thresholds the verdict used
- `guard_main.c` — argument parsing and orchestration

`GUARD_CHIP_NAME` duplicates the driver's `ASTRAL_CHIP_NAME` because `driver/astral.h`
includes `<linux/i2c.h>` and cannot be used from userspace. `tests/native/test_names.c`
asserts the two strings still match. That duplication is *not* an instance of the
independent-implementation rule in `CONTRIBUTING.md`: the guard is a consumer of the driver's
sysfs ABI, not a third implementation of the decode, and nothing here is meant to
cross-check `astral_regs.c`.
