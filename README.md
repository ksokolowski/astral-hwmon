# astral-hwmon

**Per-pin 12VHPWR monitoring for ASUS ROG Astral cards, as standard Linux hwmon sensors.**

<p align="center"><a href="https://github.com/ksokolowski/astral-hwmon/actions/workflows/ci.yml"><img src="https://github.com/ksokolowski/astral-hwmon/actions/workflows/ci.yml/badge.svg" alt="ci"></a> <a href="https://github.com/ksokolowski/astral-hwmon/releases/latest"><img src="https://img.shields.io/github/v/release/ksokolowski/astral-hwmon" alt="latest release"></a> <a href="LICENSING.md"><img src="https://img.shields.io/badge/licence-GPL--2.0%20%2F%20MIT-blue" alt="licence"></a> <a href="https://github.com/sponsors/ksokolowski"><img src="https://img.shields.io/badge/Sponsor-%E2%99%A5-ea4aaa?logo=githubsponsors&logoColor=white" alt="sponsor"></a> <a href="https://ko-fi.com/styledconsole"><img src="https://img.shields.io/badge/Ko--fi-support-ff5e5b?logo=ko-fi&logoColor=white" alt="ko-fi"></a></p>


Reads the six individual 12VHPWR pin currents and voltages from the card's own ITE IT8915FN
monitoring chip and publishes them through hwmon, so `sensors` and everything built on
libsensors — psensor, Waybar, netdata, Prometheus exporters — picks them up with no bespoke
client.

```
astral12vhpwr-i2c-4-2b
Adapter: NVIDIA i2c adapter 1 at 1:00.0
12VHPWR Pin1 Voltage:  12.18 V
12VHPWR Pin2 Voltage:  12.16 V
...
12VHPWR Pin1 Current: 540.00 mA
12VHPWR Pin2 Current: 560.00 mA
...
```

## The driver monitors. It does not protect.

There are no thresholds in the driver, nothing in it watches the readings, and nothing in it
acts on them. That is deliberate and it is not going to change: the hwmon ABI is explicit
that an `*_alarm` file is a status bit read from the chip rather than a comparison the driver
makes, and this chip has no comparator — established by sweeping its whole address space,
not assumed. The reasoning is in [`docs/GUARD-DESIGN.md`](docs/GUARD-DESIGN.md) §1.

Sustained current above ~9.5 A on a single 12VHPWR pin is the documented failure regime for
connector melting. The driver shows you that number.

`astral-guard`, below, is the part that judges it. It still does not *protect*: it reports,
and it is your monitoring system or your own judgement that acts. Neither the driver nor the
guard ever throttles the card or shuts the machine down. `examples/` carries one unit that
will — off by default, yours to arm.

## Supported hardware

Only ASUS ROG Astral cards carry per-pin current sensors. No other GPU exposes this,
including non-Astral RTX 5080/5090 cards.

| Subsystem ID | Model | Linux-verified |
|---|---|---|
| `1043:89E3` | ROG Astral RTX 5090 OC | yes |
| `1043:89EA` | ROG Astral RTX 5090D OC | inherited |
| `1043:8A61` | ROG Astral RTX 5090 Matrix | inherited |
| `1043:89EC` | ROG Astral RTX 5090 LC | yes |
| `1043:89DE` | ROG Astral RTX 5080 OC | inherited |
| `1043:8A2E` | ROG Astral RTX 5090 OC White | inherited |
| `1043:8A2B` | ROG Astral RTX 5080 OC White | inherited |
| `1043:8A45` | ROG Astral RTX 5080 OC Hatsune Miku | inherited |

"Inherited" means the id is a published Astral SKU listed on the assumption that the sensor
block is identical across the family. The RTX 5090 OC and the RTX 5090 LC have been read on Linux
here — **a report from any of the other six is the single most useful contribution to this
project**, and there is a [card report issue template][card-report] for it. Reports that it
does *not* work are just as welcome.

[card-report]: https://github.com/ksokolowski/astral-hwmon/issues/new?template=card-report.yml

Requires the proprietary NVIDIA driver, which is what exposes the GPU's I2C adapters.

## Install

```sh
git clone https://github.com/ksokolowski/astral-hwmon.git
cd astral-hwmon
make dkms-install     # builds, signs, installs via DKMS; survives kernel upgrades
sensors
```

The module declares a PCI device table for the supported cards, so **udev loads it
automatically, and only on machines that have one**. No entry in `modules-load.d` and no
hardcoded module list.

`make module && make module-load` builds and inserts it once without installing, if you would
rather try before committing.

**Secure Boot:** DKMS signs the module with your MOK key, but that key has to be enrolled
(`mokutil --import /var/lib/shim-signed/mok/MOK.der`, then confirm at the next boot) or the
kernel will refuse to load it. Nothing to do if Secure Boot is off.

Remove with `make dkms-remove`.

## Module parameters

| Parameter | Default | Effect |
|---|---|---|
| `bus=N` | auto | Use adapter N of the matched card instead of auto-detecting it |
| `addr=0xNN` | `0x2b` | Chip address |
| `allow_unknown=1` | off | Accept an NVIDIA card whose subsystem id is not in the table |

**The PCI-id check always runs first, before any I2C traffic, and no parameter disables it.**
`allow_unknown` relaxes only the model table; `bus=` only chooses among the matched card's own
adapters. Neither can point the driver at an unrelated bus — `0x2b` on your motherboard's
SMBus is somebody else's device.

The driver also refuses to register unless the chip returns a physically plausible frame
(all six voltages between 6 and 13 V). This is not decoration: a naive multi-byte read on
this bus returns all zeros, which decodes to a perfectly healthy-looking 0 A on every pin.

The floor is 6 V rather than something tighter on purpose. A failing 12VHPWR connector
raises contact resistance and so pulls the pin voltage *down* under load, and one pin below
the floor fails the whole frame — every voltage and every current would read `-EIO`
together. A tight floor would therefore hide the fault it was meant to guard. 0 V is
unambiguously a broken read; 9 V is a reading worth seeing.

## How it loads

Four stages, each reported to the kernel log, so a machine that never produces sensors says
why rather than failing silently:

1. **Is a supported card in this machine?** Answered from the PCI bus, which is enumerated
   long before the nvidia driver loads. No card → the module refuses to load
   (`-ENODEV`) instead of sitting resident forever.
2. **Are the card's i2c adapters up?** The nvidia driver publishes them well after boot
   starts. Until then the module waits, saying so once.
3. **Is the sensor chip answering?** The adapter exists before it can carry a transfer, so
   this is retried too.
4. **Attach** — register the hwmon chip.

Stages 2 and 3 retry every 2 s for 30 s, then give up with a message naming the stage that
failed. Loading the module by hand when the card is already up skips straight to stage 4, so
`modprobe astral-hwmon && sensors` works without a race.

Nothing in this sequence blocks boot: the probe runs on a workqueue, never from the notifier,
and no I2C happens until the card has been identified by PCI id.

## How often should you read it?

The driver does not poll. It touches the I2C bus only when you read an attribute, and caches
the result for **200 ms** — published as `update_interval`, so tools can discover it instead
of guessing:

```sh
cat /sys/class/hwmon/hwmon*/update_interval   # 200
```

Reading faster than that just re-serves the cache. One block read costs about 5 ms measured,
so even continuous hammering at the cache rate is ~2.5% of the bus; a 1 Hz widget is
negligible. There is no benefit to polling faster than 5 Hz and no harm in polling slower.

200 ms is shorter than the classic hwmon idiom (`w83781d` caches for 1.5 s) and than `lm90`'s
500 ms. Those chips are slow to convert; this one converts continuously and a read is cheap.
Keeping the ceiling well under a second is also what leaves room for a future guard to catch
a fast over-current, where a multi-second cache would not.

**GPU runtime power management:** each read touches the card's I2C bus, so continuous polling
can keep a GPU that would otherwise runtime-suspend (RTD3) awake. This does not arise on a
desktop driving a display, which never suspends anyway, but is worth knowing before wiring a
1 Hz widget to a laptop's dGPU.

## Unrecognised Astral variant?

If you have an Astral card that is not in the table, the driver will say so in `dmesg` and
register nothing. To check whether it would work:

```sh
sudo modprobe astral-hwmon allow_unknown=1
sensors
lspci -nn -d 10de:
```

If you get six plausible readings, open a [card report][card-report] and the id can be added.
`allow_unknown` relaxes the model list only, never the NVIDIA vendor check, so it cannot point
the driver at an unrelated bus.

## Read-only, by construction

The driver implements no write callback and every attribute is `0444`. It cannot write to the
chip, and there is no parameter that makes it able to. Writing to the wrong device on a GPU
I2C bus can destroy hardware, so the capability simply does not exist in the code.

## astral-guard

The judgement the driver refuses to make, as a separate one-shot program.

```sh
make guard
sudo make install-guard        # honours DESTDIR and PREFIX
astral-guard
```

```
OK 12VHPWR /sys/class/hwmon/hwmon10
```

It samples the driver's sysfs a few times, applies rules drawn from ASUS Power Detector+,
Thermal Grizzly's WireView Pro II and the published Astral failure reports, and exits with a
monitoring-plugin code:

| exit | meaning |
|---|---|
| 0 | OK |
| 1 | WARNING |
| 2 | CRITICAL |
| 3 | UNKNOWN — no chip found, or a channel could not be read |

A finding is reported only if it holds in **every** sample of the run, so a single odd frame
raises nothing. `--json` emits a machine-readable report carrying the thresholds the verdict
was reached with. It needs no privileges — every attribute it reads is world-readable — and
it makes no init-system assumption; `examples/` has systemd, cron and Icinga snippets, plus
an opt-in unit that powers the machine off on CRITICAL and a desktop notifier to go with it.
None of them is installed for you.

The program reports; it does not mitigate. It never throttles the card and never powers
anything off. What it can do is exit 2, and `examples/astral-guard-poweroff.service` turns
that into a poweroff after a warning and a cancellable grace period — off by default, and it
takes two deliberate edits to arm. Read it before you do. The warning goes to every terminal
via `wall`, and `examples/astral-guard-notify.sh` adds a desktop notification for the case
that matters most: somebody full-screen in a game with no terminal open.

It also measures *current*, while the hazard is contact heating: a green verdict is not a
temperature claim. Two card models have been read on real hardware — the reference machine's
RTX 5090 OC and a user-reported RTX 5090 LC — so on any other Astral variant the rules are
reasoned, not observed.

`man 1 astral-guard` documents each rule and where its number came from.
[`guard/README.md`](guard/README.md) covers the tests, including what they deliberately do
not check.

## Development

```sh
make setup       # uv sync + install the git hooks
make test        # unit tests - no hardware needed, runs anywhere
make test-native # the driver's own decoder and PCI gate, compiled for the host
make test-guard  # the guard's rule engine and sysfs reader, no hardware
make test-hw     # hardware tests - skips cleanly without a supported card
make qa          # lint + format + types + coverage + the native and guard tiers
make module      # build the kernel module against the running kernel
make guard       # build the astral-guard binary
```

Four tiers, and only the last needs a card. `make test` checks the Python mirror,
`make test-native` checks the C the driver actually ships — both against the same corpus of
frames captured off the card — `make test-guard` checks the guard's rule engine against
[`tests/data/guard-cases.jsonl`](tests/data/guard-cases.jsonl), and `make test-hw` checks the
two implementations against each other on real hardware. See
[`tests/native/README.md`](tests/native/README.md).

Current state, open questions and already-ruled-out theories are in
[`docs/STATUS.md`](docs/STATUS.md). The raw reference data every constant is derived from —
captured frames, the register survey, load and suspend/resume runs — is under
[`docs/measurements/`](docs/measurements), each directory with a README describing how it was
taken. Why the driver publishes no thresholds or alarms is
[`docs/GUARD-DESIGN.md`](docs/GUARD-DESIGN.md) §1.

## Prior art

Several people have independently worked out that ASUS Astral cards expose per-pin current
over the GPU's I2C bus — among them LibreHardwareMonitor, Timic3's `astral-power-monitoring`,
the OpenRGB device registry, and the author of this driver. The hardware facts are not
anyone's private discovery.

This is a fresh implementation. The register layout, bus, and pin ordering used here were
established by measuring the author's own card (see `docs/measurements/`) and cross-checked
against the published behaviour above. No code was taken from any of those projects.

The idea of monitoring per-pin current as a safety measure was inspired by the Windows tool
`12vhpwr-guard`; the inspiration is acknowledged, the implementation is not shared.

## Why it is out of tree

Out of tree is where this driver currently belongs, not somewhere it is parked for lack of
ambition. hwmon is a welcoming subsystem and a small I2C sensor driver is squarely the kind of
thing it takes. Two things stand in the way, and neither is fixed by sending a patch.

**It cannot reach the hardware without a proprietary module.** The chip sits on an I2C adapter
that the NVIDIA driver registers; no `nvidia` module means no adapter, and nothing for the
driver to bind to. That is why the load sequence waits for adapters rather than probing at
`module_init`. A mainline driver whose only route to the hardware is an out-of-tree
proprietary module is a hard sell no matter how clean it is. If a free driver — nouveau, or
the newer NVIDIA kernel stack — ever registers the same adapters on these cards, this
objection goes away, and that is the change most likely to make upstreaming realistic.

**The protocol was measured, not documented.** The register layout, scaling and pin order in
`docs/measurements/` came from reading the author's own card and cross-checking against
independent public descriptions. It reproduces exactly and it agrees with two separate
implementations here, but ASUS has published nothing, and two cards out of eight is a thin
basis for an ABI the kernel would then have to keep. Reports from the other six subsystem ids
are what would turn two cards' measurements into a family's behaviour — see
[Unrecognised Astral variant?](#unrecognised-astral-variant) above.

In the meantime the driver is deliberately written as though it were in-tree: standard hwmon
and I2C interfaces only, no private ABI, no sysfs of its own invention, GPL-2.0-only,
autoloaded by PCI modalias. Nothing here would need redesigning if the two obstacles above
cleared — which is the point.

## Supporting the project

astral-hwmon is a hobby project, free software, and will stay that way. If it is watching your
connector and you would like to support its development:

| Platform | Link |
| --- | --- |
| GitHub Sponsors | [github.com/sponsors/ksokolowski](https://github.com/sponsors/ksokolowski) |
| Ko-fi | [ko-fi.com/styledconsole](https://ko-fi.com/styledconsole) |

**Funding goal — a card on the bench.** Eight Astral subsystem ids are in the driver's table
and two have now been read on Linux: the reference RTX 5090 OC, and an RTX 5090 LC confirmed
by an owner report. The other six are listed on the assumption of an identical sensor block,
which remains the project's largest unverified claim.

A [card report][card-report] is still the cheapest way to shrink that number, costs you
nothing, and is worth more than money — it is how the LC was confirmed. What a report cannot
give is a card that stays here: one that can be held under load, have its connector
deliberately mis-seated, and be measured while it degrades. Every threshold in `astral-guard`
is reasoned from public sources rather than observed on a failing connector, and only a card
on the bench changes that.

Everything here so far — the card, the load testing, the measurement rig — has been personal
time and money.

## Licence

Licensed by component, because one component has no choice and the others do. Full reasoning
in [LICENSING.md](LICENSING.md); every file carries an authoritative `SPDX-License-Identifier`.

| | licence | why |
|---|---|---|
| `driver/`, `tests/native/` | **GPL-2.0-only** | The module binds ten `EXPORT_SYMBOL_GPL` symbols, so a licence the kernel does not recognise as free software would refuse to load. It is useful only inside Linux, where the GPL governs anyway. |
| `guard/`, `src/`, `tests/unit`, `tests/hardware`, `tools/`, `docs/` | **MIT** | `astral-guard` includes no kernel header and reads sysfs the way `cat` does. Nothing ties it to the kernel's licence, and the protocol it consumes came from measurement and public sources. |

Copyright © 2026 Krzysztof Sokołowski. Both texts are in [`LICENSES/`](LICENSES); the root
[`LICENSE`](LICENSE) is the GPL-2.0 text, since a detector that reads one file should read the
stricter one.
