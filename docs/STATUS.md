# Status — 2026-08-19

Where the project stands, what is verified, what is open, and
which theories have already been ruled out so nobody re-investigates them.

## Working

Built, DKMS-installed, and **attaching automatically at boot** on the reference machine
(Ubuntu 26.04, kernel 7.0.0-30, ROG Astral RTX 5090 OC `0x89E31043`, chip on `i2c-4` at
`0x2b`). Twelve read-only hwmon channels plus `update_interval`, visible to `sensors` and
anything on libsensors.

- 47 unit tests, 18 hardware tests, 20 native tests, 18 guard tests, `make qa` green.
- **`astral-guard` is built** — the userspace judgement layer, one-shot C11, monitoring-plugin
  exit codes, verified reporting OK on the reference card. Scope below.
- **Opt-in protection exists, off by default.** `examples/astral-guard-poweroff.service`
  turns a CRITICAL verdict into a warned, cancellable poweroff, with a desktop notifier
  alongside it. Nothing installs or arms it. Verified by driving the real units against a
  fake sysfs tree: it fires on exit 2 and not on 0, 1, 3 or 203, and a missing notifier does
  not block the poweroff. This is the first thing in the repository that acts rather than
  reports; the driver and the guard themselves are unchanged.
- Autoload by PCI modalias — udev loads the module only on machines with a supported card.
- Boot cost: none measurable (kernel 6.807 s, userspace 31.094 s, against 6.803 / 31.464
  before the module existed).
- **Survives an NVIDIA major-branch upgrade.** Re-verified 2026-08-14 on the open kernel module
  **610.43.02** (up from the 595 branch): attached at 10.9 s on the first probe, all 16
  hardware tests pass, adapter name format unchanged, register layout unchanged. The one thing
  that could have broken this driver — the `NVIDIA i2c adapter N at <pci>` name it parses —
  did not change across the branch.

## Verified by measurement

Raw data for each of these is under `docs/measurements/`; the short version:

- Protocol: 24 bytes from `0x80`, six blocks of `(u16 BE mV, u16 BE mA)`, **reverse pin
  order** (Pin1 at offset 20). Only `I2C_SMBUS_I2C_BLOCK_DATA` works — a repeated-START
  multi-byte read silently returns all zeros.
- Idle 12.17 V / 0.42–0.48 A per pin. Under a 562 W CUDA burn: 7.56–8.48 A per pin, 47.7 A
  total, 11.6% spread — a healthy connector. Readings quantise to 20 mA.
- **Highest load measured: 581 W / 48.26 A total / 8.56 A on the busiest pin**, 11.944 V
  minimum, zero rejections (`docs/measurements/2026-08-14/burn-2hz.csv`). Per-pin shares at
  that load match those projected from a gaming capture to within 0.23 pp.
- **The register space is mapped** (`docs/measurements/2026-08-14/register-map.md`). `0x80` is
  a bank of `(mV, mA)` channels, of which six are published; `0x60` holds eight board
  temperatures in 0.1 °C; `0xc0` holds three 1 Hz time-in-state counters that survive reboots.
  Nothing in the chip resembles a threshold, limit or alarm-status register.
- One block read costs ~5 ms, so the 200 ms cache is ≤2.5% bus duty under continuous reading.
- Boot sequence: PCI detect 7.6 s → nvidia adapters 8.9 s → attached 10.9 s, first probe.

## Open

**The seventh register block at `0x98`, and now an eighth at `0xa0`.** `0x98` tracks 0.5066 ×
the pin sum (sd 0.0004, n=77) across an 11.9× load range, reading 24.18 A at peak — which rules
out the PCIe slot rail (capped near 6.25 A). Reconfirmed 2026-08-14 at **0.50669** under a
583 W burn, a day and a reboot and an NVIDIA branch upgrade later, so it is stable to four
decimal places. The consistent +1.3% offset from exactly half, and a voltage 50–60 mV below the
pins, suggest an independent measurement of total 12VHPWR current rather than a computed
`sum/2`. Why it reads half scale is unresolved.

The register scan found an **eighth channel at `0xa0`** in the same `(mV, mA)` form: 0.368 A at
idle, 0.987 A at 583 W on a 12.1 V rail. It is not a fixed fraction of anything (15.9% of the
pin sum at idle, 2.2% under load), so it is an independent measurement. The PCIe slot's 12 V
rail is the leading hypothesis — it is the only other 12 V input to the board and the magnitude
fits — but it is untested.

v1 exposes neither. **Cheapest way to settle both at once: boot Windows and see whether HWiNFO
or GPU Tweak III names those channels.** If `0x98` gets a name it becomes `curr7`.

**A residual client-lifetime window during attach.** Because no `i2c_driver` is bound, nothing
serialises `astral_attach()` against `i2c_del_adapter()`, which frees every non-dummy child
client. The client is now published to `astral_client` the moment it exists, so the DEL
notifier can clear it, and the `i2c_get_adapter()` reference held across the attach pins the
adapter's owner module — which closes the dominant path, since `rmmod nvidia` is what deletes
these adapters. What remains is a GPU reset that calls `i2c_del_adapter()` without unloading
the module, landing inside the few milliseconds of an attach in progress.

Closing it completely means binding an `i2c_driver` and letting the driver core serialise
probe against removal on the device lock. That is a real trade: a bound driver marks `0x2b`
busy in `i2c-dev` and locks userspace out, which is precisely what lets the agreement test
sample the driver and the oracle at the same instant. **Not worth paying for a window this
narrow while the two-implementation check is the project's main safety net** — revisit if
reset testing shows it firing in practice. Suspend/resume no longer counts as a way to look
for it: two cycles on 2026-08-15 never took the adapter down at all.

**`astral-guard` is built, with a deliberately narrower scope than `docs/GUARD-DESIGN.md`
sketched.** It stays in userspace — and that is no longer a judgement call: the register sweep
found no threshold, limit or status register anywhere in the chip, so there is nothing for a
`curr*_max` to marshal. The driver's telemetry-only invariant does not change.

What shipped: four rules (per-pin current, open pin, min/max imbalance, voltage floor), each
requiring persistence across every sample of a run; text and JSON output; monitoring-plugin
exit codes; no privileges and no init-system dependency. See `guard/README.md` for the
threshold table and what the tier deliberately does not check.

What was deliberately left out, and why:

- **No fingerprint-drift metric.** It needs a stored per-machine baseline, which turns a
  stateless checker into something that has to be captured, invalidated and trusted. The
  static rules catch the documented failure shapes without it.
- **No mitigation.** It reports; it never throttles or powers anything off. Capping via
  `nvidia-smi -lgc` / `-pl` remains unexercised on Linux, which is the last unmeasured half of
  `docs/GUARD-DESIGN.md`.
- **No staleness check.** The guard does not verify the driver's readings are fresh. A wedged
  sensor path would read as steady rather than absent. Deferred deliberately, not forgotten.
- **Thresholds are reasoned from public sources, not tuned against a fleet.** ASUS Power
  Detector+ and Thermal Grizzly's WireView Pro II supply the numbers, and no connector here
  has ever been measured while actually degrading. A second card being confirmed does not
  change this: it widens the evidence for the *protocol*, not for the thresholds.

The old blocking gap — every threshold sitting above anything ever measured — is **closed**:
the 581 W burn reached 8.56 A on the busiest pin and the projected fingerprint held to 0.23 pp.

**The client-teardown path is still untested** — but suspend/resume is no longer the way to
reach it. Two S3 cycles on 2026-08-15 (`docs/measurements/2026-08-15/`) showed NVIDIA keeps
its i2c adapters registered across sleep: the hwmon node never went away, no
`BUS_NOTIFY_DEL_DEVICE` fired, and the driver logged nothing at all. The prediction recorded
here before the measurement — that resume would exercise the teardown and re-attach paths —
was simply wrong.

What suspend/resume *did* settle, in both cycles identically: reads in the window return
`-EIO` and never a decodable frame, the first good frame lands 100 ms after wake at ordinary
idle currents, and `astral-guard` reports OK at wake, +5 s and +30 s. The vendor's documented
post-resume false over-current does not occur here.

So `BUS_NOTIFY_DEL_DEVICE` and the re-attach path are reachable only by
`rmmod nvidia` / `modprobe nvidia`, which needs a console-only boot or a second machine — with
a desktop session on the card, `nvidia` has ~534 references and will not unload.

**CI has never actually run.** The workflow moved from `.gitea/workflows/` to
`.github/workflows/` for the public repository, which is what makes it live: GitHub-hosted
runners are free for public repos, so there is no runner to register any more. Its logic was
validated locally (every commit passes the commit-msg gate; the skip-detection matches real
pytest output) and the YAML parses, but no job has executed on a runner yet. Two things to
watch on the first run: the commit-range fallback for an all-zero `github.event.before`, and
the kernel-headers step, which now installs `linux-headers-generic` and points `KDIR` at it
rather than asking for the runner's own Azure kernel headers, which are often absent.

**Two cards are Linux-verified.** `0x89E31043` on the reference machine, and `0x89EC1043`
(ROG Astral RTX 5090 LC) confirmed by an owner report in issue #1 — the first evidence that
the sensor block really is shared across the family rather than assumed to be. The other six
ids are still published Astral SKUs listed on that assumption; each wants an owner report.
The `card-report` issue template is the whole mechanism for closing this — it cannot be
closed from this machine, and issue #1 is proof the mechanism works.

## Ruled out — do not re-investigate

**Slow desktop at boot is not this module.** `openrgb-fix.service` takes ~28 s of a ~31 s
userspace boot and has done since before this project existed (13–18 s on Aug 12 boots, 17.1 s
on a boot where the module was not installed, 18.4 s with it). It drives the Astral's RGB over
the same I2C bus, which makes it a tempting suspect; it is not one. Our driver is idle at boot
because it never polls — it touches I2C only when something reads a sensor.

**Userspace is not locked out of `0x2b` by the bound driver.** Assumed in an early draft of
design, disproved on hardware: no `i2c_driver` is bound to the client, so i2c-dev never
marks the address busy. This is why the agreement test samples both paths simultaneously.

## Bugs found and fixed, with their mechanisms

Kept because each is a trap that could be reintroduced:

| Bug | Mechanism |
|---|---|
| Discovery found nothing | Adapter name carries ` at <pci>`; exact `strcmp` matched nothing — and the unit fixture omitted the suffix, so the test agreed with the bug |
| Wrong pin / read past end | hwmon zero-indexes voltages, one-indexes currents |
| Use-after-free on `rmmod` | `i2c_del_adapter()` frees child clients; `astral_client` dangled and re-attach was blocked forever |
| All-zeros could reach userspace | Decode wrote into the published cache before validation, so a failed read clobbered a concurrent reader's cache hit |
| Double attach / leak | Unsynchronised check-then-set on `astral_client` |
| Two deadlocks | Lock taken before the `dev->type` filter (ADD re-entry); lock held across attach (DEL re-entry from the failure path) |
| Permanently dead driver at boot | Probe ran inline in the notifier, where the adapter cannot yet transfer, and one failure was fatal |
| 9 of 15 hardware tests never skipped | Skip depended on requesting a fixture; now autouse |
| Gate tests passed vacuously | They asserted "no chip registered" without checking the module was even loaded |
| Discovery crashed on ACPI adapters | `/sys/bus/i2c/devices` lists `i2c-MSFT8000:01`, which is not an integer |
| Stale module after iteration | Hand-copied `.ko` sat beside DKMS's `.ko.zst` |

## Suggested next steps

1. Decide what evidence promotes an id from "inherited" to "yes" in the README table. Issue
   #1 was accepted on a user's `sensors` output; a decode that agrees with the oracle is a
   higher bar. Worth settling deliberately rather than per-issue, now that reports arrive.
2. Settle `0x98` and `0xa0` from Windows — five minutes, unblocks a seventh and eighth channel.
3. Exercise `rmmod nvidia` / `modprobe nvidia` from a console-only boot. It is the only route
   left to `BUS_NOTIFY_DEL_DEVICE` and the re-attach path now that suspend/resume is known not
   to reach them.
4. Test the response side: how fast `nvidia-smi -lgc` / `-pl` actually caps, and whether it
   restores exactly. It is the last unmeasured half of `docs/GUARD-DESIGN.md`, and the
   prerequisite for `astral-guard` ever growing a mitigation mode.
5. Collect owner reports for the six remaining unverified ids, via the `card-report` issue
   template.
