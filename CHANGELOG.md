# Changelog

Notable changes per release. The version lives in `dkms.conf` and is mirrored by hand into
`MODULE_VERSION()` and `pyproject.toml`; `astral-guard` takes its `--version` string from
`dkms.conf` via the Makefile, so it cannot drift.

## 0.3.1 — 2026-09-03

A clean-code pass over the shipped C and Python, plus dev-gate repairs found while reviewing
it. Nothing in the monitored rules changed; one argument that used to be accepted is now
refused, and the `--json` report gained two fields.

### Fixed

- `astral-guard` parsed every numeric option with `atoi()`, which cannot distinguish a
  malformed value from zero, and range-checked only `--samples`. A ratio threshold of zero can
  never fire — R3 tests `lo * 100 < hi * pct` — so `--warn-ratio` given a non-numeric value, or
  an explicit `0`, disarmed the imbalance warning and the run still exited OK. Every numeric
  option is now parsed with `strtol` and refused, with the rejected value named, unless it
  falls inside the range the man page documents. Trailing junk is refused too: `70%` is a
  mistake worth reporting, not `70`.
- `--json` promised "the thresholds that were applied" but omitted `open_gate_ma` and
  `ratio_gate_ma`. Those two matter most to whoever reads a stored report: an absent imbalance
  finding means nothing unless the reader can see whether the run ever cleared the load gate.
  All ten fields of `struct guard_limits` are now emitted, and both test tiers pin that.
- `astral-guard` validated its arguments and not its readings. `read_int_attr` checked only
  that parsing had started: no trailing-junk check, no range, and a silent narrowing from
  `long` to `int`. A `curr1_input` of `8000abc` read as 8000 mA, and `4294975296` — 2^32 plus
  8000 — read as exactly 8000; both exited OK. A wider value reached the rules and overflowed
  them, which UBSan reports as undefined behaviour in `guard_eval`. Readings are now held to
  the standard the arguments already were, bounded by the driver's own plausibility window,
  and anything outside it fails the sample and reports UNKNOWN. The bounds are duplicated
  into `astral_guard.h` the way `GUARD_CHIP_NAME` already is, with the native tier asserting
  the copies agree.
- `guard_eval`'s `add()` returned on a full findings array before raising the verdict, so a
  truncated CRITICAL would have been reported as OK. Fourteen findings is the current worst
  case against a limit of sixteen, so it was unreachable — the order is now the safe one
  regardless, because the cost of being wrong about that is the whole tool.
- `make -j qa` could silently drop the guard CLI tier. The `qa` target lists its steps in
  dependency order, but under `-j` make runs prerequisites unordered; `coverage` could start
  before `guard` had built the binary, and `tests/unit/test_guard_cli.py` skips without it —
  verified at eighteen skips in under a second. `coverage` now depends on `guard`, which holds
  under both invocations.

### Structure

Findings from a separation-of-concerns and DRY pass over the whole codebase. No behaviour
changes; the duplication that is deliberate — the driver against the Python oracle — was
left alone, because the hardware agreement test is only meaningful while neither side is
derived from the other.

- The chip name `astral12vhpwr` exists in three places: `ASTRAL_CHIP_NAME`, `GUARD_CHIP_NAME`
  and `hwmon.py`'s `CHIP_NAME`. `tests/native/test_names.c` asserted the first two agree and
  said why — a hand-edit to either copy leaves the reader silently finding nothing — but a C
  tier cannot see a Python constant, so the third was unpoliced with the same failure mode:
  `find_hwmon_dir()` returns `None`, which reads as "the driver is not loaded". A new
  `test_c_and_python_chip_names_agree` parses `driver/astral.h` the way the allowlist test
  already parses `driver/astral_detect.c`.
- `astral_oracle.hwmon` read the chip's sysfs surface *and* shelled out to `sudo modprobe`.
  The module lifecycle moved to a new `astral_oracle.modctl`, which is now the only file in
  the package that can change the machine — `hwmon.py` is a pure reader. `module_resident()`
  went with it: the question it answers is about the module, not about the chip.
- `discover.py` walked `/sys/bus/pci/devices` with the same vendor filter in two functions,
  five lines duplicated verbatim. Both now go through one `_nvidia_devices()` generator.

### Build

- The module built with two kbuild warnings that reported nothing wrong. The compiler was
  byte-identical to the kernel's — same Ubuntu package, same version — but kbuild compares
  `$(CC) --version` against `CONFIG_CC_VERSION_TEXT` as a plain string, and Debian and Ubuntu
  record the triplet name (`x86_64-linux-gnu-gcc`) while gcc reports itself as `gcc`. The
  Makefile and `dkms.conf` now use the compiler name the kernel itself recorded, when a
  program by that name exists; where the `.config` records plain `gcc` this resolves to `gcc`
  and changes nothing, and an explicit `CC=` still wins. The fallback is deliberate rather
  than incidental: `dkms.conf` re-runs unattended on every kernel upgrade, and an empty `CC=`
  would override kbuild's own default with nothing and break the build rather than warn.
- The second warning was not a version mismatch: `pahole` was absent, so kbuild compared the
  kernel's `131` against `0`. Installing `dwarves` is a local prerequisite, not a repository
  change, and is noted here only because it is the other half of a clean build log.

### CI

- Every job carries a `timeout-minutes` cap, and the `apt-get` step has its own eight-minute
  cap plus explicit retries and transfer timeouts. On 2026-08-19 an Ubuntu mirror stalled with
  no timeout of its own and the module build sat silent for six hours until GitHub's runner
  limit killed it, leaving a red badge for an outage unrelated to the driver. A mirror stall
  now fails in minutes, visibly as infrastructure.
- The warning gate is keyed to the shape gcc emits (`file.c:LINE:COL: severity:`) instead of
  any line containing "warning:" or "error:". kbuild prints two benign bare prose lines —
  "the compiler differs…", "pahole version…" — which the old pattern matched and needed an
  exclusion list to ignore; they carry no file prefix and cannot match the new one. The step
  also runs under `pipefail` now: a link-stage failure ("collect2: error:") has no file prefix,
  so the grep was never a sound backstop for it — make's own exit status is.

### Documented

- The deliberate asymmetry between `astral_decode_frame()` and `decode_frame()` — the driver
  validates the whole frame after decoding because its caller needs one verdict, the Python
  mirror validates inline and raises per pin because its caller is diagnosing a pin — is now
  stated in both files. Neither said so, and a future "cleanup" unifying them would quietly
  cost the independence the hardware agreement test depends on.
- STATUS refreshed: CI's first runs went green on 2026-08-19, so "never actually run" no
  longer stood; test counts updated for the new CLI cases.
- A second owner report for `1043:89EC` (issue #2) is recorded in STATUS and the README. It is
  the first loaded reading from a card that is not the reference machine — about 586 W through
  the connector, the worst pin 860 mA under `pin_warn_ma` and the six balanced to 95% — which
  is the first outside evidence that the guard's warn line does not trip a healthy card at
  load. It also confirms the driver on Ubuntu 24.04 and on a two-digit adapter bus. No code
  changed: the id was already in the table, and the reporter's SKU shares it.

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
