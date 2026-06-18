#!/usr/bin/env bash
set -e
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── Build C++ ─────────────────────────────────────────────────────────────────
echo '[dev] building all targets ...'
cmake --build "$root/cmake-build-debug"

# ── Kairos    :8080 ───────────────────────────────────────────────────────────
kairos="$root/cmake-build-debug/kairos/kairos"
if [ ! -f "$kairos" ]; then
    echo "Error: kairos not found at $kairos" >&2
    exit 1
fi
echo '[dev] starting Kairos on :8080 ...'
"$kairos" &
kairos_pid=$!

# ── Cleanup on exit ───────────────────────────────────────────────────────────
trap '
    echo "[dev] shutting down ..."
    kill "$kairos_pid" 2>/dev/null
' EXIT INT TERM

# ── Hades    :5173 ────────────────────────────────────────────────────────────
echo '[dev] starting Hades on :5173 ...'
cd "$root/hades"
pnpm dev
