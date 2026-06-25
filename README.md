# Pantheon

Turn your Plex library into a network of 24/7 TV channels — each with its own block schedule, rerun rules, and live EPG.

**Kairos** is the scheduling engine. It decides what plays when, tracks watch history, manages rerun pools, and generates XMLTV/M3U for any IPTV client. **Hades** is the management UI, served by Kairos on the same port. **Tunarr** handles the FFmpeg stream pipeline. Together they replace the static schedule approach used by dizqueTV and vanilla Tunarr with a live, state-driven rule engine.

![Hades channel grid](.github/screenshot.png)

---

## Requirements

- Docker + Docker Compose
- A running Plex Media Server with at least one library
- *(Optional)* NVIDIA GPU for hardware-accelerated transcoding in Tunarr

---

## Quick start

**1. Copy the compose file**

```yaml
# docker-compose.yml — or use Unraid Compose Manager:
# https://raw.githubusercontent.com/X64Tyko/Pantheon/master/docker-compose.yml
```

Download it or paste the contents from this repo's `docker-compose.yml`.

**2. Set your paths**

Open `docker-compose.yml` and update two volume lines under `kairos`:

```yaml
volumes:
  - /mnt/user/appdata/kairos:/data          # config + database
  - /mnt/user/Media:/media:ro               # your media root (read-only)
  - /mnt/user/Media/Filler:/downloads       # yt-dlp download destination
```

The `/downloads` path should be a folder your Plex already watches so downloaded filler appears automatically after a library scan.

**3. No NVIDIA GPU?**

Remove these three lines from the `tunarr` service:

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

Open **http://your-server:8081** — that's Hades.

---

## First-time setup (5 minutes)

### 1. Add your Plex server

Go to **Sources → Add Source**. Enter your Plex URL and token, then test the connection. Your token is in Plex's web UI under **Account → Troubleshooting → XML** or via [plex.tv/claim](https://www.plex.tv/claim/).

### 2. Add libraries and sync

After saving the source, go back into it and add the libraries you want Kairos to know about (TV Shows, Movies). Then hit **Sync** — Kairos fetches all episode metadata and file paths from Plex. Large libraries take a few minutes; progress is visible in the **Activity** log.

### 3. Set the path map

Plex reports file paths from its own perspective (e.g. `/data/TV/...`). If your media is mounted at a different path in the Kairos container (e.g. `/media`), add a path map under **Sources → your source → Path Maps**:

```
/data  →  /media
```

This tells Tunarr's FFmpeg where to actually find the files.

### 4. Create a channel

**Channels → New Channel**. Give it a name, number, and timezone. Leave the seed alone — it makes shuffles reproducible (same week = same lineup).

### 5. Add a block

Open the channel, click **Add Block**. Choose days, a start time, and a block type. Add a show or movie list to it. Save.

### 6. Preview the EPG

Click **Preview** on the channel page. You'll see the resolved grid — what plays when, with filler filling the gaps. Adjust priority and timing until it looks right, then **Save Channel**.

---

## Connecting IPTV clients

Kairos exposes standard M3U and XMLTV endpoints:

| Endpoint | URL |
|---|---|
| M3U playlist | `http://your-server:8081/playlist.m3u` |
| XMLTV guide | `http://your-server:8081/epg.xml` |

These point directly to Tunarr's stream URLs for each channel.

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

Kairos includes yt-dlp. Go to **Downloads** in Hades, paste a URL (YouTube playlist, video, etc.), and set the destination path. Point that path at a folder Plex watches so the content shows up as a library item you can add to a filler list.

---

## Environment variables

| Variable | Default | Description |
|---|---|---|
| `KAIROS_SYNC_THREADS` | `4` | Parallel connections when syncing episode metadata |
| `KAIROS_DEBUG_EPG` | *(unset)* | Set to `1` for verbose EPG scheduling logs |
| `KAIROS_URL` | *(tunarr only)* | URL Tunarr uses to reach Kairos — set automatically in compose |

---

## Building from source

```bash
# C++ (requires cmake, ninja, g++)
cmake -B cmake-build-debug -G Ninja
cmake --build cmake-build-debug

# Hades UI (requires node, pnpm)
cd hades
pnpm install
pnpm dev        # dev server on :5173
pnpm build      # production build → ../kairos/ui-dist
```

Run both with `./dev.sh` — starts Kairos on `:8080` and Hades dev server on `:5173`.

---

## Architecture

```
Kairos (C++ · :8080)
  ├─ Scheduling engine (blocks, cursors, rerun pools)
  ├─ EPG materialization (XMLTV + M3U)
  ├─ Plex library sync
  └─ Hades UI (served as static files)

Tunarr (:8000)
  └─ FFmpeg stream pipeline → HLS / HDHomeRun output
       calls Kairos /now, /next, /played
```

Kairos publishes what to play and when. Tunarr renders it. They communicate over the Docker Compose network — nothing else needed.

---

## Status

Early access. Plex is the only supported media source today; Jellyfin, Emby, and local filesystem sources are in progress. The streaming pipeline requires Tunarr; a native stream server (removing the Tunarr dependency) is planned.

Issues and feedback welcome.
