# Contributing

## Setup

```sh
make setup     # uv sync, and points git at scripts/hooks
```

`make setup` runs `git config core.hooksPath scripts/hooks`, which installs the `commit-msg`
gate. Do this once per clone — it is not automatic.

Run `make qa` before pushing. Hardware changes want `make test-hw` too, on a real card.

## Commits

**Conventional Commits**, enforced by the hook:
`feat|fix|docs|refactor|test|chore|build|ci|perf|style|revert(scope): description`

**A single author on every commit** — no `Co-Authored-By` trailers and no "generated with"
footers, in commit messages, code, or pull requests. The hook rejects both. Describe the
change; the tools used to make it are not the record.

Commit at meaningful milestones, not per subtask.

## The two implementations must stay independent

`src/astral_oracle/decode.py` and `driver/astral_regs.c` decode the same register block. They
are deliberately separate implementations, and the hardware agreement test is meaningful only
because neither is derived from the other.

**Do not** generate one from the other, share constants between them, or "deduplicate" them.
If you change the decoding in one, change it in the other by hand and let the agreement test
tell you whether you got it right. The same applies to `discover.py` and `astral_detect.c`.

## Evidence citations

Every magic constant — register offsets, `0x2b`, the plausibility bounds, the subsystem ids —
carries a comment naming the measurement or witness it came from. A constant whose provenance
nobody recorded is a constant nobody can safely change later.

When you add a value, say where it came from. When you change one, say what you measured.

## Safety rules

These are not style preferences:

- **The driver never writes to the chip.** No write callback, every attribute `0444`. Adding a
  writable attribute is not a feature request, it is a defect.
- **The PCI-id check runs before any I2C traffic**, and no module parameter may bypass it.
- **The plausibility gate is not overridable.** A multi-byte read on this bus can silently
  return all zeros, which looks exactly like a perfectly safe connector.
- **No thresholds.** hwmon's `curr*_max` / `curr*_crit` exist, but publishing a limit that
  nothing enforces invites userspace to believe something is watching. They belong with a
  guard, not with a sensor driver.

## Hardware facts that cost time to learn

Each of these was found by running against real hardware, not by review:

- The adapter name is `NVIDIA i2c adapter 1 at 1:00.0`. An exact `strcmp` against
  `"NVIDIA i2c adapter 1"` matches nothing. Parse the index — and require a word boundary, or
  `adapter 10` matches too.
- **hwmon numbering is asymmetric**: voltages are zero-indexed (`in0`–`in5` = pins 1–6),
  currents are one-indexed (`curr1`–`curr6`). Assuming `in1..in6` reads the wrong pin and runs
  off the end.
- **A pin transposition is invisible at idle.** The six pins sit within ~80 mA of each other
  idling, inside the 150 mA agreement tolerance, so a fully transposed driver passes
  `test_driver_matches_oracle` — verified by mutation. Under the burn the spread is
  600–920 mA. That is why `test_pin_mapping_is_not_transposed` drives the load and asserts
  the spread it got: any agreement check on idle data proves nothing about mapping.
- `i2c_del_adapter()` unregisters and frees every non-dummy child client. On `rmmod nvidia`
  our client is freed behind our back — hence devm-scoping everything to the client and
  handling `BUS_NOTIFY_DEL_DEVICE`.
- `BUS_NOTIFY_ADD_DEVICE` fires from inside `device_add()` with the device lock held and the
  adapter unable to carry a transfer. There is no `ADDED_DEVICE` event. Defer to a workqueue.
- Two deadlocks live in this area: taking `astral_lock` before the `dev->type` filter
  self-deadlocks via the ADD notification `i2c_new_client_device()` fires, and holding it
  across attach self-deadlocks via the DEL notification from the failure path's
  `i2c_unregister_device()`.
- **Userspace is not locked out of `0x2b`** while the driver is bound, because no `i2c_driver`
  is bound to the client. That is what lets the agreement test sample both paths at once.
- `/sys/bus/i2c/devices` also lists ACPI adapters (`i2c-MSFT8000:01`, `i2c-ITE8853:00`) whose
  id does not parse as an integer.
- `uevent` is `0644` on every sysfs device, so a "nothing is writable" sweep must exclude
  sysfs plumbing rather than allowlist attribute prefixes.
- Test fixtures must mirror reality exactly. An over-simplified fake sysfs (missing the
  ` at <pci>` suffix) made a unit test agree with a bug that real hardware rejected.

## Adding a card

A new subsystem id needs evidence: `lspci -nn -d 10de:` from the owner, plus `sensors` output
showing six plausible readings taken with `allow_unknown=1`.

Add it to the `ASTRAL_MODEL_LIST` X-macro in `driver/astral_detect.c` — that one list feeds
both the runtime allowlist and the PCI table udev autoloads on, so they cannot drift. Then add
it to `ASTRAL_SUBSYSTEM_IDS` in `src/astral_oracle/discover.py` and to the README table,
marking whether it is verified or inherited. `test_c_and_python_allowlists_agree` fails if you
update the C side and forget the Python one.

## Bumping the version

`dkms.conf`'s `PACKAGE_VERSION` is authoritative for packaging — the Makefile derives
`DKMS_VERSION` from it, so those two can never drift. Two other copies must be bumped by
hand in the same commit:

- `MODULE_VERSION()` in `driver/astral_hwmon.c` (what `modinfo` reports)
- `version` in `pyproject.toml` (the harness, which is not distributed)

`scripts/check_release_version.sh v1.2.3` asserts a tag agrees with all three, and CI runs it
on any `v*` tag — so a forgotten bump fails the release instead of shipping a module whose
`modinfo` contradicts the release name. Run it before tagging.

Add a `CHANGELOG.md` entry in the same commit. `astral-guard --version` needs no bump: the
Makefile bakes it in from `dkms.conf`.

## CI

`.github/workflows/ci.yml` runs on push and pull request: the commit-message rules (the same
`scripts/hooks/commit-msg` the local hook uses, applied to every commit in the range), `make
qa`, a check that the hardware tier *skips* on a card-less runner, and a warning-free build of
the driver against real kernel headers.

CI is where the commit rules are actually enforced — git does not clone hooks, so the local
gate only exists for people who ran `make setup`.

It runs on GitHub-hosted runners, so there is nothing to register and nothing to keep alive.
