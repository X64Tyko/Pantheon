# Pantheon

A media platform built around three pillars:

- **Media library management** — sync from Plex, Jellyfin, Emby, or local filesystem; scrape metadata from TMDB/TVDB/AniDB; manage collections, filler lists, and content catalogues across sources.
- **IPTV scheduling** — build 24/7 channels with block schedules, rerun rules, filler, bumpers, and live EPG. Connect any IPTV client or route through XTeve into Plex DVR.
- **Media player** — direct playback within Hades, in progress.

**Kairos** is the scheduling and library engine. **Hades** is the management UI. **Hermes** is the public-facing server (streams, HDHomeRun emulation, EPG, media player API). **Hephaestus** handles the FFmpeg transcoding pipeline.

---

## Requirements

- Docker + Docker Compose
- A media server with at least one library — **Plex, Jellyfin, Emby**, or a **local filesystem** source are all supported
- *(Optional)* NVIDIA GPU for hardware-accelerated transcoding

---

## Quick start

**1. Copy the compose file**

```yaml
# docker-compose.yml — or use Unraid Compose Manager:
# https://raw.githubusercontent.com/X64Tyko/Pantheon/master/docker-compose.yml
```

Download it or paste the contents from this repo's `docker-compose.yml`.

**2. Set your paths**

Open `docker-compose.yml` and update the volume lines under `kairos`:

```yaml
volumes:
  - /mnt/user/appdata/kairos:/data          # config + database
  - /mnt/user/Media:/media:ro               # your media root (read-only)
  - /mnt/user/Media/Filler:/downloads       # yt-dlp download destination
```

**3. No NVIDIA GPU?**

Remove these lines from the `hephaestus` service:

```yaml
runtime: nvidia
environment:
  - NVIDIA_VISIBLE_DEVICES=all
  - NVIDIA_DRIVER_CAPABILITIES=video,compute,utility
```

**4. Start the stack**

```bash
docker compose up -d
```

Open **http://your-server:8000** — that's the Hades management UI, served through Hermes.

Direct Kairos access (API, debugging) is on **:8081**.

---

## First-time setup (5 minutes)

### 1. Add a media source

Go to **Sources → Add Source**. Pantheon supports:

| Source type | Auth |
|---|---|
| **Plex** | Server URL + token (from Account → Troubleshooting → XML) |
| **Jellyfin** | Server URL + API key + user ID |
| **Emby** | Server URL + API key + user ID |
| **Local** | Mount path (no auth needed) |

Enter the connection details and test the connection.

### 2. Add libraries and sync

After saving the source, go back into it and add the libraries you want Kairos to know about (TV Shows, Movies). Hit **Sync** — Kairos fetches all episode metadata and file paths. Large libraries take a few minutes; progress is visible in the **Activity** log.

### 3. Set the path map

Your media server reports file paths from its own perspective (e.g. `/data/TV/...`). If your media is mounted at a different path inside the Kairos container (e.g. `/media`), add a path map under **Sources → your source → Path Maps**:

```
/data  →  /media
```

This tells the transcoder where to actually find the files.

### 4. Create a channel

**Channels → New Channel**. Give it a name, number, and timezone. Leave the seed alone — it makes shuffles reproducible (same week = same lineup).

### 5. Add a block

Open the channel, click **Add Block**. Choose days, a start time, and a block type. Add a show or movie list to it. Save.

### 6. Preview the EPG

Click **Preview** on the channel page. You'll see the resolved grid — what plays when, with filler filling the gaps. Adjust priority and timing until it looks right, then **Save Channel**.

---

## Connecting IPTV clients

Hermes exposes standard M3U and XMLTV endpoints:

| Endpoint | URL |
|---|---|
| M3U playlist | `http://your-server:8000/playlist.m3u` |
| XMLTV guide | `http://your-server:8000/epg.xml` |
| HDHomeRun | auto-discovered on the LAN |

### XTeve → Plex

If you're routing through XTeve into Plex DVR:

1. Add the M3U and XMLTV URLs to XTeve
2. Map channels in XTeve
3. In Plex: **Settings → Live TV & DVR → Add Device** → point at XTeve
4. After any schedule change: refresh XTeve's XMLTV cache, then refresh Plex's guide

### Direct IPTV clients

TiViMate, Channels DVR, and most IPTV apps can consume the M3U and XMLTV endpoints directly.

---

## Scheduling model

Channels are built from **blocks** — recurring time slots on chosen days, each with a content list and an advancement rule.

| Concept | What it does |
|---|---|
| **Block** | Owns a time window on specific days. Higher priority wins when blocks overlap. |
| **Advancement** | How the block walks its list: `sequential`, `shuffle`, `smart_shuffle`, `rerun_shuffle`, `rerun_smart` |
| **Cursor** | Bookmark inside a show — global (shared everywhere), channel (shared on this channel), or block (private). |
| **Premier block** | Airs each show's next first-run episode in order. Aired episodes enter the rerun pool. |
| **Filler** | Patches gaps between programs so the channel never goes dark. Duration-aware: fits clips to the seam. |
| **Bumpers** | Intro/outro branding clips at block boundaries, plus interstitials every N programs. |

For a full visual breakdown of how all these interact, see the scheduling diagram in `/docs`.

---

## Downloading filler

Kairos includes yt-dlp. Go to **Downloads** in Hades, paste a URL (YouTube playlist, video, etc.), and set the destination path. Point that path at a folder your media server watches so the content shows up as a library item you can add to a filler list.

---

## Environment variables

### Kairos

| Variable | Default | Description |
|---|---|---|
| `KAIROS_SYNC_THREADS` | `min(8, cpu count)` | Parallel connections when fetching episode metadata. The compose file sets this to `4` explicitly. |
| `KAIROS_DEBUG` | *(unset)* | Set to `1` for verbose sync, ffprobe, and scraper logs. Equivalent to toggling **Sync Debug Logging** in Settings. |
| `KAIROS_DEBUG_EPG` | *(unset)* | Set to `1` for verbose EPG scheduling logs. Equivalent to **EPG Debug Logging** in Settings. |

Both debug flags are also controllable at runtime without restart via **Settings → Diagnostics** in Hades — changes are persisted to the database.

### Hermes

| Variable | Description |
|---|---|
| `KAIROS_URL` | URL Hermes uses to reach Kairos — set automatically in compose (`http://kairos:8080`) |
| `HEPHAESTUS_URL` | URL Hermes uses to reach Hephaestus — set automatically in compose |
| `HADES_URL` | URL of the Hades frontend — set automatically in compose |

---

## Building from source

```bash
# C++ engine (requires cmake, ninja, g++)
cmake -B kairos/build -G Ninja -S .
cmake --build kairos/build

# Hades UI (requires node, pnpm)
cd hades
pnpm install
pnpm dev        # dev server on :5173 (proxies API to kairos on :8080)
pnpm build      # production build
```

Run both together with `./dev.sh` — starts Kairos on `:8080` and the Hades dev server on `:5173`.

---

## Architecture

```
                         ┌──────────────────────────────┐
                         │  Hermes (:8000)               │
                         │  Public endpoint              │
                         │  M3U · XMLTV · HDHomeRun      │
                         │  Hades UI reverse proxy       │
                         └──────────┬───────────────────┘
                                    │
               ┌────────────────────┼────────────────────┐
               ▼                    ▼                    ▼
  ┌────────────────────┐  ┌──────────────────┐  ┌─────────────┐
  │  Kairos (:8080)    │  │  Hephaestus      │  │  Hades      │
  │  Scheduling engine │  │  FFmpeg pipeline │  │  React UI   │
  │  EPG materialization│  │  HLS · TS output │  │             │
  │  Library sync      │  │                  │  │             │
  │  Plex/Jellyfin/    │  │                  │  │             │
  │  Emby/Local        │  │                  │  │             │
  └────────────────────┘  └──────────────────┘  └─────────────┘
```

Kairos publishes what to play and when. Hephaestus renders it to a stream. Hermes ties everything together into a single user-facing endpoint.

---

## Status

Beta.

| Area | State |
|---|---|
| Library sync (Plex, Jellyfin, Emby, local) | Working |
| Metadata scraping (TMDB, TVDB, AniDB) | Working |
| IPTV channel scheduling + EPG | Working |
| Stream delivery (Hermes + Hephaestus) | In progress |
| Media player | In progress |
| HDHomeRun emulation | In progress |

Issues and feedback welcome.
