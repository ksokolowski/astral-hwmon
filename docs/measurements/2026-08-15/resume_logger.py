#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Sample the astral-hwmon node across a suspend/resume cycle.

Re-discovers the node by hwmon `name` on every tick on purpose: if the client
is torn down and re-attached, the hwmon index changes, and a logger that
cached the path would report a permanent failure instead of the truth.

One line per tick:
  wall  mono  state  detail
where state is one of
  NONODE  no chip named astral12vhpwr in /sys/class/hwmon
  EIO     the node exists but a channel read failed (errno given)
  OK      all twelve channels read; values follow
"""

import errno
import os
import time

ROOT = "/sys/class/hwmon"
NAME = "astral12vhpwr"
HZ = 10.0
DURATION = float(os.environ.get("DURATION", "150"))
OUT = os.environ["OUT"]


def find_node():
    try:
        entries = sorted(os.listdir(ROOT))
    except OSError:
        return None
    for e in entries:
        p = os.path.join(ROOT, e)
        try:
            with open(os.path.join(p, "name")) as fh:
                if fh.read().strip() == NAME:
                    return p
        except OSError:
            continue
    return None


def read_all(node):
    """Twelve channels. Asymmetric numbering: in0-in5, curr1-curr6."""
    mv, ma = [], []
    for pin in range(6):
        with open(os.path.join(node, f"in{pin}_input")) as fh:
            mv.append(int(fh.read().strip()))
        with open(os.path.join(node, f"curr{pin + 1}_input")) as fh:
            ma.append(int(fh.read().strip()))
    return mv, ma


def main():
    end = time.monotonic() + DURATION
    with open(OUT, "w", buffering=1) as log:
        while time.monotonic() < end:
            wall = time.time()
            mono = time.monotonic()
            node = find_node()
            if node is None:
                log.write(f"{wall:.3f} {mono:.3f} NONODE -\n")
            else:
                try:
                    mv, ma = read_all(node)
                    log.write(
                        f"{wall:.3f} {mono:.3f} OK {os.path.basename(node)} "
                        f"mv={','.join(map(str, mv))} ma={','.join(map(str, ma))}\n"
                    )
                except OSError as exc:
                    name = errno.errorcode.get(exc.errno, str(exc.errno))
                    log.write(
                        f"{wall:.3f} {mono:.3f} EIO {os.path.basename(node)} "
                        f"errno={name}\n"
                    )
            time.sleep(1.0 / HZ)


if __name__ == "__main__":
    main()
