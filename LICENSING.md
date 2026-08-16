# Licensing

Copyright (c) 2026 Krzysztof Sokołowski.

This project is licensed by component, because one of its components has no
choice in the matter and the others do.

The root `LICENSE` file is the plain GPL-2.0 text, and the two licences live
side by side in `LICENSES/` — the same layout the kernel itself uses for
`COPYING`. Automated licence detectors read a single root file and report one
answer, so the root carries the stricter of the two: anyone who takes this at
its word and treats the whole tree as GPL-2.0 is on safe ground, whereas the
reverse would not be. The per-file `SPDX-License-Identifier` tags remain
authoritative, and this document explains why the split exists.

## `driver/` — GPL-2.0-only

The kernel module is **GPL-2.0-only** (`LICENSES/GPL-2.0.txt`).

It binds ten symbols exported with `EXPORT_SYMBOL_GPL` — among them
`devm_hwmon_device_register_with_info`, `i2c_new_client_device` and
`bus_register_notifier` — so a module declaring anything the kernel does not
recognise as free software is treated as `Proprietary`, taints the kernel and
fails to resolve them. It would not load. `MODULE_LICENSE("GPL v2")` matches
the SPDX tag on every file under `driver/`.

The module is only ever useful as part of Linux, where the GPL governs
regardless. Licensing it permissively would grant a right that has no use here
except taking the work somewhere it need not be given back.

## `guard/`, `src/`, `tests/`, `tools/`, `docs/` — MIT

Everything outside the kernel module is **MIT** (`LICENSES/MIT.txt`).

`astral-guard` includes no kernel header — only `<dirent.h>`, `<stddef.h>`,
`<stdio.h>`, `<stdlib.h>`, `<string.h>` and `<time.h>` — and reads sysfs files
the way `cat` does. Nothing ties its licence to the kernel's, so it is
permissive on purpose: the measured protocol and the rule thresholds come from
public sources and measurement, and anyone should be able to build on them.

## Per-file tags

Every source file carries an `SPDX-License-Identifier` naming its licence.
That tag is authoritative for the file it appears in; this document only
explains why the split exists.
