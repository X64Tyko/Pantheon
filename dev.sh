#!/usr/bin/env bash
set -e
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="$root/cmake-build-debug"

# ── Build C++ ─────────────────────────────────────────────────────────────────
echo '[dev] building all targets ...'
cmake --build "$build"

# ── Verify binaries ───────────────────────────────────────────────────────────
for svc in kairos hephaestus hermes; do
    bin="$build/$svc/$svc"
    if [ ! -f "$bin" ]; then
        echo "Error: $svc binary not found at $bin" >&2
        exit 1
    fi
done

# ── Kairos    :8080 ───────────────────────────────────────────────────────────
echo '[dev] starting Kairos on :8080 ...'
"$build/kairos/kairos" &
kairos_pid=$!

# ── Hephaestus :8082 ─────────────────────────────────────────────────────────
echo '[dev] starting Hephaestus on :8082 ...'
"$build/hephaestus/hephaestus" \
    --kairos-url http://localhost:8080 \
    --port 8082 &
hephaestus_pid=$!

# ── Hermes   :8000 ────────────────────────────────────────────────────────────
# Points the UI proxy at the Vite dev server so hot reload works through :8000.
echo '[dev] starting Hermes on :8000 ...'
"$build/hermes/hermes" \
    --kairos-url     http://localhost:8080 \
    --hephaestus-url http://localhost:8082 \
    --hades-url      http://localhost:5173 \
    --port 8000 &
hermes_pid=$!

# ── Cleanup on exit ───────────────────────────────────────────────────────────
trap '
    echo "[dev] shutting down ..."
    kill "$kairos_pid" "$hephaestus_pid" "$hermes_pid" 2>/dev/null
    wait "$kairos_pid" "$hephaestus_pid" "$hermes_pid" 2>/dev/null
' EXIT INT TERM

# ── Hades    :5173 ────────────────────────────────────────────────────────────
echo '[dev] installing Hades dependencies ...'
cd "$root/hades"
pnpm install

echo ''
echo '  Hades UI (hot reload):    http://localhost:5173'
echo '  Full pipeline via Hermes: http://localhost:8000'
echo '  Kairos direct:            http://localhost:8080'
echo ''
echo '[dev] starting Hades on :5173 ...'
pnpm dev
