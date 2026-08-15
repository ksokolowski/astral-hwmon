# The native tier

`make test-native` compiles the driver's own pure sources for the host and runs them
against the same evidence the Python mirror uses. No kernel headers, no root, no card.

It exists because `make test` proves nothing about the code that ships. `tests/unit/`
runs `tests/data/frames.jsonl` through `src/astral_oracle/decode.py`; until this tier
existed, `driver/astral_regs.c` was only ever *compiled* in CI, and the PCI gate in
`driver/astral_detect.c` could only be exercised on a machine with the card in it.

## What it covers

- `astral_decode_frame()` and `astral_frame_plausible()` against every frame in
  `tests/data/frames.jsonl`, including the all-zeros reject case.
- `astral_read_frame()`'s transport handling: short reads become `-EIO`, bus errors
  propagate verbatim.
- `astral_is_sensor_adapter()`'s name parsing, including that `adapter 10` does not
  match `adapter 1`.
- `astral_card_matches()` and `astral_card_present()` — the PCI gate — against a fake
  PCI registry, including that `allow_unknown` widens only the model table and never
  the vendor check.
- That `astral_models[]` and `astral_pci_ids[]` still agree, so a hand-edit to either
  half of the `ASTRAL_MODEL_LIST` X-macro is caught.
- That `pci_get_*()` references are balanced by `pci_dev_put()`.

## How the kernel shims work

`tests/native/linux/` holds stand-ins for `<linux/i2c.h>`, `<linux/pci.h>` and friends,
picked up ahead of the real headers by `-Itests/native`. They define only the fields
the driver's detection and decode paths actually read. The driver sources are **not**
modified for the host build.

`to_pci_dev()` is a real `container_of` rather than a stored back pointer, so a struct
layout mistake in the shim shows up as a sanitizer report instead of silently working.
`-Wno-unused-parameter` mirrors `driver/Kbuild`: this tier must never be *stricter*
than the module build, or valid driver code would fail only here.

Fixture fidelity matters as much as it does in the Python tier — an over-simplified
fake sysfs once made a unit test agree with a bug real hardware rejected. If a shim
starts diverging from the real struct in a way the driver can observe, fix the shim.

## Things it deliberately does not check

- **The exact value of `ASTRAL_MV_MIN`.** The bounds tests use the constant symbolically,
  so nudging the floor does not fail them. What *is* pinned is the property the value
  exists for: a pin sagging to 9.0 V under load must still be publishable, because
  silencing that frame would take all six currents with it. Raising the floor back above
  9000 mV fails the run.
- Anything in `astral_hwmon.c`: the probe staging, the notifier and the hwmon surface
  need real kernel infrastructure. Those stay in the hardware tier. Running the module
  under a lockdep/KASAN kernel in a VM is the tool for that layer, not this one.

`astral_detect.c` is `#include`d by `test_detect.c` so the tests can reach its statics,
which is why the Makefile does not also compile it as its own object.

## Verifying the tests can fail

They were checked by mutation — each of these was introduced and confirmed to break the
run: forward instead of reverse pin order; little-endian `u16`; dropping the short-read
check; a plausibility loop that stops after pin 0; matching the adapter name by prefix;
skipping the NVIDIA vendor check; dropping the non-PCI parent check (caught by ASan);
leaking a `pci_dev` reference; `allow_unknown` bypassing the model lookup; and drifting
`astral_pci_ids[]` from `astral_models[]`.
