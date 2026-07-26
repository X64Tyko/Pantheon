"""Tests for extract_changelog_section.py.

extract_changelog_section.py is a standalone script (guarded by `if __name__ ==
"__main__"`), not a package -- there's no __init__.py anywhere under .github/scripts/
and no test infra elsewhere in this repo to build on. It's imported here as a plain
module; pytest's default "rootless" import mode adds this file's own directory to
sys.path when collecting it (no conftest.py / sys.path shim needed), and importing it
this way never triggers the __main__ guard, so `main()` only runs when a test calls it
explicitly.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

import extract_changelog_section as ecs

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = Path(__file__).resolve().parent / "extract_changelog_section.py"
REAL_CHANGELOG = REPO_ROOT / "CHANGELOG.md"

# ---------------------------------------------------------------------------
# extract_section
# ---------------------------------------------------------------------------

SYNTHETIC_CHANGELOG = """\
# Changelog

## [Unreleased]

### Added
- unreleased item

## [0.3.0] - 2026-08-01

### Added
- something new
  continued here

## [0.2.1] - 2026-07-25

### Fixed
- **Bug one**: description that wraps
  onto a second line
  and a third line.
- **Bug two**: another bullet
  with one continuation.

### Added
- new feature

## [0.2.0] - 2026-07-22

### Added
- old feature
"""


@pytest.fixture
def synthetic_changelog(tmp_path: Path) -> Path:
    path = tmp_path / "CHANGELOG.md"
    path.write_text(SYNTHETIC_CHANGELOG)
    return path


def test_extract_section_middle_of_file(synthetic_changelog: Path) -> None:
    section = ecs.extract_section(str(synthetic_changelog), "0.2.1")
    assert section[0] == ""
    assert "### Fixed" in section
    assert "- **Bug one**: description that wraps" in section
    assert "- **Bug two**: another bullet" in section
    assert "### Added" in section
    assert "- new feature" in section
    # Bounded correctly: nothing from the neighboring sections leaked in.
    joined = "\n".join(section)
    assert "something new" not in joined
    assert "old feature" not in joined


def test_extract_section_last_in_file_reaches_eof(synthetic_changelog: Path) -> None:
    section = ecs.extract_section(str(synthetic_changelog), "0.2.0")
    assert "### Added" in section
    assert "- old feature" in section
    joined = "\n".join(section)
    assert "new feature" not in joined


def test_extract_section_version_not_found_returns_empty_list(
        synthetic_changelog: Path,
) -> None:
    assert ecs.extract_section(str(synthetic_changelog), "9.9.9") == []


def test_extract_section_prefix_version_not_falsely_matched(tmp_path: Path) -> None:
    # A version string that is a prefix of another version's heading (e.g.
    # requesting "0.2.1" while "## [0.2.10]" also exists) must not match the
    # longer heading -- `line.startswith(target)` is safe here only because
    # `target` includes the closing "]", so "## [0.2.1]" does not start with
    # "## [0.2.1" + missing "]"... this test proves it holds for the real code path.
    changelog = tmp_path / "CHANGELOG.md"
    changelog.write_text(
        "## [0.2.10] - 2026-09-01\n"
        "\n"
        "- item belonging to 0.2.10 only\n"
        "\n"
        "## [0.2.1] - 2026-07-25\n"
        "\n"
        "- item belonging to 0.2.1 only\n"
    )
    section = ecs.extract_section(str(changelog), "0.2.1")
    joined = "\n".join(section)
    assert "item belonging to 0.2.1 only" in joined
    assert "0.2.10" not in joined


# ---------------------------------------------------------------------------
# reflow
# ---------------------------------------------------------------------------


def test_reflow_joins_wrapped_continuation_lines() -> None:
    lines = [
        "- **Bug one**: description that wraps",
        "  onto a second line",
        "  and a third line.",
    ]
    result = ecs.reflow(lines)
    assert result == "- **Bug one**: description that wraps onto a second line and a third line.\n"


def test_reflow_does_not_bleed_between_bullets() -> None:
    lines = [
        "- first bullet",
        "  continuation of first",
        "- second bullet",
        "  continuation of second",
    ]
    result = ecs.reflow(lines)
    assert result == "- first bullet continuation of first\n- second bullet continuation of second\n"


def test_reflow_subsection_heading_is_its_own_line() -> None:
    lines = [
        "### Added",
        "- first item",
        "  wrapped",
        "### Fixed",
        "- second item",
    ]
    result = ecs.reflow(lines)
    assert result == "### Added\n- first item wrapped\n### Fixed\n- second item\n"


def test_reflow_preserves_blank_lines_as_paragraph_breaks() -> None:
    lines = [
        "### Added",
        "",
        "- first item",
        "",
        "- second item",
    ]
    result = ecs.reflow(lines)
    assert result == "### Added\n\n- first item\n\n- second item\n"


def test_reflow_strips_leading_and_trailing_blank_lines() -> None:
    lines = ["", "", "- only item", "  wrapped", "", ""]
    result = ecs.reflow(lines)
    assert result == "- only item wrapped\n"
    assert not result.startswith("\n")
    assert not result.endswith("\n\n")


def test_reflow_empty_input_does_not_crash() -> None:
    result = ecs.reflow([])
    assert result == "\n"


def test_reflow_input_of_only_blank_lines_does_not_crash() -> None:
    result = ecs.reflow(["", "", ""])
    assert result == "\n"


# ---------------------------------------------------------------------------
# main() / CLI behavior
# ---------------------------------------------------------------------------


def test_main_wrong_arg_count_exits_2(monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture) -> None:
    monkeypatch.setattr(sys, "argv", ["extract_changelog_section.py", "only-one-arg"])
    rc = ecs.main()
    captured = capsys.readouterr()
    assert rc == 2
    assert "usage:" in captured.err
    assert captured.out == ""


def test_main_missing_version_exits_1_with_message(
        monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture, synthetic_changelog: Path
) -> None:
    monkeypatch.setattr(
        sys, "argv", ["extract_changelog_section.py", str(synthetic_changelog), "9.9.9"]
    )
    rc = ecs.main()
    captured = capsys.readouterr()
    assert rc == 1
    assert f"No '## [9.9.9]' section found in {synthetic_changelog}" in captured.err
    assert captured.out == ""


def test_main_valid_run_exits_0_with_reflowed_stdout(
        monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture, synthetic_changelog: Path
) -> None:
    monkeypatch.setattr(
        sys, "argv", ["extract_changelog_section.py", str(synthetic_changelog), "0.2.1"]
    )
    rc = ecs.main()
    captured = capsys.readouterr()
    assert rc == 0
    assert captured.err == ""
    assert "- **Bug one**: description that wraps onto a second line and a third line.\n" in captured.out
    assert "- **Bug two**: another bullet with one continuation.\n" in captured.out


def test_cli_subprocess_end_to_end_exit_codes(synthetic_changelog: Path) -> None:
    # Exercises the real `if __name__ == "__main__": raise SystemExit(main())`
    # entrypoint as an actual subprocess, not just the main() function in-process.
    result = subprocess.run(
        [sys.executable, str(SCRIPT_PATH), str(synthetic_changelog), "0.2.1"],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0
    assert "- **Bug one**: description that wraps onto a second line and a third line." in result.stdout

    bad_args = subprocess.run(
        [sys.executable, str(SCRIPT_PATH)],
        capture_output=True,
        text=True,
    )
    assert bad_args.returncode == 2
    assert "usage:" in bad_args.stderr


# ---------------------------------------------------------------------------
# Integration: real excerpts from this repo's own root CHANGELOG.md
# ---------------------------------------------------------------------------


@pytest.mark.skipif(not REAL_CHANGELOG.exists(), reason="repo root CHANGELOG.md not found")
def test_real_changelog_0_2_1_section_reflows_wrapped_bullet_correctly() -> None:
    """The 0.2.1 section's first bullet (internal-token auth bug) hard-wraps
    "...isPublicPath had" onto one line and "`path.ends_with(...)`..." onto the
    next -- confirm reflow rejoins that boundary into a single logical line."""
    section = ecs.extract_section(str(REAL_CHANGELOG), "0.2.1")
    assert section, "expected a '## [0.2.1]' section in the real CHANGELOG.md"
    result = ecs.reflow(section)

    assert (
            "`isPublicPath` had `path.ends_with(\"/played\")` with no method restriction"
            in result
    )
    # No line of the reflowed output should still be a bare mid-sentence wrap
    # fragment -- every non-blank line is a subsection heading or a full bullet.
    for line in result.splitlines():
        if line == "":
            continue
        assert line.startswith("### ") or line.startswith("- "), (
            f"line was not rejoined into its bullet/heading: {line!r}"
        )


@pytest.mark.skipif(not REAL_CHANGELOG.exists(), reason="repo root CHANGELOG.md not found")
def test_real_changelog_unreleased_section_reflows_self_describing_bugfix_bullet() -> None:
    """The 'Unreleased' section's own bullet describing this exact rendering bug
    hard-wraps "...through a small" / "script (`.github/scripts/extract_changelog_
    section.py`)..." across a line boundary -- a fittingly self-referential case
    to confirm the join works on the real file."""
    section = ecs.extract_section(str(REAL_CHANGELOG), "Unreleased")
    assert section, "expected a '## [Unreleased]' section in the real CHANGELOG.md"
    result = ecs.reflow(section)

    assert (
            "runs the extracted section through a small\n" not in result
    )
    assert (
            "through a small\nscript" not in result
    )
    assert "through a small script (`.github/scripts/extract_changelog_section.py`)" in result
