# Field report: a degrading 12VHPWR cable

Data from the owner of a ROG Astral LC RTX 5090 OC (`ROG-ASTRAL-LC-RTX5090-O32G-GAMING`,
`0x89EC1043`), reported in [issue #3](https://github.com/ksokolowski/astral-hwmon/issues/3)
and published here with their permission. Ubuntu 24.04.4, kernel 6.17.0-1033-oem, NVIDIA
580.178.04. Netdata logged the driver's six currents through libsensors every 2 s. This is not
the reference machine, and none of it was measured here.

## Files

| file | contents |
|---|---|
| `netdata-currents-2s.csv` | Netdata raw export, unmodified: 542 rows, Unix time in ms, then pins 1–6 in A |
| `netdata-currents.png` | the owner's Netdata chart of the same log |

## Cross-check against a clamp meter

Before this log, the owner measured each of the six wires with a Fluke 301D DC clamp meter
and found the readings agree with the driver. This is the first check of the driver against
an instrument that does not read the chip. The owner reported agreement without giving the
clamp readings, so it confirms scale and pin mapping but does not set a tolerance.

## What the log shows

Three loads at about 37 A (469 W in `nvidia-smi`): a pulsing load for five minutes, then two
steady ones.

- Pin 1 sinks under steady load. It starts each run at about 13.7% of the total and falls to
  about 10% within three minutes, against an even share of 16.7%. In the first steady run it
  went from 4.97 A to 3.78 A while the total held at 37 A. A contact whose resistance rises as
  it warms behaves this way.
- 94 s into the last run, the owner pressed the pin 1 wire into the PSU-side connector.
  Within 4 s pin 1 rose from 3.83 A to 5.62 A. Over the next few seconds pins 3, 5 and 6 each
  dropped by 0.3–0.6 A, and the total did not change. Pin 1 then began sinking again and
  reached 5.40 A within 45 s.
- Pin 1 is not the only weak contact. With it pressed in, pins 1, 2 and 4 carried 4.9–5.8 A
  against 6.6–7.2 A on pins 3, 5 and 6, and min/max still read 0.695. Pin 2 was also the low
  pin in the owner's first report, before anyone handled the wires.
- The six wires run in parallel, so whatever current one pin loses, the others carry. Nothing
  in the log needs active balancing on the card to explain this, and the same sharing pushes
  the healthy pins harder as a contact degrades.

## What astral-guard would have said

Applying R3 (min/max with at least 20 A flowing) to each loaded sample on its own gives 222
CRITICAL and 111 WARN out of 333, and none OK. Every run reached CRITICAL within two minutes.
The worst sample is 0.452: 3.33 A on pin 1 against 7.36 A on pin 5. No pin passed 7.6 A, so
R1 (9.2 A) stayed silent for the whole log.

The guard requires a finding in every sample of a run, while this is a count of 2 s samples.
The CRITICAL stretches last minutes, so the verdict is the same either way.

The worst sample and the frame with pin 1 pressed in are cases in
`tests/data/guard-cases.jsonl`. The log has no voltages, so both use the voltages from the
owner's first report.
