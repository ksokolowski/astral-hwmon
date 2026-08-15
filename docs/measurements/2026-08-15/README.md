# Suspend/resume — 2026-08-15

`astral-hwmon` 0.2.0 installed via DKMS, kernel 7.0.0-29-generic, NVIDIA 610.43.02,
ROG Astral RTX 5090 OC `0x89E31043`, chip on `i2c-4` at `0x2b`. Sleep state `deep` (S3),
`nvidia-suspend.service` / `nvidia-resume.service` enabled, `PreserveVideoMemoryAllocations=1`.

Two cycles, `systemctl suspend` with a manual wake. `resume_logger.py` sampled all twelve
channels at 10 Hz throughout, re-discovering the node by hwmon `name` on every tick so a
teardown and re-attach would show as a changed index rather than a permanent failure.
`astral-guard --json --samples 3` ran before the suspend and at wake, +5 s and +30 s.

`CLOCK_MONOTONIC` does not advance across S3, which is what makes the raw logs readable:
the one line where wall time jumps and monotonic does not is the sleep.

## Files

| file | contents |
|---|---|
| `resume_logger.py` | the sampler, as run |
| `suspend-cycle1.log` | 588 samples, 13.0 s asleep |
| `suspend-cycle2.log` | 725 samples, 32.5 s asleep |

Log format: `wall mono state detail`, state one of `OK` (twelve values follow), `EIO`
(node present, read failed) or `NONODE` (no chip named `astral12vhpwr`).

## Results

Identical across both cycles:

| | cycle 1 | cycle 2 |
|---|---|---|
| time asleep | 13.0 s | 32.5 s |
| `NONODE` samples | 0 | 0 |
| `EIO` samples | 14 | 14 |
| hwmon index seen | `hwmon10` throughout | `hwmon10` throughout |
| first good frame after wake | +0.10 s | +0.10 s |
| currents in that frame (mA) | 680 700 700 660 780 720 | 680 700 680 640 760 700 |
| `astral-guard` at wake / +5 s / +30 s | OK / OK / OK | OK / OK / OK |

Timeline of cycle 1, relative to `systemctl suspend`:

```
+1.19 s   last good frame       12184 mV, 440-480 mA per pin
+1.29 s   first -EIO            the bus goes down as the system does
          ... 13.0 s asleep ...
 wake     still -EIO            one sample
+0.10 s   good frame            12176 mV, 660-780 mA per pin
```

### 1. No false over-current on resume

The reason for running this at all. ASUS documents its own Power Detector+ reporting spurious
0 A and over-current readings immediately after wake
([ROG forum thread](https://rog-forum.asus.com/t5/nvidia-graphics-cards/asus-geforce-rtx-5090-astral-power-detector-12vhpwr-warning/td-p/1090739)). Nothing of the kind
appears here. The first readable frame is 660–780 mA per pin — ordinary idle plus the desktop
repainting — rising to about 1 A over the next second and settling. No spike, no zero, no
finding from any of the four guard rules at any point.

### 2. Failure is `-EIO`, never a plausible-looking zero

Every sample from the bus going down to it coming back returned `-EIO`: 14 of them per cycle,
the count set by the ~1.3 s of going-down at 10 Hz plus one sample after wake. Not one
returned a decodable frame. This is the property the driver's plausibility gate exists for —
a read that cannot see the pins must fail loudly rather than decode as a healthy 0 A on all
six — and it is also why `astral-guard` maps a failed read to UNKNOWN rather than OK.

Which of the two rejection paths produced the `-EIO` is **not** determined by this measurement.
The driver returns `-EPROTO` for an implausible frame and passes transport errors through, but
`astral_read()` maps both to `-EIO` for userspace. The transport is the likelier source, since
a plausibility failure requires a *successful* transfer returning nonsense and the adapter's
parent was suspended — but that is an inference. Settling it needs a tracepoint, and nothing
depends on the answer: both paths are the designed response.

### 3. The teardown path is not exercised by suspend/resume

This contradicts what `STATUS.md` predicted before the measurement, and is the reason to
write it down. The expectation was that resume would exercise `BUS_NOTIFY_DEL_DEVICE` and the
same re-attach path that runs at boot. It does neither:

- the hwmon node stayed `hwmon10` across both cycles, and no sample found it missing;
- the kernel log contains **no** `astral-hwmon` line at all across either cycle — only
  `PM: suspend entry (deep)`, `nvidia 0000:01:00.0: Enabling HDA controller` and
  `PM: suspend exit`.

NVIDIA keeps its i2c adapters registered across S3. Our client is never freed, so there is
nothing to detach and nothing to re-attach; the driver simply returns `-EIO` for the reads
that land in the window and resumes serving frames afterwards, silently, which is correct.

`BUS_NOTIFY_DEL_DEVICE` and the re-attach path therefore remain covered only by
`rmmod nvidia` / `modprobe nvidia`. That could not be run here: with a desktop session on the
card, `nvidia` had 534 references. It needs a console-only boot, or a second machine.

## Method notes

- The RTC wake alarm needs root and `sudo` had no terminal in the session that ran this, so
  both wakes were manual. It makes no difference: the analysis keys off the wall-versus-
  monotonic jump, not a known sleep duration.
- Cycle 1's logger was still running when its output file was moved aside for cycle 2. The
  open descriptor followed the inode, so the archived `suspend-cycle1.log` is truncated at
  wall `1786823300`, before cycle 2 began. Both logs are otherwise verbatim.
