# SPDX-License-Identifier: MIT
"""The commit-msg hook is the mechanical enforcement of two repo rules."""

import subprocess
from pathlib import Path

HOOK = Path(__file__).resolve().parents[2] / "scripts" / "hooks" / "commit-msg"


def run_hook(tmp_path: Path, message: str) -> subprocess.CompletedProcess[str]:
    msg_file = tmp_path / "COMMIT_EDITMSG"
    msg_file.write_text(message, encoding="utf-8")
    return subprocess.run([str(HOOK), str(msg_file)], capture_output=True, text=True, check=False)


def test_accepts_conventional_subject(tmp_path: Path) -> None:
    assert run_hook(tmp_path, "feat: add pin decoding\n").returncode == 0


def test_accepts_scoped_subject(tmp_path: Path) -> None:
    assert run_hook(tmp_path, "fix(driver): reject zero frames\n").returncode == 0


def test_rejects_non_conventional_subject(tmp_path: Path) -> None:
    result = run_hook(tmp_path, "Add pin decoding\n")
    assert result.returncode != 0
    assert "conventional" in result.stderr.lower()


def test_rejects_coauthor_trailer(tmp_path: Path) -> None:
    """A single author is the record; any co-author trailer is rejected."""
    result = run_hook(tmp_path, "feat: x\n\nCo-Authored-By: Someone <someone@example.com>\n")
    assert result.returncode != 0
    assert "co-authored-by" in result.stderr.lower()


def test_rejects_generated_with_footer(tmp_path: Path) -> None:
    result = run_hook(tmp_path, "feat: x\n\nGenerated with some-tool v3\n")
    assert result.returncode != 0


def test_ignores_comment_lines(tmp_path: Path) -> None:
    assert run_hook(tmp_path, "feat: x\n\n# Co-Authored-By: Someone\n").returncode == 0
