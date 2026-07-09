#!/usr/bin/env bash
# Run this on the Docker host where kairos's appdata volume lives (e.g. the
# Unraid box — see the volume mount in docker-compose.yml: it's whatever
# host path is mounted to /data, by default /mnt/user/appdata/kairos).
#
# Produces a plain-SQL dump of just the tables relevant to library-count /
# scraper-matching analysis (no user/session/watch-history data), written
# next to kairos.db/kairos.conf so it's easy to find and copy elsewhere.
#
# Does NOT touch kairos.db directly: it mounts the appdata dir read-only and
# uses sqlite3's online `.backup` (safe against a live writer, WAL-aware) to
# take a consistent snapshot in a throwaway container, then dumps from that
# snapshot. kairos itself is never paused or locked out.
#
# Usage: ./dump-db-for-analysis.sh [appdata_dir]
set -euo pipefail

APPDATA_DIR="${1:-/mnt/user/appdata/kairos}"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT_FILE="kairos-analysis-${STAMP}.sql"

if [ ! -f "${APPDATA_DIR}/kairos.db" ]; then
    echo "error: ${APPDATA_DIR}/kairos.db not found" >&2
    echo "pass the appdata dir as an argument, e.g.:" >&2
    echo "  $0 /mnt/user/appdata/kairos" >&2
    exit 1
fi

TABLES="media_source media_library source_mapping show movie episode \
scraper_job scanned_file match_candidate item_match_candidate metadata_override"

echo "snapshotting + dumping: ${TABLES}"

docker run --rm \
    -v "${APPDATA_DIR}:/data:ro" \
    -v "${APPDATA_DIR}:/out" \
    alpine:3.20 \
    sh -c "apk add --no-cache sqlite >/dev/null \
        && sqlite3 /data/kairos.db '.backup /tmp/snap.db' \
        && sqlite3 /tmp/snap.db '.dump ${TABLES}' > /out/${OUT_FILE}"

echo "wrote ${APPDATA_DIR}/${OUT_FILE}"
