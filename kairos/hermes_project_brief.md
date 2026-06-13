# Hermes — IPTV Schedule Engine
## Project Brief for Claude Code

---

## What we're building

**Hermes** is a C++ REST service that acts as a dynamic scheduling engine for IPTV channels. It sits alongside a lightly modified Tunarr instance and replaces Tunarr's static schedule precalculation with a live, stateful rule engine.

Tunarr (an open source IPTV server built on Node/Fastify) handles FFmpeg stream pipeline, HDHomeRun emulation, HLS output, and Plex/Jellyfin integration. Hermes owns everything about *what plays when* — channel state, block rules, watched tracking, and EPG materialization.

The two communicate over a simple REST API on a shared Docker Compose network.

---

## Why this exists

Existing IPTV scheduling tools (Tunarr, dizqueTV) are **static schedule compilers** — they precalculate a full year of programming and store it. They have no concept of:

- What has actually aired (watched state)
- Promoting aired episodes into a shuffle rerun pool
- Premier blocks that advance a show cursor only when that block actually runs
- Dynamic EPG that reflects real playback state

Hermes solves this with a lightweight in-memory rule engine backed by SQLite, exposing a REST API that Tunarr calls instead of reading from its own schedule DB.

---

## Tech stack

- **Language:** C++20
- **Build system:** CMake + Ninja
- **HTTP server:** Drogon or cpp-httplib (TBD — prefer whichever has simpler async model for this scale)
- **Database:** SQLite via SQLiteCpp wrapper
- **JSON:** nlohmann/json
- **HTTP client (for Plex/Jellyfin sync):** libcurl or cpp-httplib client
- **Containerization:** Docker, multi-stage build on ubuntu:24.04
- **Orchestration:** Docker Compose (deployed on Unraid)

---

## System architecture

```
┌─────────────────────────────────┐
│  Hermes (C++)                   │
│                                 │
│  RuleEngine                     │
│    - evaluates block rules      │
│    - returns next ProgramEntry  │
│                                 │
│  ChannelState (per channel)     │
│    - show cursors               │
│    - watched map (ep → aired_at)│
│    - dirty flag → EPG cache     │
│                                 │
│  EPGMaterializer                │
│    - projects N hours forward   │
│    - cached, invalidated on play│
│    - serializes to XMLTV        │
│                                 │
│  REST API (port 8080)           │
│  SQLite (persists state)        │
└──────────────┬──────────────────┘
               │ HTTP (docker network)
┌──────────────▼──────────────────┐
│  Tunarr (modified, Node/TS)     │
│  - calls Hermes for what to play│
│  - reports playback completion  │
│  - FFmpeg pipeline unchanged    │
│  - HDHomeRun / HLS / M3U output │
└──────────────┬──────────────────┘
               │
┌──────────────▼──────────────────┐
│  IPTV Clients                   │
│  Plex / TiViMate / Channels DVR │
└─────────────────────────────────┘
```

---

## REST API — endpoints Tunarr calls on Hermes

| Method | Endpoint | When called | Returns |
|--------|----------|-------------|---------|
| GET | `/channels/{id}/now` | Stream start or after episode ends | Current program: file path, start offset, expected end time |
| GET | `/channels/{id}/next` | ~30s before current episode ends | Next program: file path, wall-clock start time |
| POST | `/channels/{id}/played` | Episode completes playback | Ack. Hermes records `aired_at`, advances cursor, invalidates EPG cache |
| GET | `/epg` | XMLTV poll from IPTV clients (every 1–4 hrs) | Full XMLTV XML derived live from channel state |
| GET | `/channels/{id}/epg` | Per-channel EPG request | Next 24–72 hrs of programming |
| GET | `/channels` | UI / admin | List all channels with current state |
| POST | `/channels` | UI / admin | Create channel |
| PUT | `/channels/{id}/blocks` | UI / admin | Set block rules for a channel |

---

## Data model

### Channel
```
channel_id      TEXT PRIMARY KEY
name            TEXT
number          INTEGER  -- IPTV channel number
```

### Show
```
show_id         TEXT PRIMARY KEY
title           TEXT
plex_rating_key TEXT
content_rating  TEXT     -- TV-Y, TV-Y7, TV-G, TV-PG, TV-14, TV-MA
```

### Episode
```
episode_id      TEXT PRIMARY KEY
show_id         TEXT REFERENCES show
season          INTEGER
episode         INTEGER
title           TEXT
file_path       TEXT     -- absolute path as Plex/system sees it
duration_ms     INTEGER
```

### Block
```
block_id        TEXT PRIMARY KEY
channel_id      TEXT REFERENCES channel
block_type      TEXT     -- 'episode', 'premier', 'filler'
day_mask        INTEGER  -- bitmask: Sun=1 Mon=2 Tue=4 ... Sat=64
start_time      TEXT     -- "07:00"
end_time        TEXT     -- "12:00"  NULL = fills remaining time
priority        INTEGER  -- lower = evaluated first
config_json     TEXT     -- block-type-specific rules as JSON
```

### BlockShow (shows assigned to a block)
```
block_id        TEXT REFERENCES block
show_id         TEXT REFERENCES show
position        INTEGER  -- ordering within the block
advancement     TEXT     -- 'sequential', 'shuffle', 'rerun_shuffle'
```

### PlayHistory
```
history_id      INTEGER PRIMARY KEY AUTOINCREMENT
episode_id      TEXT REFERENCES episode
channel_id      TEXT REFERENCES channel
block_id        TEXT REFERENCES block
aired_at        INTEGER  -- unix timestamp
```

---

## Block types

### Episode block
- Pool of shows assigned to this block
- Optional day mask + time window — if absent, fills 24/7
- Advancement strategies:
  - `sequential` — next episode in order, per show cursor
  - `shuffle` — random episode from unwatched pool, exhausts before rerun
  - `rerun_shuffle` — random from watched pool only
- Auto-promotes: when unwatched pool exhausts, seamlessly draws from watched pool in shuffle order
- Never advances a show's "premier cursor" — only the episode block cursor

### Premier block
- Fixed day mask + time window (e.g. Sat 7:00–12:00)
- Ordered show list with per-slot configuration:
  - Which show plays in this slot
  - Duration (30 min, 60 min)
  - Advancement: `sequential` (advances the show's primary cursor) or `shuffle`
- Sequential shows in Premier blocks advance their cursor **only when this block runs**
- Marks episodes aired_at on completion → they enter the rerun pool for Episode blocks on same channel
- Example: DuckTales at Sunday 4 PM sequential, weekday slots linked as rerun_shuffle

### Filler block
- Activates when an episode ends before its slot boundary
- Pulls shortest-fit content from filler pool
- Duration-aware: fills exactly to next wall-clock boundary
- Filler content never marked as "aired" for rerun tracking purposes

---

## EPG materialization

The EPGMaterializer projects forward from the current wall clock time:

1. For each channel, walk block list ordered by priority
2. For each time window, find which block owns it (day mask + time range match)
3. Call that block's strategy `next()` to get the episode — **without advancing state** (read-only projection)
4. Accumulate `ProgramEntry { episode, channel, wall_clock_start, wall_clock_end }` until horizon reached
5. Cache result with 15-minute TTL
6. Invalidate cache early when `/played` is called (dirty flag per channel)

The XMLTV serializer walks the `vector<ProgramEntry>` and emits standard XMLTV XML. IPTV clients (Plex, TiViMate, Channels DVR) poll this endpoint every 1–4 hours.

---

## Playback flow (one episode completing, next starting)

```
1. Tunarr FFmpeg finishes episode
2. Tunarr → POST /channels/{id}/played  { episode_id, duration_actual_ms }
3. Hermes:
     - INSERT INTO play_history
     - Advance show cursor in ChannelState
     - Set dirty flag → invalidate EPG cache for this channel
4. Tunarr → GET /channels/{id}/next  (already prefetched 30s earlier)
5. Hermes:
     - Evaluate block rules for current wall clock time
     - Call block strategy next() → returns episode
     - Return { file_path, wall_clock_start, duration_ms }
6. Tunarr hands file_path to FFmpeg
7. Stream continues seamlessly
```

---

## Docker setup

### Folder structure
```
project-root/
├── hermes/
│   ├── src/
│   │   ├── main.cpp
│   │   ├── api/          -- REST endpoint handlers
│   │   ├── scheduler/    -- RuleEngine, ChannelState, EPGMaterializer
│   │   ├── db/           -- SQLite schema, queries
│   │   ├── sync/         -- Plex/Jellyfin library sync
│   │   └── model/        -- Channel, Block, Episode, ProgramEntry structs
│   ├── CMakeLists.txt
│   └── Dockerfile
├── tunarr/
│   └── Dockerfile        -- or just reference published image
├── docker-compose.yml
└── data/
    ├── hermes/           -- SQLite DB persists here (host volume)
    └── tunarr/           -- Tunarr config persists here
```

### docker-compose.yml (target)
```yaml
services:
  hermes:
    build: ./hermes
    ports:
      - "8080:8080"
    volumes:
      - ./data/hermes:/data
      - /mnt/user/Media:/media:ro   # Unraid media path, read-only
    restart: unless-stopped

  tunarr:
    image: chrisbenincasa/tunarr:latest
    ports:
      - "8000:8000"
    environment:
      - HERMES_URL=http://hermes:8080
    volumes:
      - ./data/tunarr:/config/tunarr
      - /mnt/user/Media:/media:ro   # same mount, same path
    depends_on:
      - hermes
    restart: unless-stopped
```

### Multi-stage Dockerfile for Hermes
```dockerfile
# Stage 1: build
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y \
    cmake ninja-build g++ \
    libsqlite3-dev libcurl4-openssl-dev \
    nlohmann-json3-dev

WORKDIR /src
COPY . .
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build

# Stage 2: runtime
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    libsqlite3-0 libcurl4

COPY --from=builder /src/build/hermes /usr/local/bin/hermes

EXPOSE 8080
CMD ["/usr/local/bin/hermes", "--db", "/data/hermes.db", "--port", "8080"]
```

---

## Development workflow

Local dev (no Docker needed):
```bash
cd hermes
cmake -B build -G Ninja
cmake --build build
./build/hermes --db ./data/hermes.db --port 8080
```

Full stack with Docker Compose:
```bash
docker compose up --build
```

Hermes only:
```bash
docker compose up --build hermes
```

---

## What to build first (suggested order)

1. **Project scaffold** — CMakeLists.txt, folder structure, basic HTTP server returning 200 on `/health`
2. **SQLite schema** — all tables above, migration system, SQLiteCpp wrapper
3. **Data model structs** — Channel, Block, BlockShow, Episode, ProgramEntry in C++
4. **Plex sync** — HTTP client hitting Plex API, populating episode table with file paths and metadata
5. **RuleEngine** — block evaluation, Episode block with sequential advancement
6. **ChannelState** — in-memory cursor map, watched set, dirty flag, SQLite persistence
7. **REST API** — `/now`, `/next`, `/played` endpoints (the three Tunarr needs)
8. **EPGMaterializer** — forward projection, XMLTV serialization
9. **Premier block type** — day mask + time window, per-slot advancement
10. **Filler block type** — duration-aware gap filling
11. **Dockerfile + Compose** — containerize, test full stack
12. **Tunarr integration** — modify Tunarr to call Hermes instead of its own scheduler

---

## Key engineering notes

**File paths:** Hermes stores absolute file paths as Plex reports them. Both Hermes and Tunarr must mount the media library at the **same path** in Docker so paths are valid when Tunarr's FFmpeg opens them.

**Watched state vs. EPG projection:** EPGMaterializer must be read-only — projecting forward does not advance cursors. Only `POST /played` advances state. This is critical for EPG accuracy.

**PTS continuity:** This is Tunarr's problem, not Hermes's. Hermes returns file paths and timing metadata. Tunarr/FFmpeg handles seamless concatenation.

**Channel restart recovery:** On Tunarr restart, it calls `GET /channels/{id}/now`. Hermes returns the currently-scheduled program including a start offset (how far into the episode we should be at wall-clock now). Tunarr seeks FFmpeg to that offset. The channel recovers without a full restart from the beginning.

**SQLite concurrency:** Single writer (the Hermes process), multiple readers fine. WAL mode recommended. No need for Postgres at this scale.

**The owner:** Cody — Senior Gameplay Systems Engineer at Epic Games (UEFN), C++20/Unreal background, familiar with ECS patterns, job systems, and deterministic simulation. The RuleEngine and ChannelState design should feel natural given Trinyx Engine work. This is a side project under the Dim Lit Studios umbrella.
