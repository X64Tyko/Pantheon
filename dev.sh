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

# ── Tunarr server    :8000 ────────────────────────────────────────────────────
tunarr_dir="$root/tunarr"
if [ ! -d "$tunarr_dir/server/node_modules" ]; then
    echo '[dev] installing Tunarr dependencies ...'
    (cd "$tunarr_dir" && pnpm install)
fi
echo '[dev] starting Tunarr server on :8000 ...'
(cd "$tunarr_dir/server" && KAIROS_URL=http://localhost:8080 pnpm dev) &
tunarr_server_pid=$!

# ── Tunarr web    :5180 ───────────────────────────────────────────────────────
echo '[dev] starting Tunarr web on :5180 ...'
(cd "$tunarr_dir/web" && pnpm vite --port 5180) &
tunarr_web_pid=$!

# ── Cleanup on exit ───────────────────────────────────────────────────────────
trap '
    echo "[dev] shutting down ..."
    kill "$tunarr_web_pid" "$tunarr_server_pid" "$kairos_pid" 2>/dev/null
' EXIT INT TERM

# ── Hades    :5173 ────────────────────────────────────────────────────────────
echo '[dev] starting Hades on :5173 ...'
cd "$root/hades"
pnpm dev
