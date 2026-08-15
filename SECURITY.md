# Security policy

## Reporting

Use GitHub's private vulnerability reporting on this repository
(**Security → Report a vulnerability**). That keeps the report private until there is
something to say publicly. Please do not open a public issue for anything you believe is
exploitable.

There is no SLA here. This is one person's out-of-tree driver, not a product.

## Supported versions

The latest tagged release only. There are no maintenance branches.

## What this code can and cannot do

Worth stating plainly, because the hardware involved is unforgiving.

**The driver never writes to the chip.** It implements no hwmon `write` callback,
`is_visible()` returns `0444` unconditionally, and no module parameter changes that. The
capability is absent from the code rather than merely unused. This matters more than usual: the
sensor sits on a live GPU's I2C bus shared with the RGB controller, and a write to an unknown
register on an unknown device can destroy hardware. The same rule binds the exploration
tooling — `docs/measurements/2026-08-14/regscan.py` has no write path and must not acquire one.

**The PCI identity check runs before any I2C traffic**, always. `allow_unknown=1` relaxes only
the model allowlist; `bus=N` only selects among the matched card's own adapters. Neither can
point the driver at a foreign bus, which is deliberate: address `0x2b` on a motherboard SMBus
belongs to some entirely different device.

**Everything the driver publishes is world-readable and read-only**, and `astral-guard` needs
no privileges at all — it opens sysfs attributes for reading and nothing else. Running it as
root buys nothing.

**`astral-guard` is not a safety device.** It reports; it never throttles, caps or powers
anything off. It measures current, while the hazard is contact heating, so a green verdict is
not a temperature claim. Do not build a safety case on it.

## Scope

In scope: memory safety in the driver or the guard, anything that lets an unprivileged user
influence what the driver writes to the bus (there should be nothing), the PCI gate being
bypassable, and the guard reporting OK when it cannot actually read the sensor.

Out of scope: the absence of thresholds and alarms in the driver — that is a deliberate design
decision documented in `docs/GUARD-DESIGN.md` §1, not an oversight. Also out of scope is
anything requiring root, since root can already write to any I2C device via `i2c-dev`.
