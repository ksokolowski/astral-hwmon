# Status — 2026-09-06

Where the project stands, what is verified, what is open, and
which theories have already been ruled out so nobody re-investigates them.

## Working

Built, DKMS-installed, and **attaching automatically at boot** on the reference machine
(Ubuntu 26.04, kernel 7.0.0-31, ROG Astral RTX 5090 OC `0x89E31043`, chip on `i2c-4` at
`0x2b`). Twelve read-only hwmon channels plus `update_interval`, visible to `sensors` and
anything on libsensors.

- 60 unit tests, 18 hardware tests, 20 native tests, 18 guard tests, `make qa` green.
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

**CI runs green on GitHub.** The workflow moved from `.gitea/workflows/` to
`.github/workflows/` for the public repository, which is what made it live: hosted runners are
free for public repos, so there is no runner to register. First runs 2026-08-19: green on
`main` and on the `v0.3.0` tag. Two things had been flagged to watch on the first run. The
kernel-headers step held: the build completed with `KDIR` pointing at `linux-headers-generic`
rather than at the runner's own Azure kernel headers. The commit-range fallback for an
all-zero `github.event.before` has not been exercised yet — it fires only when a push creates
history the clone does not have (branch creation, force-push), which no run has done — so it
stays a watch-item until that happens once.

**Two subsystem ids are Linux-verified, across three cards.** `0x89E31043` on the reference
machine, and `0x89EC1043` confirmed twice by unsolicited owner reports: issue #1 (ROG Astral
RTX 5090 LC, idle) and issue #2 (`ROG-ASTRAL-LC-RTX5090-O32G-GAMING`, the LC OC 32 GB). The
two reporters name different SKUs and report the same id, so `0x89EC1043` spans at least both.
Issue #1 was the first evidence that the sensor block really is shared across the family
rather than assumed to be.

Issue #2 adds what issue #1 could not: a **loaded** reading from a card that is not this one.
7.92, 8.12, 8.22, 8.34, 8.32 and 8.30 A at 11.90–11.92 V — 49.2 A, about 586 W through the
connector. Every loaded figure behind the guard's thresholds until then came from the
reference machine's own burn. On that frame the worst pin sits 860 mA under `pin_warn_ma`
(9200) and the pins balance to 95%, well clear of the 85% advisory, and `astral-guard` returns
OK: the first outside evidence that the warn line is not set so tight that a healthy card at
load trips it. The report also confirms the driver on Ubuntu 24.04 with NVIDIA 595.84, and on
a two-digit adapter bus (`i2c-15`) — until then only the unit test's word-boundary case had
covered a two-digit index.

The other six ids are still published Astral SKUs listed on that assumption; each wants an
owner report. The `card-report` issue template is the whole mechanism for closing this — it
cannot be closed from this machine, and two unprompted reports are proof the mechanism works.

## Ruled out — do not re-investigate

**Slow desktop at boot is not this module.** `openrgb-fix.service` takes ~28 s of a ~31 s
userspace boot and has done since before this project existed (13–18 s on Aug 12 boots, 17.1 s
on a boot where the module was not installed, 18.4 s with it). It drives the Astral's RGB over
the same I2C bus, which makes it a tempting suspect; it is not one. Our driver is idle at boot
because it never polls — it touches I2C only when something reads a sensor.

**Userspace is not locked out of `0x2b` by the bound driver.** Assumed in an early draft of
design, disproved on hardware: no `i2c_driver` is bound to the client, so i2c-dev never
marks the address busy. This is why the agreement test samples both paths simultaneously.

**The free drivers are not an alternative route to this data.** Checked against mainline
and against the `nouveau.ko` shipped with kernel 7.0.0-31, not assumed. nouveau does register
the GPU's DCB I2C buses as ordinary adapters — `nvkm_i2c_bus_ctor()` in
`nvkm/subdev/i2c/bus.c` names them `nvkm-<pci-addr>-bus-<id>` and calls `i2c_bit_add_bus()`
or `i2c_add_adapter()`, so they would reach `/dev/i2c-N` exactly as the NVIDIA ones do. The
subdev is simply not wired up past Ampere: in `nvkm/engine/device/base.c` the per-chipset
table declares `.i2c` through `nv177_chipset` (GA107) and omits it for every Ada and
Blackwell entry, including `nv1b2_chipset` (GB202) — display and DDC moved to GSP-RM firmware
and the bit-bang path was not carried forward. On a card this driver targets, nouveau
therefore publishes no GPU I2C adapter at all, and discovery would correctly find nothing.

Nova is further away, not closer, despite being the designated successor for exactly this
hardware. `drivers/gpu/nova-core` has no I2C code; its only token of the kind is
`NV_THERM_I2CS_SCRATCH`, an I2C-secondary scratch register used as a mailbox to poll FSP boot
completion. `Documentation/gpu/nova/core/todo.rst` is entirely foundational — devinit, MMU,
VRAM allocator, instance memory, GSP bootstrap and message queue, FIFO, GR, CE, VFN IRQ — and
mentions neither display nor DDC nor I2C. Both Kconfigs still read "work in progress and may
not be functional". `rust/kernel/i2c.rs` does exist, but it is the consumer side only:
`i2c_driver`, `I2cClient`, probe and remove. There is no binding for `i2c_add_adapter`, so a
Rust driver cannot register a bus at all until somebody writes the provider abstraction.
An I2C bus in Nova therefore needs three things stacked, where nouveau needs one backend
behind machinery that already exists.

And the in-tree `i2c-nvidia-gpu` is unrelated hardware — it matches PCI class `0x0c80`, the
separate USB Type-C controller function on Turing boards, not the board buses the sensor
sits on.

The practical answer for an Astral owner is narrower than it first looks, and better. The
reference machine already runs the **open** kernel modules (`nvidia-dkms-610-open`,
`nvidia.ko` reporting `license: Dual MIT/GPL`), and it is that open code which publishes
`NVIDIA i2c adapter 1 at 1:00.0`. Bus access here does not depend on a binary kernel module;
it depends on `nvidia-open`. Those modules are not themselves in mainline and NVIDIA has not
proposed them for it — the ABI is tied to driver releases and the GSP interface is still
moving — so "out of tree", not "proprietary", is the objection that actually applies here.
There is no free-driver substitute to migrate to, and none is being sought — this is recorded
so the question is not re-opened.

One pointer, for whoever eventually builds the in-tree path. GSP-RM already exposes an I2C
API, published by NVIDIA under MIT in the open modules as class `NV40_I2C` (`0x402c`):
`NV402C_CTRL_CMD_I2C_GET_PORT_INFO` enumerates ports and `NV402C_CTRL_CMD_I2C_TRANSACTION`
carries transfers, with `SMBUS_MULTIBYTE_REGISTER_BLOCK_RW` among its types — register index
then N bytes, which is precisely the shape of the one call this driver makes at
`astral_regs.c`. Whether the firmware will drive a non-DDC port on the host's behalf is
untested. Recorded because it is the interface such work would build on, not because it is
planned here.

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
