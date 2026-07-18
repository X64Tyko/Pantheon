#!/usr/bin/env bash
# Mints a real cast token (login -> POST /api/auth/cast-token) and opens
# /tv?castToken=... in a browser tab — automates the manual curl flow used
# throughout this session to test /tv without a real Chromecast device.
# See pantheon-relay/receiver/src/main.ts for what an actual Cast handoff
# does; this reproduces just the "arrive at /tv with a viewer-scoped token"
# part, the same one ProtectedRoute/TvProfileSelect now handle correctly.
set -euo pipefail

HERMES_URL="${HERMES_URL:-http://localhost:8000}"

if ! command -v jq >/dev/null 2>&1; then
  echo "error: jq is required (JSON parsing)" >&2
  exit 1
fi

read -rp "Username: " PANTHEON_USERNAME
read -rsp "Password: " PANTHEON_PASSWORD
echo

echo "[cast.sh] logging in as $PANTHEON_USERNAME..." >&2
LOGIN_BODY=$(jq -n --arg u "$PANTHEON_USERNAME" --arg p "$PANTHEON_PASSWORD" '{username:$u, password:$p}')
LOGIN_RESPONSE=$(curl -sf -X POST "$HERMES_URL/api/auth/login" \
  -H "Content-Type: application/json" \
  -d "$LOGIN_BODY") || { echo "[cast.sh] login request failed" >&2; exit 1; }

TOKEN=$(echo "$LOGIN_RESPONSE" | jq -r '.token // empty')
if [ -z "$TOKEN" ]; then
  echo "[cast.sh] login response had no token: $LOGIN_RESPONSE" >&2
  exit 1
fi

echo "[cast.sh] minting cast token..." >&2
CAST_RESPONSE=$(curl -sf -X POST "$HERMES_URL/api/auth/cast-token" \
  -H "Authorization: Bearer $TOKEN") || { echo "[cast.sh] cast-token mint failed" >&2; exit 1; }

CAST_TOKEN=$(echo "$CAST_RESPONSE" | jq -r '.token // empty')
if [ -z "$CAST_TOKEN" ]; then
  echo "[cast.sh] cast-token response had no token: $CAST_RESPONSE" >&2
  exit 1
fi

CAST_URL="$HERMES_URL/tv?castToken=$CAST_TOKEN"
echo "[cast.sh] cast URL: $CAST_URL"

if command -v xdg-open >/dev/null 2>&1; then
  xdg-open "$CAST_URL" >/dev/null 2>&1 &
  disown
elif command -v open >/dev/null 2>&1; then
  open "$CAST_URL"
else
  echo "[cast.sh] no browser opener found (xdg-open/open) — open the URL above manually"
fi
