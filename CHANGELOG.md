# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [Unreleased]

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
- **Testing**: Kairos test suite grew to 429 tests, Hades to 135, Hermes to 19, and Hephaestus to 26 (609 total across the stack), including new coverage for chapter detection, image URL generation, cross-source settings-merge matching, and — most recently — scheduler determinism (`scheduler/test_natural_advance.cpp`, `scheduler/test_determinism_regression.cpp`; see Fixed below).

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
