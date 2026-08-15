# SPDX-License-Identifier: MIT
"""Discovery is tested against a fake sysfs tree, so it runs anywhere."""

from pathlib import Path

from astral_oracle.discover import ASTRAL_SUBSYSTEM_IDS, find_astral_adapter

RTX5090_OC = 0x89E31043


def build_sysfs(root: Path, *, subsystem: int, adapters: dict[int, str]) -> None:
    """Mirror the real layout: /sys/bus/pci/devices/<slot>/i2c-N/name

    Adapter names MUST carry the " at <pci>" suffix the kernel actually writes.
    An earlier version of this fixture omitted it, so an exact-string match
    passed here and failed on real hardware.
    """
    dev = root / "bus" / "pci" / "devices" / "0000:01:00.0"
    dev.mkdir(parents=True)
    (dev / "vendor").write_text("0x10de\n")
    (dev / "subsystem_vendor").write_text(f"0x{subsystem & 0xFFFF:04x}\n")
    (dev / "subsystem_device").write_text(f"0x{subsystem >> 16:04x}\n")
    for number, name in adapters.items():
        adapter = dev / f"i2c-{number}"
        adapter.mkdir()
        (adapter / "name").write_text(f"{name}\n")


def test_finds_adapter_one_on_supported_card(tmp_path: Path) -> None:
    build_sysfs(
        tmp_path,
        subsystem=RTX5090_OC,
        adapters={4: "NVIDIA i2c adapter 1 at 1:00.0", 5: "NVIDIA i2c adapter 2 at 1:00.0"},
    )
    assert find_astral_adapter(str(tmp_path)) == 4


def test_ignores_other_adapters(tmp_path: Path) -> None:
    build_sysfs(
        tmp_path,
        subsystem=RTX5090_OC,
        adapters={9: "NVIDIA i2c adapter 6 at 1:00.0", 10: "NVIDIA i2c adapter 7 at 1:00.0"},
    )
    assert find_astral_adapter(str(tmp_path)) is None


def test_rejects_unknown_subsystem(tmp_path: Path) -> None:
    build_sysfs(tmp_path, subsystem=0xDEAD1043, adapters={4: "NVIDIA i2c adapter 1 at 1:00.0"})
    assert find_astral_adapter(str(tmp_path)) is None


def test_allow_unknown_overrides_allowlist(tmp_path: Path) -> None:
    build_sysfs(tmp_path, subsystem=0xDEAD1043, adapters={4: "NVIDIA i2c adapter 1 at 1:00.0"})
    assert find_astral_adapter(str(tmp_path), allow_unknown=True) == 4


def test_empty_sysfs_returns_none(tmp_path: Path) -> None:
    (tmp_path / "bus" / "pci" / "devices").mkdir(parents=True)
    assert find_astral_adapter(str(tmp_path)) is None


def test_allowlist_matches_windows_project() -> None:
    """The eight published ASUS Astral SKU ids this driver targets."""
    assert len(ASTRAL_SUBSYSTEM_IDS) == 8
    assert ASTRAL_SUBSYSTEM_IDS[RTX5090_OC] == "ROG Astral RTX 5090 OC"


def test_matches_real_sysfs_name_with_pci_suffix(tmp_path: Path) -> None:
    """Regression: the kernel writes "NVIDIA i2c adapter 1 at 1:00.0"."""
    build_sysfs(tmp_path, subsystem=RTX5090_OC, adapters={4: "NVIDIA i2c adapter 1 at 1:00.0"})
    assert find_astral_adapter(str(tmp_path)) == 4


def test_does_not_confuse_adapter_ten_for_adapter_one(tmp_path: Path) -> None:
    build_sysfs(tmp_path, subsystem=RTX5090_OC, adapters={10: "NVIDIA i2c adapter 10 at 1:00.0"})
    assert find_astral_adapter(str(tmp_path)) is None


def test_adapter_index_parsing() -> None:
    from astral_oracle.discover import adapter_index

    assert adapter_index("NVIDIA i2c adapter 1 at 1:00.0") == 1
    assert adapter_index("NVIDIA i2c adapter 10 at 1:00.0") == 10
    assert adapter_index("SMBus PIIX4 adapter port 0 at 0b00") is None


def build_i2c_bus(root: Path, numbers: list[int]) -> None:
    """The flat /sys/bus/i2c/devices view, which lists every adapter."""
    base = root / "bus" / "i2c" / "devices"
    base.mkdir(parents=True, exist_ok=True)
    for n in numbers:
        (base / f"i2c-{n}").mkdir(exist_ok=True)


def test_ignores_acpi_enumerated_adapters(tmp_path: Path) -> None:
    """Regression: /sys/bus/i2c/devices lists i2c-MSFT8000:01 and friends."""
    from astral_oracle.discover import all_adapters

    base = tmp_path / "bus" / "i2c" / "devices"
    base.mkdir(parents=True)
    for name in ("i2c-4", "i2c-10", "i2c-MSFT8000:01", "i2c-ITE8853:00", "4-002b"):
        (base / name).mkdir()
    assert all_adapters(str(tmp_path)) == {4, 10}


def test_nvidia_adapters_lists_only_gpu_buses(tmp_path: Path) -> None:
    from astral_oracle.discover import nvidia_adapters

    build_sysfs(
        tmp_path,
        subsystem=RTX5090_OC,
        adapters={4: "NVIDIA i2c adapter 1 at 1:00.0", 5: "NVIDIA i2c adapter 2 at 1:00.0"},
    )
    build_i2c_bus(tmp_path, [0, 1, 4, 5])
    assert nvidia_adapters(str(tmp_path)) == {4, 5}


def test_find_foreign_adapter_excludes_gpu_buses(tmp_path: Path) -> None:
    from astral_oracle.discover import find_foreign_adapter

    build_sysfs(
        tmp_path,
        subsystem=RTX5090_OC,
        adapters={4: "NVIDIA i2c adapter 1 at 1:00.0", 5: "NVIDIA i2c adapter 2 at 1:00.0"},
    )
    build_i2c_bus(tmp_path, [1, 4, 5])
    assert find_foreign_adapter(str(tmp_path)) == 1


def test_find_foreign_adapter_none_when_all_buses_are_nvidia(tmp_path: Path) -> None:
    from astral_oracle.discover import find_foreign_adapter

    build_sysfs(tmp_path, subsystem=RTX5090_OC, adapters={4: "NVIDIA i2c adapter 1 at 1:00.0"})
    build_i2c_bus(tmp_path, [4])
    assert find_foreign_adapter(str(tmp_path)) is None


def test_find_chipless_nvidia_adapter_skips_the_sensor_bus(tmp_path: Path) -> None:
    from astral_oracle.discover import find_chipless_nvidia_adapter

    build_sysfs(
        tmp_path,
        subsystem=RTX5090_OC,
        adapters={4: "NVIDIA i2c adapter 1 at 1:00.0", 5: "NVIDIA i2c adapter 2 at 1:00.0"},
    )
    build_i2c_bus(tmp_path, [4, 5])
    assert find_chipless_nvidia_adapter(str(tmp_path)) == 5


def test_c_and_python_allowlists_agree() -> None:
    """The driver's X-macro list and this module's dict must not drift.

    CONTRIBUTING says both are updated by hand when a card is added; this is the
    check that makes forgetting one of them fail rather than ship.
    """
    import re

    source = (Path(__file__).resolve().parents[2] / "driver" / "astral_detect.c").read_text()
    rows = re.findall(r'X\(0x([0-9a-fA-F]{4}),\s*0x([0-9a-fA-F]{4}),\s*"([^"]+)"\)', source)
    assert rows, "could not parse the model list out of astral_detect.c"

    from_c = {(int(sd, 16) << 16) | int(sv, 16): name for sv, sd, name in rows}
    assert from_c == ASTRAL_SUBSYSTEM_IDS, (
        f"only in C: {sorted(set(from_c) - set(ASTRAL_SUBSYSTEM_IDS))}, "
        f"only in Python: {sorted(set(ASTRAL_SUBSYSTEM_IDS) - set(from_c))}"
    )
