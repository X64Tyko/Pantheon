#!/bin/bash
# Regenerates the auto-generated checklist sections of docs/ROADMAP.md from
# live GitHub Issues/Milestones state — the milestones are the actual source
# of truth; this just formats a readable snapshot of them into the doc.
# Hand-written narrative (headers, Focus lines, footnotes, and all of Phase 1
# which predates milestone tracking) is untouched — only the content between
# each `roadmap-sync` marker pair is replaced.
#
# Run via .github/workflows/roadmap-sync.yml on issue/milestone changes, or
# locally: `bash .github/scripts/generate_roadmap.sh` (requires `gh` auth).
set -euo pipefail

REPO="X64Tyko/Pantheon"
ROADMAP="docs/ROADMAP.md"

# Only enhancement-labeled issues count as roadmap items — bug issues live in
# the same milestones (a milestone tracks "everything needed to call this
# phase done") but aren't roadmap-shaped checklist entries.
render_milestone() {
  local milestone="$1"
  gh issue list --repo "$REPO" --milestone "$milestone" --state all --label enhancement \
    --json number,title,state,url --limit 200 \
    --jq 'sort_by(.number)[] | "- [\(if .state == "CLOSED" then "x" else " " end)] [\(.title)](\(.url)) (#\(.number))"'
}

replace_section() {
  local marker="$1" content="$2"
  awk -v marker="$marker" -v content="$content" '
    $0 ~ ("<!-- roadmap-sync:start " marker " -->") { print; print content; in_section=1; next }
    $0 ~ ("<!-- roadmap-sync:end " marker " -->")   { in_section=0 }
    !in_section { print }
  ' "$ROADMAP" > "$ROADMAP.tmp" && mv "$ROADMAP.tmp" "$ROADMAP"
}

replace_section "beta" "$(render_milestone "Beta Complete")"
replace_section "v1"   "$(render_milestone "V1.0")"

echo "docs/ROADMAP.md regenerated from live GitHub issue/milestone state."
