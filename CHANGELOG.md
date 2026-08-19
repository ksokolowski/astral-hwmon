# Changelog

Notable changes per release. The version lives in `dkms.conf` and is mirrored by hand into
`MODULE_VERSION()` and `pyproject.toml`; `astral-guard` takes its `--version` string from
`dkms.conf` via the Makefile, so it cannot drift.

## 0.3.0 — 2026-08-19

Adds the first thing in this repository that *acts* on a reading. No logic changed in either
program: the only difference in `driver/` and `guard/` is the version string they report.
What is new lives entirely in `examples/`, installs nothing, and is off until somebody arms it
deliberately.

### Opt-in protection

- `examples/astral-guard-poweroff.service` turns a CRITICAL verdict into a poweroff, after a
  broadcast warning and a 60-second grace period that `systemctl stop` cancels. Arming it
  takes two edits in `astral-guard.service`, and reading the file first is the point of
  splitting it that way.
- The unit refuses to act unless `MONITOR_EXIT_STATUS` is exactly 2. `OnFailure=` fires on any
  failure of the check unit, not only on a CRITICAL verdict — a guard binary that is not where
  the unit says it is exits 203, which without the interlock would power the machine off every
  timer tick with a perfectly healthy connector. With no trigger metadata at all it declines to
  act, so it fails towards staying powered on.
- `examples/astral-guard-notify.sh` puts the warning in front of a desktop user, for the case
  `wall(1)` cannot reach: somebody full-screen in a game with no terminal open. It speaks
  `org.freedesktop.Notifications`, so GNOME, KDE Plasma, XFCE, Cinnamon, MATE and Budgie all
  show it on Wayland and X11 alike. It is called with a leading `-`, so a machine without
  `notify-send` still protects itself.
- Verified by driving the real units against a fake sysfs tree: the poweroff fires on exit 2
  and not on 0, 1, 3 or 203, re-arms after a cancellation, and a cancelled grace period never
  reaches the action.

**This is not a substitute for judgement.** Every threshold behind a CRITICAL verdict is
reasoned from ASUS and Thermal Grizzly's published numbers, not observed on a connector that
was actually failing. Arming an automatic poweroff means accepting those numbers on a machine
whose card may be one of the six variants nobody has ever read.

### Fixed

- All `examples/` referenced `/usr/bin/astral-guard`, while `make install-guard` uses
  `PREFIX=/usr/local` and installs to `/usr/local/bin`. Following the README produced examples
  that could not run — and, once a poweroff unit existed, that failure mode was the 203 above.
- Documentation that had drifted from the code: the test suite described as three tiers rather
  than four, and several counts left over from the days when one card had been read on Linux
  rather than two.

### Hardware

- `1043:89EC` (ROG Astral RTX 5090 LC) is Linux-verified, confirmed by an owner report in
  issue #1 — the first evidence that the sensor block is shared across the family rather than
  assumed to be. Six of the eight ids remain unverified.

## 0.2.1 — 2026-08-16

First public release. Earlier versions ran on the author's own machine and were never
published; what they contained is summarised at the end for context.

### The driver

- Six per-pin 12VHPWR voltages and currents from the card's ITE IT8915FN, published as
  standard hwmon channels (`in0`–`in5`, `curr1`–`curr6`) plus `update_interval`. Anything
  built on libsensors picks them up with no bespoke client.
- Autoloaded by PCI modalias, so udev loads it only on machines that have a supported card.
- A four-stage probe — card present, adapters up, chip answering, attach — each stage reported
  to the kernel log, so a machine that never produces sensors says why instead of failing
  silently. Stages 2–3 retry on a workqueue; `modprobe astral-hwmon && sensors` still works
  without a race.
- **Read-only by construction.** No write callback exists, `is_visible()` returns `0444`
  unconditionally, and no module parameter changes that.
- **No thresholds and no computed alarms.** The hwmon ABI reserves `*_alarm` for status bits
  read from the chip, and this chip has no comparator — established by sweeping its whole
  address space, not assumed. The reasoning, the ABI citation and the `nvme` precedent are in
  `docs/GUARD-DESIGN.md` §1.
- A plausibility gate that rejects the all-zeros frame a naive multi-byte read returns on this
  bus, with a deliberately loose 6 V floor so a sagging connector stays visible.

### astral-guard

The userspace judgement layer the driver deliberately does not contain: a one-shot C11
program needing nothing beyond POSIX.1-2008, no privileges and no init system. Exit codes
follow the monitoring-plugin convention (0 OK, 1 WARNING, 2 CRITICAL, 3 UNKNOWN), so cron,
systemd timers, Icinga, Zabbix and checkmk consume it without it knowing they exist. `--json`
emits a report carrying the thresholds the verdict was reached with.

- Four rules, each citing the measurement or public source its numbers came from: per-pin
  current (ASUS Power Detector+; the 12V-2x6 per-pin rating), open pin, min/max imbalance
  (Thermal Grizzly's WireView Pro II, original and field-revised) and a pin voltage floor.
- A finding is reported only if it holds in **every** sample of a run, so one odd frame raises
  nothing.
- An advisory NOTE tier that appears in both outputs and cannot change the exit code, so the
  one publicly documented real Astral imbalance reaches a human without paging anyone.
- `make install-guard`, honouring `DESTDIR` and `PREFIX`. Nothing is written to `/etc` and
  nothing is enabled; `examples/` holds systemd, cron and Icinga snippets as documentation.
  Nothing in this release acts on a verdict — see 0.3.0.
- `astral-guard(1)` documents each rule, the exit codes and the limitations.

### Tests

Four tiers, and only the last needs hardware: the Python mirror (`make test`), the driver's
own C compiled for the host under ASan/UBSan (`make test-native`), the guard's rule engine on
the same pattern (`make test-guard`), and the hardware tier, which skips rather than fails
without a card (`make test-hw`). The cardless tiers run corpora captured off real hardware,
each case naming its origin. `make qa` is the gate.

### Measurements

`docs/measurements/` carries the raw data every constant derives from: captured frames and
their expected decodes, a read-only sweep of the chip's whole address space, load runs to
581 W / 48.26 A total / 8.56 A on the busiest pin, and two suspend/resume cycles. Channels
that remain unexplained — an eighth `(mV, mA)` pair, eight board temperatures, lifetime
counters — are documented there and deliberately not published as sensors.

### Licence

Licensed by component. `driver/` and `tests/native/` are **GPL-2.0-only** with
`MODULE_LICENSE("GPL v2")`: the module binds ten `EXPORT_SYMBOL_GPL` symbols, so a licence the
kernel does not recognise as free software would refuse to load, and once GPL compatibility is
compulsory a permissive grant only permits the work to be taken somewhere it need not be given
back. Everything else is **MIT** — `astral-guard` includes no kernel header and reads sysfs the
way `cat` does. Full reasoning in `LICENSING.md`.

## Earlier, unpublished

0.1.0 was the driver alone: the hwmon channels, the staged probe, the PCI gate, the
plausibility gate, DKMS packaging and the Python mirror. 0.2.0 added `astral-guard` and its
test tier. Neither was released.
