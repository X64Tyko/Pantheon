# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [Unreleased]

### Added
- **Hades**: Activity page's System Resources card now plots per-component RAM usage (Kairos/Hermes/Hephaestus) alongside CPU, in the same slot style as the GPU row. Data was already collected/normalized (`ram_bytes` on each component's metrics); this was a frontend-only addition.

## [0.2.0] - 2026-07-22

*Alpha complete — every Phase 1 item in [docs/ROADMAP.md](docs/ROADMAP.md) is now checked off.*

### Added
- **API Reference**: Published a full endpoint-by-endpoint reference for the Kairos admin/management API at [docs/API.html](https://x64tyko.github.io/Pantheon/API.html), covering all ~185 routes across sources, libraries, channels, scheduling, scrapers, and more.
- **Cross-Source Merge**: Manual link/merge for duplicate shows and movies discovered across Plex/Jellyfin/local sources; the detail panel now shows all linked sources.
- **Metadata Writeback**: Bi-directional writeback of confirmed matches to Plex/Jellyfin, gated strictly on human-confirmed matches (never auto- or force-accepted).
- **Show Specials Linking**: `episode.linked_movie_id` joins a show's OVA/special to a movie-library file so it plays via a live join while the movie stays independently scheduled; auto-scan is opt-in per show via a `find_specials` toggle.
- **Chapter Detection & Review**: Automatic ad-break/intro-outro chapter detection, plus a read-only "Chapters" tab on the Review page for visual QA against the actual video. Not yet wired into scheduling.
- **Skip Scraping**: Per-library, per-show, and per-movie flag to exempt items from the metadata scraper pipeline.
- **Hard Sync**: "Hard Sync" (per-source) and "Hard Sync All" actions that force a full re-sync, bypassing incremental diffing.
- **Scraper Priority**: Per-library scraper priority ordering.
- **Admin Tools**: "Download Debug Dump" for admins, and a metrics dashboard on the Activity page.
- **Roku Channel**: Native BrightScript Roku channel (`pantheon-roku`) with device pairing, a Hermes-driven command channel, and native HLS playback with watch-progress sync. Code-complete; not yet verified on physical hardware.
- **Chromecast Relay**: `pantheon-relay`, a small HTTPS-hosted Cast receiver bootstrap, so Chromecast sending works from a LAN-only Hades install without a Cloudflare Tunnel.
- **Multi-User & Parental Controls**: Admin/viewer roles with per-user max TV/movie/channel rating ceilings, enforced on both API and stream requests.
- **First-Time Setup Tour**: Guided in-app tour for the first admin account, mirrored in [`docs/First-Time Setup.html`](docs/First-Time%20Setup.html).
- **TV (10-foot) UI**: Dedicated `TvHome`/`TvLibrary`/`TvGuideSection` surface with a live Guide grid and preview player, tuned for remote navigation.
- **Hermes**: Added `/api/images/proxy` endpoint to safely fetch external media art (like Plex thumbnails) from public domains, bypassing browser Private Network Access (PNA) and CORS restrictions.
- **Hades**: Implemented `mediaUrl` helper to automatically route external URLs through the Hermes image proxy.
- **Playlists & Home Shelves**: Playlists can be static (an explicit item list) or "smart" (items periodically recomputed from a stored filter expression — the same query language `/api/shows`/`/api/movies` already use), and optionally linked to a Plex playlist/collection or Jellyfin/Emby playlist for two-way sync (pull via `plex-sync`/`source-sync`) and writeback (`push`, reconciling adds/removes in both directions). A Home page shelf is just a smart playlist flagged `show_on_home`, orderable and optionally scoped to a seasonal `MM-DD` active window — no separate shelf-definition table. Playlists also support portable JSON export/import and a channel block can be materialized into one directly (`POST /api/channels/:id/blocks/:bid/playlist`).
- **Scraper-Derived Tags**: TMDB keywords, AniList tags (spoiler-flagged tags dropped), and Wikidata's "main subject" claim (P921) now populate a `tags` field on shows/movies, filterable via `tag:` and usable directly in smart playlists/Home shelves — e.g. an auto-populating, seasonally-windowed `tag:christmas` shelf with no manual per-item labeling.
- **Audio/Subtitle Language Filtering & External Subtitles**: Embedded audio/subtitle track languages (from the existing sync-time ffprobe pass) and external subtitle sidecar files (`.srt`/`.ass`/`.ssa`/`.vtt`, matched to their video by filename convention) are both filterable (`audio_language:`, `subtitle_language:`) and selectable from the player's track menu. Sidecar tracks direct-play as WebVTT muxed into the HLS output where the format allows it; bitmap subtitle formats (PGS/DVD/DVB) burn in via an overlay filter instead and force a transcode.
- **Per-Client Direct-Play Capability**: Clients can declare their real decode capabilities (`POST /stream/client-capabilities`) so Hephaestus can decide direct-play vs. transcode against what a device actually supports instead of a fixed h264/aac allowlist. pantheon-android is the first client to declare (`MediaCodecList`-backed), on session restore/login/profile-switch.
- **TV Manifest Theming**: `GET /api/tv/manifest` now includes a `theme` block of style tokens generated from Hades' own `index.css`, so native manifest-driven renderers (pantheon-android, Hades' `/tv` route) stay visually in sync with the main design system without hand-duplicating token values.
- **Testing**: Kairos test suite grew to 569 tests, Hades to 133, Hermes to 25, and Hephaestus to 30 (757 total across the stack), including new coverage for chapter detection, image URL generation, cross-source settings-merge matching, playlist/smart-playlist behavior, and — most recently — scheduler determinism (`scheduler/test_natural_advance.cpp`, `scheduler/test_determinism_regression.cpp`; see Fixed below).

### Fixed
- **Scheduler Determinism**: Rerun-mode show position and Smart-shuffle cooldown no longer derive from live `play_history`/`filler_play_history` queries — both are now carried entirely in the per-week anchor snapshot, so a background schedule refresh happening between two preview requests (or two `generate()` calls with the same seed) can no longer change what a later preview shows. This was the root cause of previews looking different after simply navigating away and back.
- **Scheduler Determinism**: Weekly anchors are now captured at *every* week boundary a projection crosses, not just the first — a multi-week preview or the divergence checker's probe used to silently lose cursor/RNG continuity past week one. Anchor keys are also now computed in the channel's own timezone rather than naive UTC; a channel not on UTC could otherwise have its cursor/RNG state land under the wrong key and get reset or overwritten.
- **Scheduler Determinism**: Sequential/rerun episode advancement now tracks a stable per-episode watermark instead of a raw list index, so a library scan that backfills a missing episode out of numeric order (e.g. episode 3 arrives before episode 2) no longer skips or duplicates an episode once the gap fills in — it self-heals and catches up on the very next occurrence instead of waiting for a full rerun cycle.
- **Scraper Matching**: Added a hard year filter on TMDB/TVDB search results, rewrote `parseTitle`, and fixed a URL-encoding gap that broke search entirely for titles containing a literal `%`.
- **Scraper Matching**: Cross-source items could be silently double-matched with conflicting per-source library settings; matching now merges settings into one canonical, conflict-checked row per item.
- **Local Source**: Removed an early-return in `guessLibraryType` that was inflating the review queue and misclassifying libraries; `library_type` is now directly editable as a fallback.
- **Hades TV**: Harmonized TV mode visuals with the main Hades design system, including hero banner vignettes and glassmorphism effects.
- **Hades TV**: Relocated the library button and optimized vertical shelf spacing for better 10-foot navigation.
- **CI**: Fixed `NOT_BUILT` test errors by scoping CTest execution to specific targets in component workflows.
- **Scheduler**: A channel cursor insert referencing an episode already deleted out from under it (a stale watermark surfaced from a deserialized anchor snapshot after orphan cleanup) used to throw an uncaught FK-constraint error that rolled back the entire commit and 500'd `GET /api/channels/:id/epg` for the whole channel; now caught, logged, and skipped without failing the rest of the commit.
- **Background Threads**: Replaced the `std::thread(...).detach()` idiom used for fire-and-forget background work (Hephaestus, Hermes, Kairos) with a shared `TaskRegistry` (`shared/thread/`) that tracks spawned threads instead of abandoning them, reaping finished ones lazily and joining any still outstanding at process teardown — fixes latent lifetime issues around `shared_ptr`-captured `ChannelSession`/`ChannelBroadcaster` state in detached threads.

## [v0.1.0-alpha.1] - 2026-07-07

### Added
- **Multi-Source Scraper Priority:** Link library items to multiple scrapers (TMDB, TVDB, AniDB, IMDb) and define custom priority ordering.
- **Unified Logging:** Centralized logging for both backend services and frontend console errors into `data/kairos.log` with automatic rotation.
- **In-Memory Rerun Logic:** Optimized rerun pool generation and smart shuffle logic using memory-first projection, reducing database load.
- **Language Weighting:** Scraper match scores now receive a bonus if the result matches the library's preferred language.
- **Governance:** Established project interaction model with `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, and restrictive licensing.
- **Issue Templates:** Standardized bug report template requiring logs and environment details.

### Fixed
- Intermittent match confirmation failures by decoupling DB transactions from external API calls.
- Google Cast SDK initialization and absolute URL resolution for mobile/desktop browsers.
- AniDB image proxying by implementing `Referer` header spoofing.
- Settings page 401 spam by introducing a dedicated public settings endpoint.

### Changed
- Refactored frontend assets into manual chunks to optimize initial load times.
- Relocated deep-dive technical documentation to `docs/ARCHITECTURE.md`.
- Restricted full system configuration access to admin-only roles.
