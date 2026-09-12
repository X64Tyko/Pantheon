#!/usr/bin/env python3
"""Generate the 5 other docker-compose.*.yml release variants from the base
docker-compose.yml (CPU-only, no Cloudflare Tunnel).

All 6 files are meant to be identical except for which lines are commented
out, so a user switching hwaccel/cloudflared later can just copy the toggled
block into their own already-customized file instead of re-fetching a
different one. Run this after editing docker-compose.yml so the variants
never drift out of sync with it:

    python3 scripts/generate-compose-variants.py
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASE = ROOT / "docker-compose.yml"

# (commented text, uncommented text) — hephaestus hwaccel toggle lines.
# Hardcoded literal pairs rather than a generic comment-stripping rule since
# it's a small fixed set and this makes exactly what's being toggled explicit
# and easy to verify against docker-compose.yml by eye.
NVIDIA_LINES = [
    ("      # - HEPH_HW_ACCEL=nvidia",
     "      - HEPH_HW_ACCEL=nvidia"),
    ("      # - NVIDIA_VISIBLE_DEVICES=all",
     "      - NVIDIA_VISIBLE_DEVICES=all"),
    ("      # - NVIDIA_DRIVER_CAPABILITIES=video,compute,utility",
     "      - NVIDIA_DRIVER_CAPABILITIES=video,compute,utility"),
    ("    # runtime: nvidia   # NVIDIA — requires NVIDIA Container Toolkit on the host",
     "    runtime: nvidia   # NVIDIA — requires NVIDIA Container Toolkit on the host"),
]
VAAPI_LINES = [
    ("      # - HEPH_HW_ACCEL=amd",
     "      - HEPH_HW_ACCEL=amd"),
    ("      # - HEPH_VAAPI_DEV=/dev/dri/renderD128   # override if your render node differs",
     "      - HEPH_VAAPI_DEV=/dev/dri/renderD128   # override if your render node differs"),
    ("    # devices:          # AMD — expose the GPU render node",
     "    devices:          # AMD — expose the GPU render node"),
    ("    #   - /dev/dri/renderD128:/dev/dri/renderD128",
     "      - /dev/dri/renderD128:/dev/dri/renderD128"),
]

CLOUDFLARED_START = "  # cloudflared:"
CLOUDFLARED_END = "  #   restart: on-failure"


def apply_pairs(lines: list[str], pairs: list[tuple[str, str]]) -> list[str]:
    remaining = {c: u for c, u in pairs}
    out = []
    for line in lines:
        if line in remaining:
            out.append(remaining[line])
        else:
            out.append(line)
    missing = [c for c, _ in pairs if c not in lines]
    if missing:
        raise SystemExit(
            f"generate-compose-variants.py: expected line(s) not found in "
            f"docker-compose.yml (base file changed shape?):\n  "
            + "\n  ".join(missing)
        )
    return out


def uncomment_cloudflared(lines: list[str]) -> list[str]:
    start = lines.index(CLOUDFLARED_START)
    end = lines.index(CLOUDFLARED_END)
    out = list(lines)
    for i in range(start, end + 1):
        line = out[i]
        if not line.startswith("  # "):
            raise SystemExit(
                f"generate-compose-variants.py: cloudflared block line {i+1} "
                f"doesn't start with the expected '  # ' prefix:\n  {line!r}"
            )
        out[i] = "  " + line[4:]
    return out


def write_variant(name: str, lines: list[str]) -> None:
    path = ROOT / name
    path.write_text("\n".join(lines) + "\n")
    print(f"wrote {name}")


def main() -> None:
    base_lines = BASE.read_text().splitlines()

    variants = {
        "docker-compose.cloudflared.yml": uncomment_cloudflared(base_lines),
        "docker-compose.nvenc.yml": apply_pairs(base_lines, NVIDIA_LINES),
        "docker-compose.vaapi.yml": apply_pairs(base_lines, VAAPI_LINES),
    }
    variants["docker-compose.nvenc.cloudflared.yml"] = uncomment_cloudflared(
        variants["docker-compose.nvenc.yml"]
    )
    variants["docker-compose.vaapi.cloudflared.yml"] = uncomment_cloudflared(
        variants["docker-compose.vaapi.yml"]
    )

    for name, lines in variants.items():
        write_variant(name, lines)


if __name__ == "__main__":
    main()
