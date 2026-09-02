# SPDX-License-Identifier: MIT
"""Drive the driver's module lifecycle: load it, unload it, ask whether it is in.

Split from hwmon.py, which reads the chip's sysfs surface. Those are two
concerns, and only this one can change the machine: every function in hwmon.py
opens a world-readable file, while load_module() shells out to sudo. Keeping the
privileged calls together makes "what in this package can alter the system"
answerable by reading one short file.

module_resident() lives here rather than with the readers despite being a plain
path check, because the question it answers is about the module, not about the
chip. hwmon.find_hwmon_dir() is the chip-side question, and the two are
deliberately distinct - see its docstring.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

MODULE_NAME = "astral-hwmon"


def module_resident(sysfs_root: str = "/sys") -> bool:
    """Is the module loaded into the kernel?

    Deliberately distinct from hwmon.find_hwmon_dir(): the module can be
    resident and have registered nothing (no card, foreign bus, implausible
    frame). Tests that assert "it declined" need both facts to mean anything -
    which is why this one is unit-tested despite being two lines. If it ever
    returned True unconditionally, every anti-vacuity guard in test_gate.py
    would still pass while proving nothing.

    Note the dash: the module is astral-hwmon but /sys/module spells it with an
    underscore, as does lsmod.
    """
    return Path(sysfs_root).joinpath("module", MODULE_NAME.replace("-", "_")).exists()


def load_module(**params: object) -> None:
    args = [f"{key}={value}" for key, value in params.items()]
    subprocess.run(["sudo", "modprobe", MODULE_NAME, *args], check=True)


def unload_module() -> None:
    subprocess.run(["sudo", "modprobe", "-r", MODULE_NAME], check=False)
