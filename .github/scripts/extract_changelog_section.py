#!/usr/bin/env python3
"""Extract one version's section from CHANGELOG.md and reflow it for GitHub Release notes.

CHANGELOG.md is hard-wrapped at ~120 columns for readable diffs/terminal viewing --
fine for GitHub's own file-blob renderer (which collapses a soft line break like any
CommonMark reader), but the renderer GitHub uses for Releases (same one as issues/PRs)
instead turns every one of those soft breaks into a literal <br>, so the wrapped prose
comes out looking broken as release notes. This joins each list item's wrapped
continuation lines back into one logical line before handing the section to
`gh release create`/`edit`.

Assumes (true of this file today; a future author changing that should update this
too): every bullet is a single paragraph -- no nested sub-lists, no blank line in the
middle of one item's own text, no fenced code blocks.
"""
import sys


def extract_section(changelog_path: str, version: str) -> list[str]:
    target = f"## [{version}]"
    section: list[str] = []
    in_section = False
    with open(changelog_path) as f:
        for raw_line in f:
            line = raw_line.rstrip("\n")
            if line.startswith("## ["):
                if in_section:
                    break
                if line.startswith(target):
                    in_section = True
                continue
            if in_section:
                section.append(line)
    return section


def reflow(lines: list[str]) -> str:
    out: list[str] = []
    current: str | None = None

    def flush() -> None:
        if current is not None:
            out.append(current)

    for line in lines:
        stripped = line.strip()
        if stripped == "":
            flush()
            current = None
            out.append("")
        elif line.startswith("### ") or line.startswith("- "):
            flush()
            current = line
        else:
            current = stripped if current is None else current + " " + stripped
    flush()
    return "\n".join(out).strip("\n") + "\n"


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: extract_changelog_section.py <CHANGELOG.md> <version>", file=sys.stderr)
        return 2
    changelog_path, version = sys.argv[1], sys.argv[2]
    section = extract_section(changelog_path, version)
    if not section:
        print(f"No '## [{version}]' section found in {changelog_path}", file=sys.stderr)
        return 1
    sys.stdout.write(reflow(section))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
