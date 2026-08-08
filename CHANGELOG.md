# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [Unreleased]

### Added

- **In-memory HLS segment cache + cross-viewer deduplication** (Hephaestus + Hermes): opt-in RAM cache for HLS
  segments, off by default (`HEPH_SEGMENT_CACHE_MB`/`HERMES_SEGMENT_CACHE_MB`, both 0) so low-power deployments see
  no change unless explicitly enabled. Hephaestus caches segment bytes to skip repeat disk reads, with live-channel
  buckets capped to a handful of resident segments (no benefit to holding more — a channel never seeks backward) and
  a startup log suggesting a real budget computed from each configured channel's actual quality settings. Hermes gets
  its own independent cache plus request coalescing ("singleflight"): concurrent requests for the same not-yet-cached
  segment now collapse into one upstream fetch to Hephaestus instead of one per viewer. Making cross-viewer
  deduplication actually work required fixing a real gap: the capability-bucketed channel-viewer playlist handed
  every viewer their own `viewer_session_id`-scoped segment URLs for identical bytes, so Hermes had no way to
  recognize two viewers were requesting the same content — that playlist's segment URIs are now rewritten to the
  same content-addressed, channel+bucket-scoped URL for every viewer (new bucket-explicit segment route on
  Hephaestus), which is what actually lets the cache/coalescer hit across viewers on a shared live channel.
- **`.ogv` file support** (Kairos): the local library scanner now recognizes `.ogv` (Ogg Theora/Vorbis) files as
  video and ingests them like any other container. They aren't eligible for the direct-stream channel bucket
  (Theora/Vorbis aren't in the direct-streamable codec set) but play normally through the existing software
  transcode path.
- **Multilingual alternate titles + display-language preference** (Kairos + Hades): searching now matches an item by
  any known-language title, not just the one currently displayed — typing "El Perro y El Gato" finds "The Dog and
  the Cat" if that Spanish title was ever scraped or manually entered. `item_alternate_title` gained `language`/
  `overview` columns, and TMDB, TVDB, AniDB, Wikidata, Trakt, and AniList all now feed every localized title (and,
  where the source has it, overview) they see into it, instead of discarding all but one pick (TVMaze skipped — its
  `akas` endpoint is country-tagged regional aliases, not real language translations). Bare-word fuzzy search and
  the `title:` filter field both now match against the canonical title, `original_title`, and every alternate title.
  A per-library **and** per-item "Display Language" preference (new field on the item edit form, next to Original
  Title) now determines which language's title/overview a scraper writes as canonical for that item, with a new
  alternate-titles list editor (language/title/overview, add/remove) alongside the existing external-IDs editor.
  Also fixes a pre-existing bug where `original_title` was silently dropped on every scraper refresh after the
  initial library sync (only sync itself ever wrote it).
- **"Normalize to H.264/AAC" library job** (Kairos + Hades): new manual-trigger job that re-encodes any episode/movie
  file that isn't H.264/AAC, or is flagged variable-frame-rate (same heuristic as Hephaestus's own `isLikelyVfr`), so
  more of the library qualifies for the cheap direct-stream channel bucket instead of a live software transcode.
  Writes to a temp sibling, ffprobe-verifies the result before replacing the original (never touches a file it
  can't confirm is good), and refreshes the cached duration/resolution/keyframe metadata afterward. Runs one file
  at a time — a single libx264 encode already saturates every core. Disabled by default, no automatic schedule;
  triggered from Settings > Jobs like the existing sync/writeback jobs.
- **Multi-part movie detection and scheduling** (Kairos): a movie split across several video files (`CD1`/`CD2`,
  `Part 1`/`Part 2`, `Disc 1`/`Disc 2`, `pt.1`/`pt.2` naming) is now grouped into one logical movie row instead of
  syncing as separate/duplicate entries. New `movie_part` table + `movie.is_multi_part` flag; LocalSource groups
  sibling files by filename marker, PlexSource walks Plex's own `Media.Part[]` array (previously discarded past
  index 0), and JellyfinBaseSource (Emby too) clusters sibling library items the same way LocalSource does.
  `movie.duration_ms` is kept as the sum of part durations — including a new local-file per-part ffprobe pass — so
  channel scheduling (`RuleEngine`) treats a multi-part movie as one correctly-durationed block with no scheduler
  changes needed.
- **Multi-part movie playback continuity** (Kairos + Hephaestus + Hades): a multi-part movie now plays as one
  continuous session instead of stopping after part 1. Each part still plays through the existing single-file
  `VodSession`/HLS pipeline unchanged (no ffmpeg concat-demuxer); Kairos's `GET /api/playback/movie/:id` gained an
  optional `?part=N`, and `GET /api/movies/:id/resolve-play-target`/`PUT /api/watch-progress/movie/:id` now translate
  between a per-part position and the summed-across-parts position `watch_progress` stores, so resume always lands
  in the right part at the right offset. The Hades player advances to the next part automatically on HLS
  end-of-stream (no "Up Next" interstitial, no reload/back-navigation) and only marks the movie fully watched once
  the *last* part finishes. Also fixes a pre-existing bug where Hades' movie playback never called the resume
  endpoint at all (hardcoded `positionMs: 0` for every movie, single-file or not).
- **Manual multi-part movie link/unlink** (Kairos + Hades): admin safety valve for multi-part movies the sync-time
  heuristic missed (or got wrong) — `GET /api/movies/grouping-candidates` surfaces heuristic-scored candidates
  among standalone movies, `POST /api/movies/:id/parts/link` groups them (reusing the existing movie-merge FK
  cleanup), `POST /api/movies/:id/parts/unlink` splits a part back out into its own movie. A manually-linked movie
  is locked (`movie.locked=1`) and its `movie_part` rows marked `origin='manual'`, so a later sync of the target's
  own source can never silently undo the admin's grouping — sync only ever touches `origin='auto'` rows and skips
  locked movies entirely. New "MOVIE PARTS" admin panel section (Hades library detail) and a "Link as multi-part"
  action alongside the existing duplicate-merge flow in the Review queue.
- **"About Pantheon" page** (Hades `/about`, `docs/About.html`): contribution model, financial support (placeholder
  for now), and credits. Both render the same `docs/About.md`, fetched live from GitHub and cached server-side
  (`GET /api/about`, public) so the two can never drift apart.
- **SECURITY.md**: vulnerability disclosure policy, `security@pantheonmedia.app`.
- **Same-priority block conflict warning** (Hades channel builder): two blocks covering the same time on the same
  day at equal priority have no defined winner. The day grid now flags them with a red border/badge and a tooltip
  explaining why, and brings them to the front of the day's stack so the warning isn't hidden behind a
  higher-priority block.
- **Categorized live-playback diagnostics** (Hades): hls.js buffer append/flush events (per-track buffered ranges,
  not just the intersection `HTMLMediaElement.buffered` reports), fatal errors, track switches, seeks, fragment
  loads, and session/viewer lifecycle (reconnects, capability-bucket fallback, channel item transitions) are now
  forwarded to the server log under a `[category]` prefix for live channels, gated on the existing "Hades Console
  Error Logging" (`hades_debug`) toggle.
- **"Download all logs" diagnostics export** (Hades, Hermes, Kairos, Hephaestus): the Activity page's Engine Logs
  panel can now download a zip with all three services' complete on-disk log files, not just the ~2000-line combined
  in-memory view the live viewer shows. Each service exposes its own log file via a new `GET /api/logs/file`; Hermes
  aggregates all three into `GET /api/logs/export`, admin-authenticated the same way as the existing log stream.

### Fixed

- **Queued live-channel preroll producer could crash mid-encode with "No such file or directory"** (Hephaestus):
  `ChannelSession::stop()` removed the whole session's HLS directory (`remove_all(hlsDir())`) without first stopping
  any producers still sitting in the preroll queue (built ahead of their actual air time, per-item, each a live ffmpeg
  process writing into its own `pending/<id>` subdirectory) — only the single currently-active producer was stopped.
  A still-running queued producer's ffmpeg process would keep writing after its directory was deleted out from under
  it, crashing with a real "Failed to open file ... No such file or directory" mid-item and leaving the live playlist
  frozen on stale content until the next item's own (unaffected) producer got spliced in. `stop()` now stops and
  clears the preroll queue in the same critical section, before the directory removal.
- **Hermes could serve stale/wrong segment bytes after a live-channel session restarted** (Hephaestus + Hermes): a
  channel's segment numbering (`seg-00000.ts`, ...) always restarts at zero when its underlying `ChannelSession` gets
  torn down and recreated (idle reap, then a fresh request) — Hephaestus's own cache already handled this (invalidated
  on teardown), but Hermes's independent cache had no way to know a session had restarted, so it could keep serving
  old cached bytes under a since-reused segment filename: playback jumping back to content from a previous session
  with nothing in the current schedule explaining it, sometimes cascading into a dead stream. `ChannelSession` now
  carries a per-incarnation `instance_id`, embedded (opaque, unvalidated) in the content-addressed segment URL the
  channel-viewer playlist rewrite emits — a restart naturally changes the URL, so Hermes's cache can't collide across
  incarnations.
- **`kHlsHeadWindowSegments` (Hephaestus) was tuned for a since-changed segment length**: the "150s of slack" the
  constant's own comment claimed assumed `kLiveHlsSegmentSecs=6`; the value was never revisited when that was set
  back to 2, silently leaving only ~50s of real slack (3x less than intended) before a live-channel producer's
  encoder head had to rotate — recurring roughly every 50s for any item watched that long. Now derived from a named
  target-seconds constant divided by `kLiveHlsSegmentSecs` so it can't silently drift out of sync again.
- **Sidebar profile controls looked like static text, not buttons (Hades)**: `usernameLinkBtn`/`sidebarExitBtn` had
  no resting border or hover background, unlike their already-correct mobile-drawer equivalents. Both now match the
  drawer's affordance; the new About link sits alongside them as a matching icon button.
- **Live-channel transcode-bucket buffer stalls on low-power/no-hwaccel hosts** (Hephaestus): `kLiveHlsSegmentSecs`
  had been quietly reverted from 6 back to 2 (a prior confirmed A/B fix for periodic stutter) while retesting an
  unrelated bitrate-cap/NVENC change — reproduced the same stalls on a real, software-only demo VM. Restored to 6.
- **Direct-stream (native bucket) audio/video desync** (Hephaestus): the direct-stream bucket copies video untouched
  but always re-encodes audio (for loudness normalization), an asymmetric pipeline with no prior resync mechanism.
  Added `aresample=async=1` to that bucket's audio filter chain to correct drift against the copied video track.
- **Live-channel bucket switch corrupted playback (audio/video from one item bleeding into the next, plus stalls)**
  (Hephaestus + Hades): `ChannelViewerRegistry` used to silently migrate an already-connected viewer's serving bucket
  (default transcode vs. native direct-stream) whenever the schedule moved onto/off of a directly-streamable item —
  the two buckets are fully independent ffmpeg encodes/HLS playlists with unrelated segment numbering, so an already-
  polling hls.js session had no way to know its manifest URL just started resolving to an unrelated stream. A
  viewer's serving bucket is now pinned at connect time and never mutated after; `reassignForChannel` only updates
  an advisory `recommended_bucket`, exposed via a new `GET /stream/channel/viewer/:id/status`, and Hades polls it and
  does a real reconnect (fresh viewer session + manifest) when a switch is actually recommended. Recommendations are
  also gated on the item being at least 45s and not a filler, so a channel interleaving many short bumpers (the
  schedule that originally exposed this bug) doesn't reconnect on almost every item boundary. That gate only applies
  to the opportunistic default-to-native upgrade — a native-to-default fallback (a viewer already pinned to native
  who is no longer eligible for what's now airing) is never gated, since the native bucket copies video verbatim
  regardless of viewer capability and leaving an ineligible viewer on it risks genuinely undecodable playback, not
  just a missed efficiency win.
- **Live channel repeatedly restarting a different, unscheduled episode from the beginning** (Kairos): `GET
  /api/channels/:id/now`'s fallback for a gap in the committed schedule called a non-persisted "peek" block
  resolution and always reported the picked item as starting at the query's own timestamp, so every re-poll during
  the same gap (a session restart, a reconnect) independently re-resolved and reported a fresh episode starting at
  offset 0. Gaps now fall straight to the channel's configured filler instead.
- **Live channel producer thrashing under transient GPU/CPU contention** (Hephaestus): `VodEncodeStream` used to
  kill and cold-restart a segment-producing head as soon as it fell a few segments behind schedule, even if it was
  still actively producing — the replacement pays the same reopen/reprobe/reseek/NVENC-init cost while wall-clock
  keeps moving, which could turn a transient slowdown into a self-sustaining restart loop (confirmed live: the same
  source file reopened repeatedly within seconds, never catching up). A head is now only replaced once it's
  genuinely stalled (no forward progress for 15s) or exhausted; a merely-lagging head gets a much wider catch-up
  margin. Channel head windows also widened 10→25 segments to reduce how often a handoff happens at all.
- **Live channel HLS producer always started an item from position 0, ignoring the correct wall-clock resume
  offset** (Hephaestus): `hlsCreateProducer`'s `-ss` seek used `VodEncodeStream`'s own segment-relative
  `position_ms` (always 0 for a spawn's first head) instead of the real, separately-computed resume offset — the
  correct offset was used to size the segment count but silently never reached the actual ffmpeg seek argument.
  Every cold start, reconnect, or bucket switch landing partway into an item played from its beginning instead of
  the right position.
- **Live channel playback randomly rewinding then snapping back to the correct position** (Hephaestus, Hermes):
  HLS playlist/segment responses set no `Cache-Control` at all, so an intermediary in the real deployment path
  (Cloudflare Tunnel, a browser's own HTTP cache) was free to cache a snapshot of the live, constantly-rewritten
  `playlist.m3u8` and keep serving it — read by hls.js as the available segment list randomly jumping backward and
  forward between whatever snapshot a given request happened to get. Manifests now set `no-store, no-cache,
  must-revalidate`; segments (immutable once written, never reused under the same name) now set a long, cacheable
  `max-age`. Hermes's generic proxy also silently dropped every response header except `Location`/`Content-Type`
  from the upstream it was forwarding — now forwards `Cache-Control`/`Pragma` too, or the origin's fix above would
  never have reached a real client anyway.
- **Cold starts and segment stutter on short live-channel filler/bumpers** (Hephaestus): the producer's own poll
  loop only checked whether the next item needed prerolling every `segment_length*500ms` (3s at the current 6s
  segment length) — a filler shorter than that gap could start and fully end between two polls, so nothing was
  ever prerolled for whatever came after it, and the real transition (whenever the next poll finally landed) had
  already fallen behind and cold-spawned instead. The poll loop now runs on a fixed 1s interval, independent of
  segment length, so even a several-seconds-long filler gets multiple chances to be caught mid-item. Separately, a
  preroll built for one item can still go stale (its own target window fully elapses) before ever being promoted,
  if a chain of short items keeps the schedule running behind real time — previously undetected until the real
  transition found the mismatch and discarded it; now caught as soon as it's determinable, freeing that hardware
  encoder slot for a fresh, still-useful preroll instead of sitting on an already-doomed one.
- **Short filler/bumpers still got too little preroll lead time** (Hephaestus): even with the above fix, preroll was
  still reactive one item at a time — a filler right behind a 5s bumper only started building once the bumper
  itself became the current item, leaving it barely any lead time of its own. The producer now maintains a small
  queue of preroll'd items instead of a single slot: once inside the lead window of the *current* item's own end, it
  walks the schedule forward and builds a producer for every item starting within that same window, so a filler
  behind a short bumper starts prerolling at the same moment as the bumper, not after. Bounded to 6 queued items at
  once as a safety rail against a pathological run of near-zero-duration schedule entries.
- **Preroll'd producers could get discarded and cold-restarted right at promotion, despite a good backlog**
  (Hephaestus): a preroll's own progress bookkeeping only ever advances via an explicit `tick()` call, which was
  only ever made on the currently-live producer — a producer still sitting in the preroll queue kept writing
  segments in the background the whole time, but its bookkeeping stayed frozen at "nothing generated yet"
  regardless. The first real segment request right after promotion could then misjudge a perfectly good backlog as
  behind schedule and trigger an unwanted respawn, discarding it. Every queued preroll is now ticked on the same
  cadence as the live producer, plus a final catch-up tick right at the promotion handoff itself.
- **"Verbose transcode logs" flooded the log file down to well under a minute of real coverage** (Hephaestus): its
  per-frame `showinfo`/`ashowinfo` debug filters logged a line per video/audio frame each — tens of lines a second
  per active ffmpeg process, easily enough to rotate a 10MB log file out within a minute under normal multi-producer
  load. Both are now sampled to roughly once per second per stream instead of logged in full — still enough to spot
  A/V drift trending over time (the reason to turn this on in the first place), without burying everything else.
  Also fixed a separate, unrelated bug surfaced by the same log: output from concurrent ffmpeg processes could get
  interleaved mid-line into unreadable jammed-together text, since each forwarded line was written as several
  separate (each individually flushed) stream insertions instead of one.

### Removed

- **`on_play` channel advance mode** (Kairos + Hades): predated block scheduling and never got a UI toggle to
  select it. Advancing cursors only on confirmed playback made sense before blocks existed, but required
  `ensureScheduled` to fully delete-and-reproject an `on_play` channel's schedule from a rolling window on every
  single `/now` poll (no persisted wall-clock anchor for the in-progress item) — any regenerate/timing wobble could
  report the currently-airing item as having just started. `scheduled` mode's cursors already advance during normal
  EPG projection, so nothing else relied on this.

### Changed

- **README/CONTRIBUTING**: corrected stale "Alpha" status to Beta, reworded the PR policy around maintainer
  bandwidth instead of "no unsolicited PRs," and refreshed the test count (1,072 → 1,128, re-counted from a clean
  run rather than carried forward).

## [0.3.0] - 2026-08-03

### Security

- **Public-demo hardening pass**: a full audit (5 parallel workstreams across auth, DDoS/resource-exhaustion,
  secrets storage, injection/path-traversal, and deployment/CORS, plus a follow-up guest-account-abuse pass)
  turned up several real gaps before this could safely go up as a public server; all are now fixed and covered by
  new tests in `momus/kairos/api/test_content_service_security.cpp`,
  `momus/kairos/api/test_scraper_service_security.cpp`, and `momus/kairos/auth/test_auth_store.cpp`. Ship
  blockers: (1) `POST /api/auth/setup` had no protection against a random visitor claiming the admin account before
  the operator did on a freshly-deployed public instance — Kairos now bootstraps the admin account itself at
  startup from `KAIROS_ADMIN_USERNAME`/`KAIROS_ADMIN_PASSWORD` env vars (`docker-compose.yml`), closing the window
  before the port ever opens. (2) `GET /api/images/proxy?url=` was an open, unauthenticated SSRF proxy — no host
  allowlist meant an outside attacker could point it at internal Docker services or a cloud metadata endpoint;
  `ContentService.cpp` now resolves and rejects private/loopback/link-local/reserved addresses before fetching.
  (3) `/api/logs/stream` (Kairos and Hermes both) was completely unauthenticated and held an API-thread-pool slot
  open per connection — a handful of concurrent anonymous connections could freeze the whole API; now admin-only,
  with a new shared-secret internal-token path (`hermes/src/kairos/InternalToken.{h,cpp}`, mirroring Hephaestus's
  existing one) so Hermes's server-to-server log relay and Hephaestus's own now-internal-only `/api/logs/stream`
  keep working. (4) VOD/preview transcode sessions (`Hephaestus`) had no concurrency cap at all — any account,
  including a free self-service guest, could loop-request sessions and exhaust host CPU/GPU; both session managers
  now take a `max_sessions` cap (`HEPH_MAX_VOD_SESSIONS`/`HEPH_MAX_PREVIEW_SESSIONS`, default 8 each). High-priority:
  `POST /api/auth/login` had no brute-force protection at all (now a 5-fail/5-minute lockout per account, mirroring
  the existing PIN-lockout pattern); `GET /api/auth/profiles` leaked every account's full detail
  (`must_change_password`, `last_seen`, rating ceilings, language/landing-page prefs) to any authenticated session
  including a guest, enabling username enumeration for the login lockout to matter against — now trimmed to exactly
  what the picker UI renders; `GET /api/scrapers/config` returned real TMDB/TVDB/Trakt/AniDB keys and TVDB's pin in
  plaintext, breaking this codebase's own established `has_token`-style masking convention — now write-only
  (`has_api_key`/`has_pin` booleans on GET, empty-means-unchanged on PATCH, with a matching Hades `SettingsPage.tsx`
  placeholder so an untouched field can't silently wipe a stored key on an unrelated save); guest-account creation
  had no rate limit beyond the existing concurrent-count cap (now also 10/IP/10min, `RateLimiter.h`); `POST
  /api/sync/all` had no role check at all, letting any authenticated account including a guest trigger a full
  library resync; sessions never expired on idle (a 30-day flat TTL regardless of activity) — now also enforces a
  14-day idle cutoff; and all three services gained baseline hardening response headers
  (`X-Content-Type-Options`/`X-Frame-Options`/`Referrer-Policy`/`Strict-Transport-Security`) and a bounded request
  payload size (25 MB, previously unbounded). Reviewed and deliberately left unchanged: wildcard CORS (this API is
  bearer-token-only with no cookie/credentialed mode, so the classic wildcard-CORS exploit doesn't apply, and
  several endpoints — HDHomeRun/XMLTV/M3U feeds, the Chromecast custom receiver — need it) and the download
  destination path (an intentional operator-configured absolute directory, same category as `FILLER_PATH`, not a
  sandboxed value with a boundary to escape). No Content-Security-Policy was added — this app loads Google Fonts
  and the Chromecast Sender SDK from `gstatic.com` and hls.js may use a blob: worker, and getting a CSP subtly
  wrong (especially Cast, unverifiable without real hardware) is worse for a live public demo than not having one;
  flagged as a follow-up needing real device testing.

### Added

- **Diagnostics for the periodic live-channel transcode stutter, targeting the layer below ffmpeg's own frame
  encoding (Hades, Hephaestus)**: the segment-duration bisection (glitch present at `kLiveHlsSegmentSecs=2`, absent
  at `6`) plus clean `showinfo`/`ashowinfo` per-frame `pts_time` output (see the A/V-drift diagnostics entry just
  below) together point away from ffmpeg's encode timing and toward the HLS muxer/segmenter or the player's
  handling of frequent segment boundaries — neither of which had any instrumentation. Two additions: (1) hls.js's
  non-fatal `BUFFER_STALLED_ERROR`/`BUFFER_NUDGE_ON_STALL` events (`VideoPlayer.tsx`) — already detected client-side
  as the "chirp" nudges described in that code's own comment, but only ever `console.warn`'d, which `remoteLog.ts`
  never forwards (it only patches `console.error`) — are now sent to `POST /api/logs/client` for every live-channel
  occurrence, not just the escalation after 3-in-12s, gated on the existing "Hades Console Error Logging"
  (`hades_debug`) toggle; frequency-over-time is exactly the signal missing server-side, and it should scale with
  segment count the same way the glitch itself does. (2) `ChannelSession::patchDiscontinuitySequence` now also
  checks each newly-appeared HLS segment's actual `#EXTINF` duration against the requested `hls_time`
  (`kLiveHlsSegmentSecs`), piggybacked on the playlist read it already does every tick, and logs a mismatch beyond
  20% — evidence the muxer itself is cutting segments irregularly (e.g. off-GOP-boundary) rather than a frame-level
  problem. Gated on `verbose_transcode_logs`, same as the diagnostics below.
- **Diagnostics for tracking down A/V drift on live channels, specifically software (non-hardware-accelerated)
  transcoding under real CPU contention (Hephaestus)**: reported on the public demo server as audio and video
  slowly desyncing on a software-transcoded channel — a plausible mechanism given the codebase already had no
  `-thread_queue_size`/`-max_muxing_queue_size` tuning and no per-stream drift correction, but not yet confirmed
  against real evidence. Turning on the existing `verbose_transcode_logs` setting now gets meaningfully more to look
  at: `-stats -stats_period 2` (ffmpeg only auto-prints `-stats` on a real tty, never true here since stderr is
  always piped, so it needs to be requested explicitly) for periodic fps/speed/drop/dup counters, plus `showinfo`/
  `ashowinfo` filters on the live-channel transcode bucket's video and audio chains reporting each frame's
  `pts_time` right before it reaches its encoder. Every captured ffmpeg stderr line (when Ffmpeg Debug Logging,
  below, is also on) is now prefixed with a wall-clock timestamp (`FfmpegProcess.cpp`) — ffmpeg's own timings here
  are all stream-relative, so without this there was no way to line up "video frame at pts_time X" against "audio
  frame at pts_time Y" to see which stream is actually falling behind in real time. All of this is diagnostic-only,
  gated behind `verbose_transcode_logs` (too chatty to leave on otherwise), and expected to come back out once the
  actual root cause is confirmed and fixed. Covered by a new
  `PushAudioEncoderArgs_DebugShowinfoAppendsAshowinfoFilter` test in `momus/hephaestus/test_encoder_args.cpp`;
  `PushLogLevelArgs`'s existing test updated for the new `-stats` args.
- **`HEPH_FFMPEG_DEBUG` (whether ffmpeg's stderr streams live into the Activity page vs. only being tail-captured
  for an on-failure dump) is now a runtime Settings toggle ("Ffmpeg Debug Logging"), not a Hephaestus-startup-only
  env var (Kairos, Hephaestus, Hades)**: needed a restart to flip while diagnosing the A/V drift issue just above,
  unlike its neighboring `verbose_transcode_logs` ("Verbose Transcode Logging") which was already live-toggleable —
  an inconsistency once the two are meant to be used together. Wired through the exact same path as that setting:
  a new Kairos-owned, DB-persisted `g_ffmpeg_debug_logs` flag (`RuntimeFlags.h`/`.cpp`), exposed on both
  `GET /api/config/settings` (admin) and `GET /api/config/public-settings` (unauthenticated, for Hephaestus) and
  settable via `PATCH /api/config/settings`; a new `KairosClient::getFfmpegDebugLogs()` polled by all three of
  Hephaestus's session managers (`SessionManager`/`VodSessionManager`/`PreviewSessionManager`) on their existing
  ~15s/5min cache-refresh cadence, same as `getVerboseTranscodeLogs()`. `HEPH_FFMPEG_DEBUG` still sets the
  pre-first-poll default at cold start, same role `HEPH_VERBOSE_TRANSCODE` already had.
- **Live-channel viewing now shows up in Play History / "who's watching" telemetry (Kairos, Hades, pantheon-android)**:
  `PUT /api/watch-progress/:content_type/:id`'s `validContentType` only ever accepted `movie`/`episode` — a channel
  viewer produced no `playback_history` row at all, so the Activity tab's Play History table and "who's actively
  playing" view had no idea live channels were being watched, by whom, on what device, or on which bucket
  (direct-stream/transcode). Added a dedicated `content_type == "channel"` branch to the watch-progress handler
  that feeds `PlaybackHistoryRepository` exactly like a movie/episode ping does (title resolved from the channel's
  own name; `recordPing`'s existing (user, content_type, content_id) session-extension logic needed no changes to
  correctly treat a channel switch as a new sitting) — but deliberately skips `WatchProgressRepository` entirely,
  since a channel has no position/duration/resume concept and upserting one would incorrectly surface live TV in
  Continue Watching. Hades' web player (`PlayerPage.tsx`) previously skipped progress pings entirely for channels
  (`if (kind === 'channel') return`) — now pings this new endpoint on the same interval instead. Android had the
  identical gap (`PlayerViewModel.reportProgress`'s `if (isLive ...) return`) — fixed the same way, reusing the
  existing `putWatchProgress` endpoint/DTO with `contentType="channel"` rather than adding new API surface there.
  `PlayHistoryPanel.tsx`'s Progress column showed a meaningless "0% · 0s" for a zero-duration channel row; now
  shows total sitting length instead, with a live-channel indicator on the title. Covered by three new route-level
  tests in `momus/kairos/api/test_playback_and_channel_routes.cpp`. Roku not yet covered — no time to audit its
  equivalent ping path this pass.
- **Android player pauses when the app is backgrounded (pantheon-android)**: nothing stopped playback (audio kept
  running, and a live channel's HLS session kept being polled/billed as an active viewer) when the app left the
  foreground — pressing Home mid-episode just kept playing unseen. New `PlaybackPreferences` (plain
  `SharedPreferences`, per-device — this isn't the kind of setting that should sync across devices) gates a
  `ProcessLifecycleOwner`-driven `ON_STOP` → `exoPlayer.pause()` in `PlayerScreen.kt`, shared by both the
  google/mobile and TV flavors. `ProcessLifecycleOwner` (whole-app foreground/background), not this screen's own
  Activity lifecycle, since an Activity's `onStop` also fires on ordinary in-app navigation, not just the app
  actually backgrounding. Deliberately doesn't auto-resume on return (`ON_START`) — an unprompted resume would
  restart audio/video the user didn't ask for. `pauseOnBackground` defaults to `true` and is a real persisted flag
  (not a hardcoded constant) even though no settings screen exposes it yet — no general Settings UI exists on
  Android today — specifically so a future audio/"radio" content type (which should keep playing backgrounded,
  the way music apps do) has a real hook to flip instead of needing this behavior re-architected in later.
- **"Always transcode" per-channel setting (Kairos, Hephaestus, Hades)**: direct-stream (Hephaestus's native
  bucket) can't run a real filter graph, so it can only ever correct schedule drift in coarse, discrete jumps
  (skipping ahead at the next item transition) rather than the transcode bucket's smooth, continuous speed-based
  correction — and, until this session's audio-encode fix above, couldn't apply loudnorm at all. Some channels
  need those smoother guarantees badly enough to be worth giving up direct-stream's CPU/GPU savings entirely.
  New `channel.force_transcode` column (migration v106, default false — every existing channel keeps today's
  dual-bucket behavior) flows through `KairosChannel`/`SessionManager::channelForcesTranscode()` into
  `ChannelViewerRegistry::start()`/`reassignForChannel()`, which now short-circuit straight to the default
  (transcode) bucket for such a channel regardless of viewer capability eligibility — the native bucket is never
  resolved or spun up for it at all. New "Always transcode" toggle in the channel editor's Transcode Quality
  section (Hades).
- **Activity page now shows which bucket a live channel session is using, and an exact HLS viewer count where
  available (Hephaestus, Hades)**: the "Now Playing" panel previously had no way to tell a direct-stream session
  from a transcode one (both looked identical), and every channel's HLS viewer count was a bare "someone's
  watching" presence boolean floored to +1, regardless of how many people actually were. `ChannelSession` gained a
  `bucketName()` accessor; `ChannelViewerRegistry` gained `viewerCounts()` (exact per-channel, per-bucket counts of
  capability-bucketed HLS viewers — the same registry that already tracks each viewer's bucket assignment for
  routing, just never had a query surface). `/stream/activity/sessions` now reports `bucket` and
  `hls_viewer_count` per channel session; the Now Playing panel shows the bucket inline (Direct stream/Transcode)
  and uses the exact count when available, only falling back to the old "≥" floor for a viewer on the legacy,
  non-bucketed HLS URL (no per-viewer identity to count there at all). Covered by new tests in
  `momus/hephaestus/test_channel_viewer_registry.cpp`.
- **"Update Live" option when saving channel edits (Kairos, Hades)**: saving a channel normally leaves whatever's
  currently on-air alone and only applies the edit going forward (see the "currently-airing item yanked" fix
  below) — safe by default, but a broader edit (e.g. a shifted block boundary) can leave that untouched item
  overlapping with the freshly-regenerated schedule after it, since the regeneration has no awareness the old row
  is still occupying part of its window. `ScheduleCache::clear()`/`hardReset()` gained a `preserve_current`
  parameter (default `true`, the existing behavior); `POST /api/channels/:id/epg/clear?live=true` passes `false`,
  dropping the carve-out so the live stream picks up the edit immediately instead. The channel editor's header now
  offers "Update Live…" next to "Save Channel" (behind an explicit confirm, since it visibly interrupts anyone
  currently watching) alongside the unchanged default save path. Covered by
  `momus/kairos/api/test_schedule_cache.cpp`.
- **"Keep positions" option when a channel save would hard-reset cursor state (Kairos, Hades)**: a structural block
  edit (day_mask/start_time/end_time/priority/play_style/advancement/cursor_scope/no_history_behavior, or an
  added/removed block), or a channel timezone/seed change, has always triggered `ScheduleCache::hardReset()` —
  wiping every accumulated media cursor so all of a channel's shows/movies resume from the beginning instead of
  where they'd aired up to. That's the right default (old cursor state can be actively wrong for the new
  configuration) but was previously silent and unconditional, so an otherwise-minor edit (e.g. nudging a block's
  start time) could unexpectedly restart an entire channel's content. `ChannelService.cpp`'s `PATCH
  /api/channels/:id` and `BlockService.cpp`'s block create/update/delete routes now accept `?preserve_cursor=true`,
  downgrading the hard reset to the existing soft `clear()` — an explicit opt-out rather than automatic. Hades'
  channel editor now detects a pending structural change before saving (`ChannelDetailStore::hasStructuralChanges`)
  and prompts with "Reset positions & save" / "Keep positions & save" / "Cancel" instead of resetting silently;
  non-structural saves are unaffected and still save without a prompt. Covered by new tests in
  `momus/kairos/api/test_preserve_cursor.cpp`.
- **SDUI manifest completeness pass — Watch Together as a real Home row, per-zone action gating, dead field
  removed (Kairos, Hades, Android)**: a from-scratch audit comparing how much of the Android client is actually
  manifest-driven vs hardcoded (prep work for eventually porting Roku onto the same system) turned up three real
  gaps, all fixed in Kairos migration v105: (1) the Android Watch Together shelf was fully hardcoded (fixed
  position, unconditional fetch) instead of gated/positioned by the manifest the way every other Home row is —
  `home.rows` gains a `watch-together` row type, handled like `guide` (presence is the whole signal; the client
  fetches its own data since a session's host/member_count shape doesn't fit the uniform shelf-tile response every
  other row resolves through). (2) Detail's Play/Play from Beginning/Watch Together buttons were one hardcoded
  bundle behind a single zone check — `TvZone` gains an `actions` list (mirroring `TvHomeRow.actions`, which
  already existed but had no zone equivalent) so each button is independently controllable server-side. (3)
  `TvZone.dataSource`/`TvDataSource` — confirmed dead on every client (Android's own comments already said as
  much; grepping hades/src turned up zero actual `.dataSource` reads) — removed entirely rather than left
  half-implemented. Android also gates its "Library" quick-action on the manifest actually declaring library
  zones, the same treatment "Guide" already had. Along the way, Android's theme-token model
  (`TvThemeTokens`/`PantheonColors`) — previously colors-only even though `generate-tv-tokens.mjs` has always
  emitted spacing/radii/transitions/sizing/fontSizes too — gained a parsed, typed `PantheonMetrics` counterpart
  (`LocalPantheonMetrics`) for those categories, wired into both flavors' theme providers; this is infrastructure
  only in this pass (positioning/animation *values*, never behavior — see the audit's own reasoning on why a
  generic layout DSL isn't the right lever), no existing hardcoded dp/duration constants were retargeted to it yet
  pending verifying each one against hades' actual CSS individually — except one real consumer added alongside it:
  Android TV Detail's Play/Play from Beginning/Watch Together buttons (now independently toggleable per the
  `actions` list above) now collapse to just an icon while unfocused and expand to icon+label on D-pad focus
  (`CollapsibleActionButton`, animated via `animateContentSize` using the new `hds-transition-fast` token) so all
  three sit comfortably inline instead of competing for row width as full-text buttons. Covered by new tests in
  `momus/kairos/api/test_tv_manifest_service.cpp` and `DetailViewModelTest.kt`.
- **Detail page consistency pass across every client, "first test" for the broader consistency effort (Hades,
  Android)**: comparing all four Detail implementations (desktop web, web `/tv`, Android mobile, Android TV) found
  "Play from Beginning" existed only on Android, and web `/tv` was additionally missing "Watch Together" entirely
  — two clients had 3 actions, one had 2, one had 1. Added `PlayFromBeginningAction` to desktop web
  (`LibraryDetailActions.tsx`, wired into both its callers — `MediaDetail.tsx` and `HomePage.tsx`'s own inline
  detail view — via `MediaDetailHero`'s `playButton` prop, now a render-prop like `actions` already was, so it can
  read `seasonsWithEpisodes` without a second fetch) and both `PlayFromBeginningAction`/a new Watch Together
  handler to web `/tv` (`TvLibraryDetail.tsx`), which also now honors the `play-button` zone's `actions` list
  (kairos v105) the same way Android does — previously it rendered Play unconditionally with no zone-actions
  awareness at all. Web `/tv`'s three buttons also collapse to icon-only while unfocused and expand on remote
  focus, mirroring Android TV's own treatment above (`.actionButton`/`.actionLabel` in
  `TvLibraryDetail.module.css`, transitioning on `hds-transition-fast`).
- **Web `/tv`'s episode row could scroll to the wrong position (or not visibly move at all) the first time D-pad
  focus entered a collapsed season (Hades)**: `EpisodeShelf.tsx` expands a season's episode row via `useDebounce`
  on `hovered || hasFocusedChild` (150ms, meant to stop a mouse sweeping across several collapsed seasons from
  firing a burst of expand/collapse) — but `useFocusable`'s own `scrollIntoView()` fires synchronously the instant
  a tile receives focus, with no debounce of its own. A discrete D-pad focus landing on an episode inside a still-
  collapsed season therefore scrolled *before* the row had expanded, computing against its still-near-zero-height
  CSS grid geometry. Split the debounce to apply only to the hover half (`debouncedHovered`) — `hasFocusedChild`
  now expands the row instantly, so the scroll and the expand are no longer racing. Hover-driven expansion (mouse)
  is unaffected.
- **Android TV Detail's hero height was an independently-hardcoded `320.dp`, one manual edit away from silently
  drifting from web's own matching value (Hades, Android)**: promoted web's `HERO_HEIGHT_CSS` floor and
  `HERO_OVERLAP` (both previously plain TS module constants, duplicated a second time as hardcoded literals in
  `TvLibraryDetail.module.css`'s `.heroSpacer` with a code comment warning "if either constant ever changes,
  update both places") to real tokens — `--hds-tile-hero-height-tv`/`--hds-tile-hero-overlap-tv` in `index.css`,
  picked up by `generate-tv-tokens.mjs` automatically. Android's new `PantheonMetrics.heroHeightTv` reads the same
  token (`DetailScreen.kt` no longer has its own separate `HERO_HEIGHT` constant); web's own two previously-manual
  duplicate spots (the TS constant and the CSS module) now both read the one token too, closing that
  drift-by-hand-edit gap on the web side as well.
- **Live channels (and possibly VOD — one unconfirmed report) periodically showed a black frame/stutter, sometimes
  recovering cleanly and sometimes appearing to replay the last ~0.25s of frames (Hephaestus)**: every real-content
  ffmpeg invocation set `-fflags +genpts+discardcorrupt`. `discardcorrupt` is a demuxer heuristic for dropping
  packets it judges corrupt — appropriate for a genuinely lossy source (flaky network capture, RTSP packet loss),
  not a well-formed local file, where it can false-positive on legitimate-but-unusual timestamp patterns (VFR
  content, edit lists, telecine-era masters) and silently drop real reference frames. Traced to the very first
  Hephaestus scaffold commit — part of a generic "reduce startup latency" flag combo, never independently
  justified for this specific use, unlike nearly every other ffmpeg flag choice in this codebase. Dropped from all
  four call sites (`ChannelSession.cpp`, `VodSession.cpp` ×2, `PreviewSession.cpp`), keeping `+genpts`/`-flags
  low_delay`. Diagnosed from code review, not a captured incident — please retest live.
- **Direct-stream (kNativeBucket) live channels could still stutter roughly every 12-15s after the fix above
  (Hephaestus)**: `-force_key_frames` (which makes the transcode bucket's real segments land exactly on the
  requested 2s) is unusable on a `-c:v copy` session — ffmpeg can only cut where the *source file's own* keyframes
  actually are, which can easily be 5-15s+ apart for real-world files. `hls_list_size`/delete timing were sized
  only for the intended 2s case, leaving too shallow a rolling window once real segments came out far longer —
  raising the odds of a client's briefly-stale manifest racing the deletion of a segment it still legitimately
  needs. Bumped `hls_list_size` 6→12 and added `hls_delete_threshold=8` (ffmpeg keeps a segment file on disk until
  it's older than list_size+delete_threshold entries, specifically for this kind of client staleness). A blunt,
  safe mitigation, not the precise fix — sizing `-hls_time` per item off that item's own real keyframe spacing
  (already cached, see `snapToKeyframe`) would be the more correct follow-up.

### Fixed

- **`Sync All` silently died partway through whenever it reached a source with a genuinely empty library —
  crashing and restarting the whole Kairos process, not returning a clean error (Kairos)**: `JellyfinBaseSource::
  fetchShows()`/`fetchMovies()` (shared by Jellyfin and Emby) and `PlexSource::fetchShows()`/`fetchMovies()` each
  had a debug log line — `"Queried <first> to <last>"` — that indexed `items[0]` and `items[page_count - 1]`
  without checking the page actually had any items. A present-but-empty result page (a real, valid response shape
  — an empty library, or the final page landing exactly on a pagination boundary) triggered unchecked indexing
  into an empty `nlohmann::json` array: undefined behavior, not a catchable `json::exception`, so the existing
  per-page `try`/`catch` never saw it. Reported as "every sync-all run dies right when it reaches Emby" — diagnosed
  from real log output showing the process's own startup lines appearing again immediately after "fetching shows:
  Emby / Tutorials," with nothing in between (a clean crash-and-restart, not a hung/slow request). Also fixed the
  same unguarded pattern in `PlexSource`'s device-discovery path (`device_list[0]` on an account with zero
  registered devices). All four fetch call sites now skip the debug line entirely when the page is empty; the
  device-list one now checks `!device_list.empty()` before indexing. Covered by 4 new tests (2 Jellyfin/Emby, 2
  Plex) — manually verified each fails with a real out-of-bounds crash before the fix and passes after, not just
  trusting the diagnosis.
- **Episode-scope Align Start silently abandoned alignment (and any filler) whenever nothing fit the remaining
  gap (Kairos)**: `RuleEngine.cpp`'s episode-alignment padding tries to bridge the gap up to the next
  `align_to_mins` boundary with real filler content, but if `pickFillerSim` couldn't find anything eligible for
  what was left (nothing short enough, or the pool ran dry partway through) it just `break`'d out of the loop —
  leaving `pass.t` wherever it had gotten to instead of snapping the rest of the way to the boundary. The next
  item then started immediately, completely unaligned, with no filler at all — confirmed via a real EPG export
  showing consecutive episodes back-to-back with zero gap on a channel that had filler configured, on transitions
  where the remaining gap was shorter than every available filler clip. Added a `reached_boundary` flag
  distinguishing a genuine success (gap fully closed, or landed within tolerance of a boundary after inserting
  filler) from a failure exit, and a fallback that snaps `pass.t` to the boundary on failure — the same behavior
  the empty-pool case already had, now applied consistently whenever filler can't fully do the job. Covered by a
  new test in `momus/kairos/scheduler/test_rule_engine.cpp` (verified it actually fails without the fix before
  confirming the fix passes it).
- **A block's "Insert filler clips between programs" checkbox did nothing while Episode-scope Align Start was
  active, and its Filler Selection control disappeared even though it still mattered (Hades)**: `RuleEngine.cpp`'s
  episode-alignment padding (snapping each episode to the next `align_to_mins` clock boundary) always fills the
  gap with real filler content to reach that boundary — a deliberate backend design, independent of
  `block.inter_filler` (alignment needs *some* padding mechanism regardless of that separate playback preference).
  But the editor's checkbox for `inter_filler` had no idea about that: unchecking it while episode-scope alignment
  was on had zero effect (dead control), and the "Filler Selection" dropdown right below it — which *does* still
  apply, since alignment padding draws from the same filler pool via the same `pickFillerSim` call — was hidden
  whenever the checkbox read unchecked. `EditorForm.tsx` now shows the checkbox as checked-and-disabled with an
  explanatory hint whenever episode-scope alignment is active (`start_scope === 'episode' && align_to_mins > 0`),
  and keeps Filler Selection visible in that case too, instead of gating both on a setting the scheduler doesn't
  actually consult there.
- **Syncing a Plex/Jellyfin/Emby-linked playlist or filler list failed with "FOREIGN KEY constraint failed" every
  single sync, for as long as the link existed (Kairos)**: `plex_list_link` has no FK constraint tying its `list_id`
  back to `playlist`/`filler_list` (only to `media_source`), so a link whose target got deleted through a path that
  bypasses `PlaylistRepository::remove()`/`FillerRepository::remove()` — both of which already correctly clean up
  their own `plex_list_link` row — survives as an orphan. `POST /api/config/library/reset` was exactly such a path:
  it wipes `playlist`/`playlist_item` with raw SQL and never touched `plex_list_link`. The next `syncPlexLinks()`
  pass then tried to `INSERT INTO playlist_item`/`filler_list_item` for a `playlist_id`/`filler_list_id` that no
  longer existed, which does have a real FK, and died — reported as "sync all silently fails partway through, right
  when it gets to re-syncing linked lists," reproducing on every sync thereafter with no way to recover short of a
  manual DB fix. `syncPlexLinks()` now checks whether each link's target still exists before syncing it and
  self-heals by deleting the stale `plex_list_link` row instead of retrying forever; the library-reset route also
  now deletes `plex_list_link` rows for playlists it wipes, closing the leak at the source. Covered by 3 new tests
  in `momus/kairos/source/test_sync_manager.cpp` (stale playlist link, stale filler-list link, and a sanity check
  that a still-valid link is left alone) — verified the two stale-link tests fail with a real FK-constraint crash
  without the fix before confirming they pass with it.
- **A live channel always showed both a transcode and a direct-stream session active, and the Now Playing panel
  could report more viewers than were actually watching (Hephaestus)**: `POST /stream/channel/:id/start` — the
  capability-bucketed viewer opt-in — unconditionally called `SessionManager::getOrCreate()` for the *default*
  (transcode) bucket on every single call, just to read back the channel's resolved audio track, even for a
  viewer whose capabilities went on to resolve them onto the *native* (direct-stream) bucket instead. Every
  native-eligible viewer therefore spun up a redundant, genuinely-unwatched default-bucket ffmpeg transcode
  alongside the real native session, which then sat active in the Activity list (as a "no viewers detected"
  channel row) until its own idle linger elapsed. Added `SessionManager::peek()` (look-without-create) and had
  the handler reuse whichever bucket, if either, is already running before falling back to creating one — a
  genuinely cold channel still creates a session to learn this, but a warm one (the common case: reconnects,
  multiple viewers, a channel someone's already watching) no longer forces a duplicate. Also excluded
  zero-real-viewer channel sessions (`client_count == 0` and no HLS activity) from `/stream/activity/sessions`
  entirely, so a transient info-only session — the unavoidable cold-start case, or anything like it — never
  renders as a phantom "channel" with nobody actually watching it.
- **Direct-stream (kNativeBucket) live channels had no volume normalization at all (Hephaestus)**: the native
  bucket's `-c:v copy -c:a copy` never ran any filter graph, so `dynaudnorm` (the loudness normalization used by
  the default/transcode bucket) never applied — quiet content just stayed quiet for any direct-stream viewer.
  Video stays a pure stream copy (the actual point of this bucket), but audio is now genuinely re-encoded through
  the same `pushAudioEncoderArgs` path the transcode bucket already uses, so normalization applies there too —
  an audio-only encode is cheap enough not to erode the CPU/GPU savings this bucket exists for. Also relaxed
  `isChannelDirectStreamable`'s eligibility check: it no longer requires the *viewer's* declared capabilities to
  cover the source's audio codec, since audio is no longer copied through unchanged — a source audio codec a
  client doesn't support used to bounce that viewer onto the far more expensive default bucket for an audio-only
  reason that no longer applies.
- **A live channel's native (direct-stream) bucket could start an item from position 0 instead of its real,
  correct wall-clock catch-up offset (Hephaestus)**: `start()`/`transition()` both compute a "gentle speed
  correction" for small scheduling drift — when viable, they zero `startOffset` (the whole point: the item plays
  from its beginning, just slightly faster/slower, closing the drift over its full runtime) and hand `speed` off to
  `spawnFfmpeg()`. But `spawnFfmpeg()` unconditionally forces `speed` back to `1.0` for the native bucket (a stream
  copy can't run the `setpts` filter this needs) *without* undoing the `startOffset` zeroing that decision already
  caused — so a native-bucket item could silently start from the top even though the real, correct wall-clock
  position (computed moments earlier) was nonzero. Most noticeable on short items, where a "small" discarded
  offset is a large fraction of the whole runtime. Both call sites now skip the entire speed-correction branch
  (not just its output) for the native bucket, so `startOffset` always keeps the real offset `computeOffset()`
  produced. Diagnosed from a real production log showing a `default`-bucket transition applying `speed=1.00132`
  while the concurrent `native`-bucket transition for the exact same item silently landed on `offset=0ms` instead.
  Not yet independently confirmed as *the* cause of the still-open periodic stutter above — please retest.
- **Periodic black-frame/stutter on live channels, isolated to the transcode bucket specifically (Hephaestus)**:
  confirmed via the new "Always Transcode" setting — with direct-stream removed from the equation entirely, the
  same glitch still reproduced, ruling out everything native-bucket-specific investigated above. The transcode
  bucket's `-fps_mode cfr` had no explicit `-r` alongside it, leaving ffmpeg to *guess* a target output frame rate
  from the input's own timestamps — the exact timing this same code already didn't trust enough to add `-fps_mode
  cfr` in the first place (VFR/irregular sources). A wrong guess (e.g. rounding a 23.976fps source to a nearby
  rate) means the CFR conversion duplicates/drops frames to correct for a mismatch that doesn't really exist, at a
  periodic beat frequency — a plausible match for a content-independent, repeating glitch. Now pins `-r` to the
  source's own already-probed declared frame rate (`r_frame_rate`) so CFR conversion has a precise target instead
  of guessing one from the same untrustworthy timing it exists to correct. Diagnosed from the user isolating the
  bug to the transcode bucket via live testing. **Confirmed NOT the fix** — user retested live, glitch unchanged.
  Kept the explicit `-r` regardless (still strictly more correct than leaving `-fps_mode cfr` to guess).
- **Root cause found and fixed for the transcode-bucket stutter above (Hephaestus)**: with verbose ffmpeg logs
  showing nothing unusual near glitch moments, and the glitch timing described as "fairly consistent... within a
  window, not exactly" any fixed interval, bisected it directly: temporarily tripled `kLiveHlsSegmentSecs` (the
  forced-keyframe interval) from 2s to 6s as a live test — user confirmed the glitch disappeared entirely at 6s,
  then confirmed it came back at 2s, then confirmed it was gone again at 6s. That conclusively ties the stutter to
  the forced-keyframe cadence itself, not anything content- or frame-rate-related. No `-g` (GOP size) was ever set
  alongside `-force_key_frames`/`-forced-idr`, leaving the encoder to plan its own default GOP length internally
  (NVENC's default is far longer than a 2-second live segment) and then interrupt/replan that structure every
  single time the forced keyframe actually fires — a real, periodic cost landing exactly on the interval, which
  wouldn't produce any log output (consistent with the clean verbose logs). `pushVideoEncoderArgs` (shared by
  live channels, VOD, and preview sessions) now computes `-g` in frames from the source's own real, already-probed
  frame rate (`VideoTrack::r_frame_rate`, via a newly-exposed `parseFrameRateFraction`) and passes it alongside
  `-force_key_frames`, so the encoder knows the real cadence upfront instead of being repeatedly surprised by it.
  Covered by two new tests in `momus/hephaestus/test_encoder_args.cpp`. **Confirmed NOT sufficient on its own** —
  user retested live with `-g` in place at the normal 2s interval, glitch still reproduced. Kept the `-g` fix
  regardless (still strictly more correct). As an interim mitigation while the actual mechanism is still being
  chased, `kLiveHlsSegmentSecs` is bumped 2→6 (confirmed effective via the earlier live A/B/A test) — the tradeoff
  fast-channel-switching vs. glitch-free playback this session hoped to avoid is back in effect for now, until the
  real root cause is found.
- **Two more candidate fixes for the transcode-bucket stutter above, from a deep web research pass (Hephaestus)**:
  found real precedent for this failure class (Jellyfin issue #10283, a near-identical periodic-repeat symptom on
  a different hw backend; ErsatzTV shipping a custom-patched ffmpeg specifically for live-simulated-channel
  encoding reliability) plus two concrete, well-evidenced gaps in our own command line. (1) The live transcode
  bucket ran CQ/VBR with no `-maxrate`/`-bufsize` at all whenever a channel had no admin-configured bitrate
  (`stream_video_bitrate == 0`, the default) — multiple independent sources flag uncapped VBR/CQ as a live-
  streaming anti-pattern specifically because a complex or forced I-frame can spike bitrate arbitrarily, and a
  `-re`-paced real-time pipeline has no slack to absorb that the way file-based encoding would. `ChannelSession`'s
  two `buildArgs`/`buildImageArgs` call sites now fall back to a generous, resolution-scaled default cap
  (`defaultBitrateCapKbps`/`effectiveOutputHeight`, new in `EncoderArgs.cpp`) instead of leaving this genuinely
  uncapped — sized well above normal CQ-23 output so it should never visibly constrain quality, purely bounding
  the pathological spike case. (2) NVENC has its own scene-cut detection that `-force_key_frames` does not
  disable, so a real scene change between two forced keyframes could still insert an extra, unplanned one —
  irregular GOP structure the segmenter isn't expecting. The obvious fix, `-sc_threshold`, is confirmed
  **silently ignored** by `h264_nvenc`/`hevc_nvenc` (NVIDIA developer forum); the flag that actually works there is
  `-no-scenecut 1`, now added to all three NVENC branches (H.264, HEVC, HEVC HDR10 passthrough).
  `libx264`/`libx265` genuinely respect `-sc_threshold`/`x265-params scenecut=0` and now set it explicitly too, for
  the same reason. Both fixes also apply to VOD transcodes for free (shared `pushVideoEncoderArgs`). Covered by
  three new tests in `momus/hephaestus/test_encoder_args.cpp`. Not yet retested live — still at `kLiveHlsSegmentSecs
  = 6` pending confirmation these hold up at the original 2s interval.
- **A dual-bucket channel's default-bucket viewers could drift apart in actual content position from its
  native-bucket viewers of the "same" channel (Hephaestus)**: the speed-correction gate above was keyed on
  "not the native bucket," not on whether a native bucket exists for this channel at all — so on a channel where
  direct-stream is still enabled (`force_transcode=false`), a viewer on the default bucket kept getting smoothly
  speed-corrected while a concurrent native-bucket viewer never does (a stream copy can't run `setpts`). Two
  people nominally watching the same live channel would silently end up at different points in the same content,
  which defeats the entire premise of it being one shared channel. Speed correction in `applyResolvedItem()`/
  `transition()` is now gated on `opts.force_transcode` instead — only ever allowed when this channel has no
  native bucket to stay in sync with in the first place. A dual-bucket channel's default bucket now uses the same
  plain wall-clock seek/skip correction native does, so both buckets stay positionally identical; only a
  force-transcode channel (single bucket, nothing to desync from) still gets the smooth speed nudge.

- **A channel's configured filler never played as the last-resort gap-filler on `/now` (Kairos)**:
  `ScheduleRepository::getChannelFillerFallback()` joined `channel_filler_entry` to `filler_list_item` via the
  legacy `filler_list_id` column — but `channel_filler_entry` has carried generic `content_type`/`content_id`
  columns since migrations v35/v36, and `ChannelRepository::addFillerEntry()` (the only insert path) has never
  populated `filler_list_id`. The join silently matched zero rows for every entry added through the current UI,
  regardless of content type, so a channel that hit a real scheduling gap with no active block/window fell
  straight through to the offline slate instead of playing its configured filler. Rewrote the query to resolve
  each of the four content types (`filler_list`/`playlist`/`show`/`movie`) generically, mirroring how
  `ContentRepository::loadFillerItems()`/`RuleEngine::pickFillerSim()` already handle them elsewhere. Covered by
  new tests in `momus/kairos/db/test_schedule_repository.cpp`.
- **A block's between-episode filler selection mode had no UI control, silently staying on "round-robin" (Hades)**:
  `block.filler_selection` (which filler *list* a block draws from between episodes when more than one is
  attached — separate from the channel-level fallback-filler setting above, and separate from each list's own
  Sequential/Shuffle/Sized clip ordering) was fully wired through the backend and block draft, but no input
  anywhere let a user actually change it — only the channel-level fallback filler's dropdown was reachable,
  which doesn't affect this. A block with more than one filler list therefore always cycled them in the same
  fixed order regardless of intent, reading as "the same fillers keep repeating." Added a "Filler Selection"
  dropdown to `EditorForm.tsx`, shown whenever "Insert filler clips between programs" is checked.
- **Android TV Detail's episode row didn't scroll with D-pad focus (pantheon-android)**: `DetailScreen.kt`'s
  `NoOpBringIntoViewSpec` (added earlier to stop Compose's default focus-scroll from racing the screen's own
  manual header-offset scroll) was provided at the top of the whole season/episode `LazyColumn`, which also
  silently disabled the *nested* per-season `LazyRow`'s own horizontal bring-into-view — D-pad focus moved
  episode-to-episode fine but the row never scrolled to reveal off-screen tiles. Scoped a real
  (edge-aligned) `BringIntoViewSpec` back just to that inner `LazyRow`, restoring normal scrolling there while
  the outer list still suppresses its own.
- **Editing a channel's blocks while it was live could yank the currently-airing item out from under viewers
  (Kairos)**: `ScheduleCache::clear()` (called from nearly every block/content mutation route in
  `BlockService.cpp`) deleted every `scheduled_program` row for the channel unconditionally, including whatever
  was actively on-air — Hephaestus's `ChannelSession` resolves what to stream via `/api/channels/:id/now`, so a
  routine edit (reordering content, changing a weight, not just structural changes) could make the very next poll
  either find nothing or re-resolve to a different item/timing under the freshly-edited blocks mid-playback.
  `clear()` now excludes the row currently airing (`wall_clock_start <= now < wall_clock_end`) from the delete;
  only not-yet-started rows get wiped and regenerated, which is what should pick up the edit.
- **Channel EPG preview always started from "now" instead of the current week (Kairos)**: `POST
  /api/channels/:id/epg/preview` anchored both its generation window and its item filter (`we <= now`) to the
  request time, so a same-day preview showed a truncated view running from the moment of the request to end of
  the 336-hour (2-week) window, instead of the intended full 2 weeks starting from that week's Monday. Preview
  generation now starts from `RuleEngine::weekMondayForChannel()` (channel-timezone aware, matching
  `project()`'s own week-walk) — a "today" preview now shows the whole current week from Monday through 2 weeks
  out, not just the remainder of today.
- **Live channel transitions carried avoidable buffer time (Kairos, Hephaestus)**: direct-stream channel items handed
  ffmpeg a raw start offset on every transition, leaving it to seek-and-search for the nearest real keyframe cold
  every time — `ChannelSession::snapToKeyframe()` now snaps to a real keyframe using the same sync-time keyframe
  cache VOD sessions already got (`Database.cpp`'s v98 migration), exposed on `/now`/`/next`. A new background
  `ChannelSession::prefetchLoop()` also warms Hephaestus's own file-probe cache for the next scheduled item a few
  seconds before a transition needs it, so the audio/subtitle/codec probe is a cache hit instead of a cold ffprobe
  on the hot path.
- **Live-channel HLS playlist rewrites raced ffmpeg's own writes, causing periodic stutter and, at transitions,
  clients briefly playing mismatched old/new-program segments (Hephaestus)**: `playlist.m3u8` was rewritten in
  place (no rename) both by ffmpeg's own HLS muxer and by `ChannelSession::patchDiscontinuitySequence()`'s ~1s
  poll loop, so a reader (Router's file serving, or — the highest-stakes case — a brand-new transition's ffmpeg
  process parsing the existing playlist at startup to continue segment numbering via `hls_flags=append_list`)
  could land mid-write. A torn read there made the new process silently restart segment numbering at
  `seg-00000.ts`, clobbering files a connected client's already-fetched playlist still pointed at. Fixed by adding
  `hls_flags=+temp_file` (ffmpeg now writes segments/playlist to `<name>.tmp` then renames into place) and making
  `patchDiscontinuitySequence()`'s own rewrite rename-based too, so every reader always sees either the fully-old
  or fully-new file. Same `+temp_file` addition applied to `PreviewSession.cpp`'s Guide hover-preview output,
  which has the identical kill-and-respawn-into-the-same-dir pattern on channel switch.
- **Live-channel playback could stall and "chirp" repeatedly after a transition until it failed outright, even
  though the backing encode was still running fine (Hades, Android)**: hls.js's own gap-controller recovers from a
  buffer hole by nudging `currentTime` forward as a *non-fatal* error (`bufferNudgeOnStall`/`bufferSeekOverHole`) —
  each nudge is a tiny forced seek, audible as a chirp — and only escalates to a fatal error after several
  *consecutive* failed nudges within one stall period. That per-stall counter resets the moment playback moves at
  all, so a stream that nudges past one hole, plays a few frames into the next, nudges again, etc. could repeat
  indefinitely without ever tripping the fatal path our existing recovery code was waiting on — the only fix was
  leaving and re-entering the channel. `VideoPlayer.tsx` (shared by `/player`, `/tv`, the Guide preview, and the
  Cast receiver) now tracks these non-fatal stall events in a rolling window, and forces a full hls.js reload
  (fresh MediaSource, re-fetched manifest — the same reset a manual channel re-entry produces) only if
  `currentTime` has also made near-zero real progress across that window, so an ordinary run of separate,
  self-resolving transition gaps isn't mistaken for one genuinely wedged stall. The Android player
  (`PlayerScreen.kt`) needed the same reload mechanism, but keyed off ExoPlayer's own `STATE_BUFFERING`
  instead: that state fires on every ordinary channel transition too (the brief expected gap while a new
  ffmpeg process spins up), so an event-count version of this fix reloaded on essentially every transition in
  practice — confirmed live, made the chirping worse than the bug it was meant to fix. Replaced with a
  position-based watchdog: only reload if `exoPlayer.currentPosition` fails to advance for 10 straight seconds
  while nominally playing, which a normal transition's brief gap is always well under.
- **A slow cache-miss ffprobe at a channel transition could inflate Hephaestus's tracked schedule drift enough
  to force an unnecessary hard seek into (skipping the true beginning of) the next program (Hephaestus)**:
  `item_start` — the anchor `transition()` measures real elapsed playback time against, which feeds directly
  into the drift calculation deciding between a gentle ±2% speed nudge and a hard seek — was set by the caller
  *before* `spawnFfmpeg()`'s synchronous `probeMediaCached()` call, so any real ffprobe latency on a cache miss
  (slow network share, `prefetchLoop()`'s warm-ahead window too short) was silently counted as real paced
  playback time, inflating perceived drift by time ffmpeg wasn't even running yet. `spawnFfmpeg()` now
  re-anchors `item_start` itself, right before the real process launches, after the probe.
- **Deleting a Plex-linked playlist or filler list left an orphaned sync-link row, breaking the next sync with a
  `FOREIGN KEY constraint failed` (Kairos)**: `PlaylistRepository::remove()`/`FillerRepository::remove()` only ever
  deleted the parent row — the matching `plex_list_link` row was only cleaned up by a separate `unlinkPlex()` method
  nobody called from the real delete path. The next `syncPlexLinks()` pass would then try to `INSERT INTO
  playlist_item`/`filler_list_item` for a playlist/filler-list id that no longer existed. Both `remove()` methods now
  delete the link row atomically in the same transaction.
- **VOD concurrency hardening pass (Hephaestus)**: a from-scratch audit of `VodEncodeStream`'s multi-head design
  (each a real ffmpeg process responsible for a segment window, shared across every concurrent viewer of the same
  content) found two real ways two heads could end up writing colliding segment filenames under ordinary use — an
  ordinary forward seek past a lagging head's progress, and direct-stream content whose real keyframe cadence
  doesn't match the assumed-uniform one used when no sync-time keyframe cache is available. Fixed by clamping every
  new head's window against other live heads' declared ranges, evicting a superseded lagging head instead of
  leaving it running, and having `tick()` actively detect and stop a head that's overrun its own declared window.
  Also fixed the shared per-content lock being held across `FfmpegProcess::kill()`'s blocking teardown, which could
  stall every other concurrent viewer of that content for up to ~2s during an eviction.
- **Live channel sessions could get stuck on the connect splash forever if Kairos was unreachable at the moment a
  viewer connected (Hephaestus)**: `transition()` already retried on a mid-session Kairos outage, but `start()`'s
  very first `/now` lookup had no retry path at all — a single failure left the session showing the unbounded
  cold-start splash with nothing left to ever re-poll Kairos, even long after it recovered. `applyResolvedItem()`'s
  failure branch now uses the same bounded-retry shape `transition()`'s own fallback already has.
- **`ChannelSession::current_item` was read across threads with no synchronization (Hephaestus)**: a `KairosNowResponse`
  (several `std::string` fields plus a `vector`) was whole-struct-reassigned on the scheduling thread while other
  threads (the activity-view accessors, and the new prefetch loop above) read it directly — real undefined behavior
  during a `std::string`'s internal reallocation, not just a stale-value risk. Now guarded by a dedicated mutex on
  every read and write.
- **Hephaestus's own ffprobe calls had no timeout (Hephaestus)**: unlike Kairos's own prober, `MediaProbe.cpp`'s
  `runCommand()` ran ffprobe with no bound at all — a stalled/unreachable network mount could hang a probe
  indefinitely, permanently leaking the OS thread it ran on (no per-task pool or reaping exists in `TaskRegistry`).
  Now wrapped in the same `timeout -k 2 <n>` guard Kairos's prober already uses.
- **Kairos handed out scheduled/VOD items with no check that the underlying file actually existed on disk (Kairos)**:
  `/now`, `/next`, and `/playback/:content_type/:id` only ever checked that `file_path` was non-empty — an item
  from a source Kairos can't actually reach on its own filesystem (bad/absent path map, unmounted share) was served
  with total confidence, guaranteeing a downstream ffmpeg spawn failure in Hephaestus with no diagnostic beyond its
  own stderr. All three routes now verify the mapped path resolves on disk, falling through to the next scheduling
  tier (channels) or a clear 404 (VOD) instead.
- **A Roku device re-paired to a different Pantheon account kept showing up under the previous owner (Hermes)**:
  `DeviceSessionManager::getOrCreate()` reused the existing session (and its old `userId()`) on reconnect
  regardless of what `user_id` was actually passed in, so a re-paired device stayed scoped to the previous
  account's "my devices" list — and kept its stale queued commands/reported state — until the old session happened
  to idle out on its own. Reconnecting under a different `user_id` now always gets a fresh session.
- **Concurrent viewers on the same channel could each independently pay the full EPG re-projection cost (Kairos)**:
  `EPGMaterializer::ensureScheduled()`'s "already covers the horizon, skip regenerate" check was a plain
  check-then-act with no lock — several simultaneous `/now`/`/next`/`/epg` requests right as a channel's horizon
  needed extending could each trigger the same (increasingly expensive by Thursday/Friday, per its own comment)
  re-projection instead of one doing the work. Idempotent, not corrupting, but wasteful; now serialized per-channel.
- **yt-dlp playlist downloads showed a misleading progress bar (Kairos, Hades)**: `parseProgress()` only ever read
  the current video's own percentage, which yt-dlp resets to 0% at the start of every item, so a playlist download's
  bar visibly reset and yo-yoed with no indication of overall position. `DownloadManager.cpp` now also parses
  yt-dlp's `Downloading item N of M` line into new `playlist_index`/`playlist_count` fields on the job; the
  Downloads page shows "item N of M" next to the per-file percentage whenever a job is a playlist.
- **Timeslot blocks: dragging a show from the library browser onto the slot list could only append at the end
  (Hades)**: `TimeslotEditor.tsx`'s row drag handlers only recognized in-list reorders and silently ignored an
  external library drag, so the only way to place a new slot was the append-only drop zone below the whole list.
  Rows now also accept a library drag and insert the new slot at the hovered position (`store.insertDraftSlotAt`).
  Also fixed `reorderSlots` discarding `recomputeSlotOffsets`'s return value, leaving `slot_offset_mins` stale after
  every reorder.
- **yt-dlp downloads failed with "Sign in to confirm you're not a bot" on cloud/datacenter hosts (Kairos)**: this
  is YouTube's IP-reputation bot-check, not a rate limit — the existing `--sleep-requests`/`--sleep-interval`
  pacing (added for a separate large-playlist blocking issue) doesn't affect it. `DownloadManager.cpp` now passes
  `--cookies <path>` to yt-dlp when `KAIROS_YTDLP_COOKIES` is set, letting an operator supply cookies exported from
  a real logged-in browser session; unset behaves exactly as before. `docker-compose.yml` (and all 5 generated
  variants) document the env var and add a commented-out volume mount for it.
- **Episode-scope start-time alignment silently did nothing unless "Insert filler clips between programs" was
  also enabled (Kairos)**: `RuleEngine.cpp`'s alignment step was gated on `block.inter_filler`, an unrelated
  playback preference — Hades presents "Align Start" and filler insertion as two independent settings in separate
  sections, so a channel with alignment configured but filler off just played episodes back-to-back off-grid with
  no indication why. Alignment now always runs when `align_to_mins`/episode scope call for it, using the same
  filler pool purely as its padding mechanism (falling back to a contentless snap when the pool is empty, as
  before).
- **Syncs aborted with `CHECK constraint failed: source IN ('tmdb','tvdb','anidb')` (Kairos)**: the
  `item_match_candidate`/`content_request` source CHECK was last widened in migration v51, before the tvmaze/
  trakt/anilist/wikidata scrapers existed — any match candidate or content request from one of those four sources
  hit the constraint and killed the sync. New migration v104 rebuilds both tables with the full 7-source list
  (`Database.cpp`).
- **`TimeslotService.cpp` had no role or ownership check on any of its 9 routes** (`/api/blocks/:bid/slots...`) —
  any authenticated viewer, not just the new self-service channel builder below, could already mutate arbitrary
  channels' timeslot data via a direct API call; no viewer-facing UI happened to expose this, but nothing on the
  server stopped it either. Closed as part of adding the ownership model (`channel_auth::canEditChannelForBlock`
  on every route), not just guarding the new self-service path — see "Self-service channel building" under Added.
- **Guests couldn't save their own channel — the per-user mutation rate limit (60 req/60s) was far too tight for
  how the editor actually saves (Kairos)**: `ChannelDetailStore::saveChannel()` (Hades) fires one HTTP call per
  block create/update/delete, per content item, per filler entry, per timeslot slot, and per queue entry — any
  real schedule easily produces well over 60 calls in a single Save, and `channel_auth::canMutateChannel`/
  `canMutateChannelForBlock` return the exact same `false` for "rate limited" as for "not the owner," so every
  denial surfaced as a blanket, unexplained 403. Compounded by two read-only routes (`GET /api/channels/:id/
  blocks`, `GET /api/channels/:id/bumpers`) mistakenly wired to the rate-limited check instead of the plain
  ownership one, burning the same budget on ordinary page loads. The limit is now a deliberately generous 2000
  req/60s (`ChannelAuth.cpp`'s `kMutationRateLimitCount` — still enough to blunt a scripted flood, effectively
  unreachable by real interactive use) and both read routes go through `canEditChannel` with no rate limit at all.
- **Non-admin sessions spammed four admin-only endpoints with 403s on a fixed poll interval for as long as any
  tab stayed open (Hades)**: `Layout.tsx` unconditionally called `statusStore.startPolling()` (sync/scraper-match/
  writeback status + full `GET /api/config/settings`, all backend-gated to admin) and `systemStore.connectLogs()`
  (the admin-only `/api/logs/stream` SSE connection) for every authenticated session, viewer and guest included —
  `StatusStore._poll()`'s `catch {}` meant these silently retried forever rather than giving up after the first
    403. Both are now gated on `user?.role === 'admin'`, matching the pattern the pending-requests-badge and
         setup-tour-progress effects right above them already used.
- **Guest profiles: no way to add a guest from the "Who's watching?" picker (Hades)**: `LoginPage.tsx`'s "Continue
  as Guest" only ever covered a device with no active session at all — a device that already passed a real
  username/password login (the actual precondition for ever reaching `/profiles`) had no equivalent entry point
  there, so enabling guest profiles and signing out to the picker showed nothing new. `ProfileSelectPage.tsx` gains
  its own "Add Guest" tile (shown only when the same `guest_profiles_enabled` public setting is on), which mints a
  new guest and switches this device's active session onto it exactly the way picking any other existing profile
  tile already does, then continues into the same first-run setup wizard the login-page path uses.
- **`POST /api/config/library/reset` ("Reset Library Index") failed with "FOREIGN KEY constraint failed" whenever
  any show/movie had a per-user track preference set**: reported live as "Can't delete DB because of FK watch
  progress" — SQLite's FK violation error never actually names the offending table, and `watch_progress` turns out
  to have no foreign key to `episode`/`movie`/`show` at all (verified against the live schema via
  `pragma_foreign_key_list`, not just the migration source). The real cause: `show_track_preference`/
  `movie_track_preference` (migrations v92/v94) reference `show(show_id)`/`movie(movie_id)` with no
  `ON DELETE CASCADE`, and this route (which predates both tables) was never updated to clear them before deleting
  the shows/movies they point at — same root-cause class as `AuthStore::deleteUserCascade`'s fix for guest-profile
  deletion (above) for the exact same two tables on the user-deletion path, just a second independent call site
  with the identical gap. `ConfigService.cpp`'s reset now deletes both tables up front, alongside the other
  content-dependent cleanup (`chapter`, `playlist_item`, etc.) it already did. New
  `momus/kairos/api/test_config_service_library_reset.cpp` reproduces the exact reported error message with the
  fix disabled before confirming the fix resolves it.
- **Android: VOD playback always restarted from 0 instead of resuming (Android)**: `PlayerScreen.kt` hardcoded
  ExoPlayer's `MediaItem` start position to `0L` for every VOD session, on a stale assumption (leftover from before
  Hephaestus's VOD sessions moved to a whole-file, absolute-timeline manifest) that the manifest itself always began
  at the resume point. `VodSession::buildStaticPlaylist()` actually emits the entire source file on its own real
  timeline from the first request — `position_ms` only tells Hephaestus which segment to encode first for a fast
  first byte, it never trims or renumbers the playlist — so an explicit client-side seek is required, same as Hades'
  `VideoPlayer.tsx` (`startPosition: startPositionSec ?? 0`) already does. Now seeks to `PlayerViewModel.basePositionMs`
  (already tracked correctly for watch-progress-ping math, just never fed into the player itself). This is what made
  Continue Watching, and every other resume path, start over entirely on Android.
- **Android: "Play" from a shelf (other than Continue Watching) ignored watch progress for movies**: `HomeScreen.kt`
  (mobile + TV) and `DetailViewModel.kt` hardcoded `position_ms = 0` for every movie "Play" action, on the (now
  outdated) assumption that a movie never needed a resume lookup. Kairos's `resolve-play-target` endpoint gains a
  movie counterpart (`GET /api/movies/:id/resolve-play-target`, `PlaybackService.cpp`) backed by
  `WatchProgressRepository::getStates`, and both Android flavors' shelf/hero "Play" actions and `DetailViewModel`'s
  own resolver now call it instead of assuming 0. A show's `PLAY_LATEST_EPISODE` shelf action (e.g. a "New Episodes"
  row) is similarly fixed to resume that specific episode via `GET /api/shows/:id/watch-state` when the viewer is
  actually mid-way through it, rather than always starting at 0.
- **Android: movie playback always timed out (Android)**: `ApiClient.kt`'s shared `OkHttpClient` set no explicit
  timeouts, so `POST /stream/vod/start` ran under OkHttp's default 10s read/write timeout. Direct-stream sessions
  (`VodSession::start()` -> `computeSegmentBoundaries()`, Hephaestus) ran `ffprobe` synchronously over the whole file
  to find real keyframe boundaries before responding — legitimately slower than 10s for a movie-length file, especially
  on network storage — and Android declares wide enough codec support (`[av1,h264,hevc,vp9]`/`[aac,ac3,eac3]`) that
  most movies land on that direct-stream path. Web's `fetch()` has no such timeout and just waits it out, which is why
  only Android was affected. `connectTimeout` stays at 10s (an unreachable server should still fail fast);
  `readTimeout`/`writeTimeout` are now 60s. (The underlying slow probe itself is separately addressed below.)
- **GitHub Release notes rendered as a wall of broken lines**: this file is hard-wrapped at ~120 columns for readable
  diffs/terminal viewing — fine for GitHub's own file-blob renderer (a soft line break just collapses, same as any
  CommonMark reader), but the renderer GitHub uses for Releases (same one as issues/PRs) turns every one of those
  soft breaks into a literal `<br>` instead, so `v0.2.1`'s freshly-backfilled release came out as one forced line
  break per wrapped source line. `.github/workflows/release.yml` now runs the extracted section through a small
  script (`.github/scripts/extract_changelog_section.py`) that joins each entry's wrapped continuation lines back
  into one before handing it to `gh release create`/`edit`. Also fixed two spots in this file where the original
  hard-wrap happened to land mid-token (a long run of `` `code`/`code` `` pairs with no real space to break at,
  e.g. `` `recent-shows`/`recent-movies`/ ``  →  `` `recent-released`/`recent-aired` ``) rather than at a real word
  boundary — the join script can't tell those apart from a normal wrap without guessing wrong, so the two rewrapped
  instead.
- **Local-source-only libraries: detail pages, chapters, track preferences, and watch state all silently came back
  empty**: local items' `kairos_id` embeds the raw filesystem path (unlike Plex/Jellyfin/Emby's opaque ratingKey/GUID
  ids), which broke Kairos's path-segment-based route matching for any endpoint taking one as a URL parameter — the
  request 404'd before ever reaching the database, so a scanned-and-matched item's detail view just rendered blank
  with no visible error. Fixed the same way an equivalent gap in the scraper match routes was fixed previously (a
  slash-tolerant route pattern instead of a single-segment one), applied everywhere else the same shape occurs. New
  local-source items also get an opaque id instead of embedding the path directly, matching how every other source
  already works, so this can't recur for anything scanned going forward — existing libraries are unblocked by the
  route fix alone, no rescan needed. Closing this also surfaced and fixed a separate, source-agnostic route-ordering
  bug in the same area that could shadow a more specific endpoint with a more general one regardless of source type.

### Changed

- **App version now has a single source of truth**: a new root `VERSION` file replaces six separately-maintained
  `0.2.1` literals (root and per-service `CMakeLists.txt` x4, `hades/package.json`), plus a `Layout.tsx` sidebar
  string that was hardcoded to `v0.2.1` and wasn't even wired to anything — bumping the version never updated it.
  Every `CMakeLists.txt`'s `project(... VERSION ...)` now reads `VERSION` via `CMAKE_SOURCE_DIR`, which resolves
  correctly both in the monorepo build and in each service's flattened standalone Docker build. Hades reads it at
  build time (`vite.config.ts`/`vitest.config.ts` inject it as `__APP_VERSION__`) and displays the real version in
  the sidebar. Hades' Docker build context moved from `hades/` to the repo root (matching Hephaestus/Hermes/Kairos,
  whose Dockerfiles already build from the root to reach `shared/`) so it can see `VERSION` too; a new root
  `.dockerignore` keeps that wider context from uploading `build/`, `node_modules/`, etc. Releasing now means
  editing exactly one file.
- **"Direct play" renamed to "direct stream" everywhere (Kairos, Hephaestus, Hermes, Hades, Android, Roku, docs)**: a
  stream copy is still very much a stream, just not a transcoded one — "play" never actually described what this does,
  and the name was inviting confusion with unrelated concepts (e.g. Hades' Home shelf `directPlayPath`, a navigation
  shortcut with nothing to do with this, deliberately left untouched). Covers the `direct_play` JSON wire field (now
  `direct_stream`, GET playback-info responses and the watch-progress ping body alike), the persisted
  `playback_history.direct_play` DB column (migration v97, `RENAME COLUMN` — no data loss), function names
  (`isVideoDirectPlayable`/`isAudioDirectPlayable`/`isDirectPlayable` -> `...DirectStreamable`,
  `simulateDirectPlaySegmentBoundaries` -> `...DirectStream...`), and every user-facing label (Hades' Settings menu
  and Activity → Play History/Now Playing panels: "Direct Play" -> "Direct Stream"). Purely a rename — no behavior
  change on its own.
- **New-channel creation's timezone field now offers the same suggestion list as the channel editor (Hades)**:
  `ChannelsPage.tsx`'s "Add Channel" form was a bare free-text input with no suggestions at all, unlike
  `ChannelDefaultsPanel.tsx`'s timezone field, which has always paired one with a `<datalist>` of common IANA
  zones. The list is now a shared `TIMEZONE_SUGGESTIONS` constant (`channel/constants.ts`) so both forms offer
  the identical set instead of one being strictly worse than the other.

### Added

- **Downloads page: real show-folder picker instead of free text (Kairos, Hades)**: the "show folder" field was a
  plain text input autocompleting over library *names*, with no way to see actual folders on disk. New
  `GET /api/sources/:id/libraries/:lid/show-folders` lists a local library's immediate subfolders; the Downloads
  page gained a library selector that scopes the folder dropdown to it (falls back to manual entry for non-local
  libraries, which have no folder concept).
- **Block editor's panel splits are now drag-resizable (Hades)**: `BlockEditMain.tsx`'s right panel (Content/
  Filler/Bumpers) was hardcoded to `width: 300px`, and the left panel's week-grid schedule was a fixed 188px tall
  over the library browser below it. Both splits now have a draggable divider (right panel 220–600px wide, schedule
  100–500px tall, both clamped), persisted to `localStorage` the same way the week-grid collapse state already was.
- **Home shelves are now server-resolved, fixing "mixed" shelves and closing off a whole class of client-parity
  bug (Kairos, Hades, Android)**: `GET /api/tv/manifest` used to tell every manifest consumer (`/tv`, native
  clients) *which REST endpoint to call* for a shelf's content (`dataSource: {endpoint, params}`) — so every time
  the server gained a new shelf capability, every client needed new code to understand it. This bit twice in two
  weeks: playlist-backed Home shelves silently never appeared on `/tv`/native until 2026-07-25's fix, and "mixed"
  (interleaved show+movie) smart-playlist shelves silently rendered shows-only, because the old
  `smart_type == "movie" ? "/api/movies" : "/api/shows"` selection in `TvManifestService.cpp` had no branch for
  `"mixed"` at all. Home rows now carry an opaque `filter` object instead (content type, sort, canon filter
  string, limit — the same fields `tv_shelf.params_json` already had, just without an endpoint telling the client
  where to send them); every client implements exactly one generic "resolve a filter into tiles" call
  (`GET /api/tv/shelf-items`, new — unlike the manifest itself this requires auth, for restriction/watch-state
  scoping) instead of switching on an endpoint string, so a new shelf shape never needs a client-side change
  again. The hero row's old two-source (shows+movies) client-side fetch-and-art-filter-merge collapses into one
  `content_type: "hero"` filter resolved once, server-side, instead of being duplicated in both `TvHome.tsx` and
  `HomeViewModel.kt`. `tv_shelf`'s `endpoint` column is gone (migration v103), replaced by `content_type`.
  Mixed shelves resolve at movie + individual-episode granularity (never whole shows — same as everywhere else
  "mixed" means in this codebase), which `/tv` and Android had no rendering concept for at all before now (mixed
  shelves never worked there, so it never came up); both gained real episode-tile rendering (own thumbnail,
  "Show — SxEy" caption, tapping jumps straight into that episode) matching desktop's existing
  `HomePage.tsx`/`mixedToShelfEntry` behavior, rather than a lesser stopgap. Desktop `HomePage.tsx` itself is
  unchanged — it already resolved shelves correctly and doesn't consume this manifest.
- **Scheduled jobs + backup/restore, with a new Settings → Jobs tab (Kairos, Hades)**: sync, metadata refresh,
  chapter detection, and metadata writeback have always been fully-working manual operations with no way to run
  them on a timer — `JobScheduler` (`kairos/src/jobs/JobScheduler.h`) existed for exactly this since the
  playback-history/guest-pruning jobs were added, but nothing else had been wired to it yet. It now drives five
  jobs total: `sync` (`SyncManager::triggerSync`, which already runs orphan cleanup/specials linking/chapter sync
  as phases of its own pass — those don't get independent schedules, see below), `metadata_refresh`
  (`ScraperManager::triggerRefreshAll`), `chapter_detection` (a new `ChapterRepository::
  getShowIdsNeedingDetection`, globalizing the "no detected intro/credits yet" eligibility check sync already used
  internally, so this can pick a target independent of sync's own cadence), `writeback_sweep` (`ContentService::
  triggerWritebackAll`, extracted from `POST /api/writeback/all`'s handler so both share one guarded code path),
  and `backup`. Each job is enable/disable-toggleable and schedulable as either a fixed interval or a daily
  UTC time (`JobScheduler` gained `setEnabled`/`setInterval`/`setDaily` plus `listJobs()` status reporting, so a
  Settings change takes effect immediately without a restart); all five default to **disabled** — this is new
  background behavior on an existing self-hosted app, not something that should silently start happening on
  upgrade. New `GET/PATCH /api/jobs`, `POST /api/jobs/:name/run-now` (`JobService`).
  Orphan cleanup was considered for its own job but isn't one: `SyncManager::runOrphanCleanup` consumes a
  `SyncLiveIds` snapshot only a real sync pass produces, so decoupling it would mean either a redundant second
  full-library walk on its own timer or a much larger `SyncManager` refactor — out of scope here.
  **Backup didn't exist anywhere in the codebase before this** — new `kairos/src/backup/BackupManager`
  snapshots the SQLite database (via SQLiteCpp's online-backup API against a dedicated connection, safe against a
  live/actively-written DB) and `kairos.conf` into timestamped pairs under `data/backups/` (inside the existing
  `/data` volume — no new Docker volume needed), with admin-configurable retention (`GET/POST/DELETE /api/backup`,
  `PATCH /api/backup/config`, `BackupService`). Restore (`POST /api/backup/:id/restore`) stages the chosen
  backup's files over the live ones and exits the process — there's no safe way to hot-swap a SQLite file out from
  under an open connection — relying on `docker-compose.yml`'s existing `restart: unless-stopped` to bring Kairos
  back up against the restored data; the Settings UI gates this behind a "restarts Kairos — sure?" confirm, not a
  casual button.
- **Security: "Require Password for Admin Profile Switch" (Kairos, Hades)**: the "Who's watching?" picker
  (`ProfileSelectPage.tsx`/`TvProfileSelect.tsx`) lets a device that already passed a real username/password login
  hop into any profile without re-entering credentials — for admin, that previously meant a 4-6 digit PIN alone was
  enough, a meaningfully weaker credential than the real password guarding an account that can do real damage. New
  Settings → Security toggle (`require_admin_password_switch`, `AuthStore::switchProfile` gains a
  `require_password_for_admin` parameter, enforced server-side — not just left to the client to decide whether to
  show a PIN prompt or a password one, same "can't be bypassed by calling the endpoint directly" posture the
  existing no-pin-admin rule already has) forces the real password every time for admin, PIN configured or not.
  Independent of guest profiles on its own, but turning guest profiles on auto-enables it as a safe default — the
  same "who's watching?" picker showing an admin tile is now reachable by any guest — and deliberately
  one-directional: turning guest profiles back off does not auto-revert it, and the *effective* enforcement
  (`security_settings::requireAdminPasswordSwitchEffective`, exposed via public-settings for the picker's own
  client-side routing) stays "on if guest profiles are on" regardless of the raw stored setting, so it can't be
  quietly weakened while guests remain enabled.
- **Guest profiles — self-service "Continue as Guest" for demo servers (Kairos, Hades)**: admin-toggleable
  (Settings → Guest Access, off by default), gives anyone who reaches the login page a passwordless, viewer-only
  account they create and name themselves — meant for running a public Pantheon demo server without handing out
  real credentials. `POST /api/auth/guest` (public, but 403s unless `guest_profiles_enabled` is on, and rejects once
  `guest_max_concurrent` active guests already exist — an admin-adjustable cap against a publicly-exposed endpoint
  being spammed) creates the account and mints a session in one call, same response shape as `/api/auth/setup`. A
  new first-run wizard (`GuestSetupPage.tsx`) lets the guest configure the exact same fields a normal account has —
  PIN (profile-switch) and parental-control restriction/rating ceilings — minus the admin/viewer role toggle,
  through two new guest-only self-service routes (`PATCH`/`DELETE /api/auth/me/guest`, gated on
  `currentUser()->is_guest` specifically rather than "any authenticated caller editing their own id," so a
  *non-guest* viewer can never use these to loosen restrictions an admin set on them). Pruning is idle-based, not
  creation-based: a background job deletes any guest account with no session activity (`session.last_seen`, already
  tracked) within the admin-configured window (`guest_idle_timeout_days`, default 7) — an actively-used guest
  account never expires on its own. A guest can also delete their own account early from My Account
  ("mindful types"); an admin can delete any guest the same way they delete any other user. New migration v99 adds
  a single `user.is_guest` column — no expiry timestamp is stored, idle time is computed at prune time. Fixed a
  latent bug found while wiring guest deletion: `playback_history`, `show_track_preference`,
  `movie_track_preference`, and `watch_together_session`/`watch_together_member` reference `user(user_id)` without
  `ON DELETE CASCADE`, so deleting *any* account (guest or not) that had ever played something or used Watch
  Together threw a foreign-key constraint violation instead of actually deleting it — `AuthStore::deleteUser` now
  goes through a shared `deleteUserCascade` helper that clears those five tables first.
- **Self-service channel building for guests and granted real accounts (Kairos, Hades)**: the channel builder was
  previously admin-only end to end. New nullable `channel.owner_user_id` + `channel.is_demo` columns (migration
  v101) let a non-admin own a channel; a new `channel_auth::canEditChannel`/`canEditChannelForBlock` helper (admin
  always passes, otherwise only the owner) replaces the old admin-only checks across `ChannelService.cpp`'s filler
  CRUD and update/delete routes and every route in `BlockService.cpp`. Two independent access mechanisms, matching
  how differently trusted the two groups are: a **guest** gets a server-wide toggle
  (`guest_channel_builder_enabled`, off by default and only effective while `guest_profiles_enabled` is also on)
  building against the real library, same as any other viewer, capped at `guest_max_demo_channels` (default 1)
  throwaway channels, deleted automatically along with the guest account (idle prune or self-delete) via the
  existing `ON DELETE CASCADE` chain — no new cleanup code needed. No separate curated-library scoping: guest
  channel building is meant for a separate, already-curated public demo deployment, not a server that also holds
  a personal library. A **real named account** instead gets a per-user admin grant
  (`user.channel_builder_enabled`, same v101 migration, same shape as the existing `restricted` flag — a new
  `PATCH /api/users/:id/channel-builder` route and `UsersPage.tsx` checkbox, deliberately *not* a server-wide
  setting since real accounts are provisioned individually) against the
  real library, capped at `viewer_max_channels` (default 3); their channels are full lineup citizens, unlike a
  guest's. `GET /api/channels` now filters `is_demo=1` channels out for everyone except the owning caller (admin
  still sees everything unfiltered), and `EPGMaterializer::generateM3U`/`generateXMLTV` unconditionally exclude
  them so a guest's demo channel never reaches a real IPTV client's lineup or another user's channel list. Both
  new mutation paths share a `RateLimiter` (one instance on `ServiceContext`, not per-service, so a caller can't
  multiply their effective rate by spreading calls across `ChannelService`/`BlockService`/`TimeslotService`).
  Playback needed no changes at all: Kairos's `/playlist.m3u`/`/epg.xml` and Hephaestus's `/stream/channels/*` were
  already fully unauthenticated by design (for third-party IPTV/XMLTV client compatibility), so a self-built
  channel is watchable the moment it exists. New `momus/kairos/api/test_channel_ownership_routes.cpp` covers the
  full guest/real-viewer/admin permission and visibility matrix.
- **`JobScheduler` — a reusable periodic-job mechanism (Kairos)**: replaces the hand-rolled tick-counter that used
  to live inline in `main.cpp`'s coordinator thread (a single 24h playback-history prune, easy to lose track of
  buried in an unrelated sync-coordination loop). Supports both fixed intervals (`registerInterval`) and
  wall-clock daily jobs (`registerDaily`, UTC) — the latter isn't used yet but is there for upcoming sync/backup
  work, which need "run at a specific time," not just "run every N hours since boot." One background thread,
  each job's exception caught and logged so one broken job can't take down the others or block the rest. The
  playback-history prune migrated over as its first job; the new guest-idle-prune job (above) is its second. New
  `kairos/src/jobs/` — deliberately not under `kairos/src/scheduler/`, which is unrelated EPG/channel scheduling.
- **Android: Watch Together (mobile + TV)**: full native port of the existing web/Hades feature — Kairos and Hermes
  already implemented the whole protocol (session identity/discovery, live position/paused coordination over SSE),
  this just gives the Android app a client. "Watch Together" button on the Detail screen (movies/shows, next to
  Play) creates a session via `POST /api/watch-together` and opens the player as host; a "Watch Together" shelf on
  Home (mirroring `HomePage.tsx`'s `WatchTogetherShelf`, fed by `GET /api/watch-together/active`) lets any other
  viewer join an open session, seeded at the host's live position via Hermes' `POST /watch-together/:id/join`
  instead of starting at 0. `PlayerViewModel`/`PlayerScreen` add: a host-only heartbeat (`WT_HEARTBEAT_MS`, matching
  Hermes' own sync-tick interval) and explicit play/pause/seek command dispatch (via ExoPlayer's
  `onPlayWhenReadyChanged`/`onPositionDiscontinuity(..., DISCONTINUITY_REASON_SEEK)` listeners); a follower-only SSE
  subscription (`ApiClient.openWatchTogetherStream`, via a new `okhttp-sse` dependency — Retrofit has no SSE support)
  applying `sync`/`seek`/`pause`/`play` events with the same drift-tolerant correction PlayerPage.tsx's
  `applyWtEvent` uses; and a small "Hosting/Watching Together" badge with an explicit Leave/End action. One
  deliberate scope trim vs. the web client: a follower's own manual seek via ExoPlayer's native scrub bar isn't
  intercepted into a no-op (media3's default `PlayerView` controller has no easy per-gesture interception point) —
  it still self-corrects via the next command/sync tick, just one round trip later rather than never visibly moving.
- **Kairos: playlist-backed Home shelves now reach `/api/tv/manifest` (and therefore `/tv` and Android)**: a smart
  playlist's "show on home" toggle (`PlaylistRepository::listHomeShelves`) previously only ever rendered on the
  regular web Home page (`HomePage.tsx`'s `customShelves`, via `GET /api/home-playlists`) — `TvManifestService.cpp`
  only ever read the static, admin-seeded `tv_shelf` table, with no code path turning a playlist into a manifest row
  at all. `/tv` and every native client (Android included) silently never showed a playlist-backed shelf that the
  desktop Home page did. Fixed by having the manifest builder additionally query `listHomeShelves()` and append an
  equivalent `shelf`-type row per playlist (`/api/shows` or `/api/movies`, with `limit`/`sort`/`filter`/`home`/
  `hide_empty` params — the exact same `dataSource` shape every `tv_shelf` row already emits), ordered after the
  fixed rows. No client-side changes were needed at all: Android's `HomeViewModel.fetchDataSource` and `/tv`'s own
  manifest consumer already handle any `shelf` row pointed at those two endpoints generically.
- **Android: manifest-driven color theming, actually reaching the UI**: `PantheonTheme.kt` (both flavors) has always
  read real hex values from the manifest's `theme.tokens.colors` (`generate-tv-tokens.mjs`'s output), but only ever
  fed them into the stock Material3 `ColorScheme` slots — every screen's own hand-rolled UI (`HomeScreen`,
  `DetailScreen`, `PlayerScreen`, `GuideScreen`, `LibraryScreen`, `ProfileSelectScreen`, both flavors) defined its own
  local hardcoded `Color(0x...)` constants instead and never read `MaterialTheme.colorScheme` at all, so a manifest
  theme change never actually reached them. New `PantheonColors`/`LocalPantheonColors` (`ui/theme/PantheonColors.kt`)
  exposes the fuller HDS token set (`bg`/`bg2`/`bg3`/`bg4`, `txt`/`txt2`/`txt3`, `gold`/`gold2`/`txtOnGold`,
  `violet`/`violetDeep`, the `match*` semantic colors, `glassBorder`, `discoverAccent`) that Material3's handful of
  semantic slots can't represent without losing information (HDS has multiple background/text tiers; Material3 has
  one background/onBackground pair). Every screen across both flavors now reads `LocalPantheonColors.current`
  instead of a local constant — an admin's theme customization now actually reaches the native app end to end.
- **Android: "Play from Beginning" button on the Detail screen (mobile + TV)**: sits next to the existing "Play"
  button, which now always resumes real progress (see the resume fixes above) — this gives an explicit way to
  restart a movie or a show (from episode 1, not whatever episode/position "Play" would resume into) instead of
  needing to clear watch progress to do it. `DetailViewModel.playFromBeginningTarget()` bypasses
  `resolve-play-target` entirely rather than resolving progress and discarding it.
- **Cached direct-stream keyframe data — no more full-file probe on every playback start (Kairos + Hephaestus)**: a
  direct-stream (stream copy) VOD session can only cut its HLS segments at real keyframes, which meant
  `VodSession::computeSegmentBoundaries()` ran a full packet-level `ffprobe` scan of the whole file synchronously
  inside `POST /stream/vod/start` on *every* session start — the actual root cause of the Android timeout above, and
  a real (if smaller) tax on every other client too. That scan now also runs once during Kairos's existing sync-time
  media probe pass (`SyncManager::syncMediaProbeFromFiles`, alongside the resolution/duration/language probing
  already there — same "empty means never probed" trigger convention, so it backfills automatically for libraries
  synced before this migration) and gets persisted on the `movie`/`episode` row (`keyframes_ms`, migration v98) rather
  than thrown away. `GET /api/playback/:content_type/:id` (Kairos) now returns it; `VodSession` uses it directly
  instead of re-probing — *if* the file's current `stat()` still matches `keyframes_size`/`keyframes_mtime`, captured
  alongside the keyframe data at probe time. That check matters: a stable `file_path` doesn't prove a stable file —
  Sonarr/Radarr-style upgrades replace a library file in place (same path, new content) — so a mismatch (or no cached
  data at all yet) falls back to the same live `ffprobe` scan as before, whose fresh result Hephaestus then pushes
  back to Kairos (`PUT /api/playback/:content_type/:id/keyframes`, internal-token gated like `/played`) so the next
  session on that file doesn't pay for it again either.
- **A `vX.Y.Z` tag push now also publishes a GitHub Release (`.github/workflows/release.yml`)**: previously the
  docker-\*.yml workflows built and pushed versioned GHCR images on a tag push, but nothing ever touched the repo's
  Releases tab — `v0.2.1` shipped images with no release to show for it. This new workflow (independent of the image
  builds — a release doesn't need to wait on those, and a failure in one shouldn't block the other) pulls its notes
  straight from `CHANGELOG.md`'s matching `## [x.y.z]` section, attaches the root `docker-compose*.yml` variants as
  assets (matching what `v0.2.0`'s release already did by hand), and marks a hyphenated version (e.g.
  `v0.3.0-beta.1`) as a pre-release. Fails loudly rather than publishing an empty release if that CHANGELOG section
  doesn't exist yet. Re-runnable via `workflow_dispatch` (with a `tag` input) to backfill a release for a tag pushed
  before this workflow existed, or to republish after a CHANGELOG fix.
- **NFO writeback for local sources, plus match-confirmation recovery after a DB wipe (Kairos, Hades)**: local
  sources were a writeback target in name only — `LocalSource::pushMetadata` was unimplemented, so a `source_mapping`
  row of `source_type='local'` always failed silently. It now writes `movie.nfo`/`<video>.nfo`/`tvshow.nfo` plus
  poster/fanart sidecars (new `SidecarMetadata::saveMovieSidecar`/`saveShowSidecar`), wired into the existing generic
  writeback pipeline (confirmed-match gate, per-source `auto_writeback`/`writeback_update_*` toggles all apply
  unchanged) with zero changes to that pipeline itself. Two tags round-trip through the existing
  `WritebackFields.match_confirmed`/`.locked`: Kodi's own `<lockdata>` convention, and a new Pantheon-specific
  `<pantheon_confirmed>` tag (harmless to other Kodi/Jellyfin/Plex tools, which ignore unknown tags) — new
  `show.nfo_confirmed`/`movie.nfo_confirmed` columns (migration v107) carry that on-disk tag from sync time through
  to the next matching pass, and `matchShow`/`matchMovie`'s existing trusted-ID short-circuit now also restores
  `match_confirmed` (not just `match_status`) when it's set — so wiping the DB and rescanning a local library no
  longer forces re-matching everything that was already confirmed. Safe by construction: that short-circuit only
  ever runs for rows not already `match_status='matched'`, so an explicit later un-confirm can never be silently
  re-applied by a future resync just because the on-disk tag is still sitting there. Ships with a new safety valve
  for the risk this recovery path accepts (a stray/copied NFO could otherwise grant an unearned confirmation):
  `ScraperManager::unconfirmMatch`/`unconfirmAllMatches`, `POST /api/scrapers/queue/:id/unconfirm` /
  `/api/scrapers/unconfirm-all`, and an "Unconfirm All Matches" button in Hades Settings, mirroring the existing
  "Confirm All Matches" flow. **Deployment note:** every `docker-compose*.yml` now mounts the Media volume writable
  (`/media` instead of `/media:ro`) — required for NFO/sidecar writes to reach local libraries at all. Existing
  deployments won't pick this up automatically; re-pull the compose file (or drop `:ro` yourself) to enable it, and
  local-source writeback stays off by default (`auto_writeback`) regardless.

## [0.2.1] - 2026-07-25

### Fixed

- **`POST /api/channels/:id/played` was globally unauthenticated (Kairos)**: `Router.cpp`'s `isPublicPath` had
  `path.ends_with("/played")` with no method restriction — unlike the sibling internal-service rules right above it,
  which correctly scope by method. Since Kairos's port is published directly to the host by default
  (`docker-compose.yml`'s `8081:8080`, documented as "optional; useful for debugging"), any LAN-reachable client could
  POST to this route with zero credentials and corrupt a channel's live scheduling state (`RuleEngine::markPlayed`,
  schedule-cache invalidation) — no token, no service identity, nothing. Fixed with a shared machine secret
  (`ConfStore::getInternalToken()`) gating this one route specifically, since it's the one "public" path that mutates
  state instead of just reading it. The secret is auto-generated into `kairos.conf`'s `[_global]` section on first run
  (`ConfStore`'s constructor, same CSPRNG approach as `AuthStore::generateToken`) — no setup step, no env var, nothing
  for an operator to configure. Hephaestus reads it by parsing that same file directly off the `/data` volume both
  containers already share (`hephaestus/src/kairos/InternalToken.cpp`) rather than fetching it over HTTP, since it must
  never be served by any unauthenticated route (that would defeat the whole point) and Hephaestus has no end-user
  session to authenticate a fetch with. Admin-visible and editable (including a one-click regenerate) via Settings →
  Diagnostics → Internal Service Token (`GET`/`PATCH /api/config/settings`'s new `internal_token` field, deliberately
  kept out of the existing unauthenticated `/api/config/public-settings` subset) — an edit takes effect on Hephaestus's
  very next playback report, no restart of either service required.

- **Data race on `VodSession::video_stream_`/`audio_stream_` (Hephaestus)**: `session_mtx` is documented as guarding
  these two fields, and `ensureAudioTrack()`/`stop()` correctly took it before reassigning/resetting them — but
  `prepareSegment()`/`prepareAudioSegment()` (called per-segment on whatever thread is handling that HTTP request) and
  `lookaheadLoop()` (its own dedicated thread) both read them with no lock at all, racing a plain (non-atomic)
  `shared_ptr` read on one thread against a reassignment/reset on another. `audio_stream_` was the one actually likely
  to trigger it in practice — it gets reassigned mid-session on every audio track switch, while `video_stream_` is
  otherwise fixed until `stop()`. All three now snapshot the shared_ptr under `session_mtx` before using it, held only
  long enough to copy the pointer, not for the (potentially slow) `prepareSegment()`/`tick()` calls themselves.
- **Guide's live preview never actually switched channels (Hades + Android)**: `PreviewSession.h`'s `switchChannel()`
  deliberately reuses the exact same `manifest_url` for a preview session's whole life, so the video player's own load
  effect — keyed on `manifestUrl` in `VideoPlayer.tsx`, on the same in Android's `PreviewPlayerView.kt` — only ever
  fired once, on the *first* focused channel. Every later channel switch retargeted the session server-side, but
  neither hls.js nor ExoPlayer was ever told to reload, so both were left to notice on their own that the server had
  deleted and recreated the segments underneath them — which a live media-sequence reset like that isn't something
  players reliably self-recover from. `GuidePreview.tsx` now remounts `VideoPlayer` on `key={channel.channel_id}`;
  Android's `PreviewPlayerView` gained a separate `reloadKey` (the focused channel id) alongside `manifestUrl` so its
  reload effect fires on either changing — both react to the same "channel changed" signal the channel-header's own
  focus highlight already does, instead of the effectively-constant manifest URL. Also gave Android TV's Guide an
  explicit auto-select-first-channel-on-load fallback (mirroring what mobile's own preview card already had), since
  default D-pad initial focus isn't guaranteed to land inside the grid now that the Home/Library quick-action row sits
  above it.
- **`/tv`'s Home shelves silently missed any row Kairos hadn't been hardcoded for, and the hero never used its own
  declared data source (Hades)**: `TvHome.tsx` fetched exactly four named shelves
  (`recent-shows`/`recent-movies`/`recent-released`/`recent-aired`) instead of iterating every `shelf`-typed row
  `GET /api/tv/manifest` actually returns — a new shelf added server-side (`tv_shelf` table) would render nothing until
  this file was updated too,
  defeating the manifest's entire point. Row fetching is now generic (dispatches on each row's own `dataSource.
  endpoint`), and the hero panel now reads its own `dataSources.shows`/`dataSources.movies` (already modeled in
  `TvManifestService.cpp`'s `heroRowJson()`, never actually consumed) instead of reusing whatever recent-shows/
  recent-movies happened to be configured with. Found auditing native Android for the same gap (see that repo's own
  changelog) — this was the one place web `/tv` had it too.
- **Watch Together: a joining viewer's session didn't catch up to the host, and stayed paused (Hades + Hermes)**:
  joining always started the follower's own VOD session at position 0 and relied on the SSE stream's first `sync` event
  to nudge it into place via a raw `video.currentTime` assignment — which fights hls.js's own internal seek/buffering
  state machine (the same one `startPosition` exists to drive correctly) instead of working with it, and could leave the
  follower stuck fully-buffered-but-paused at the wrong position (pressing play would resume from that stale local
  position). `POST /watch-together/:id/join` on Hermes now forwards the real join to Kairos and merges in the live
  position/paused Kairos never tracks, so the one call the Home shelf was already making returns everything needed — the
  client seeds the new VOD session's start position with it, letting hls.js's own `startPosition` mechanism handle it
  the same reliable way a continue-watching resume already does.
- **No way to end or leave a Watch Together (Hades)**: the close/leave API existed since Phase C/E but had no UI. The
  player now shows a small "Watch Together — Hosting/Following" badge with an End/Leave button (strips `?wt=` from the
  URL, which the existing session-cleanup effect already reacts to); the Home shelf card also gets a close (×) button,
  visible to the host or an admin, for ending a session without needing to join it first.
- **Android TV: clicking the Library search bar never opened the keyboard (Android)**: selecting the search row swaps
  its content from a static `Text` to an `OutlinedTextField` and requests focus on it from a `LaunchedEffect`, but
  Compose fires that field's own `onFocusChanged` once, synchronously, the moment it attaches to the focus tree —
  with `isFocused = false`, since attach always precedes the pending `requestFocus()` call. That spurious first
  "false" ran the same collapse-back-to-button branch a real focus loss does, flipping edit mode back off before
  focus (and the IME `show()` it triggers) ever actually landed — visible as the row's selection animation flashing
  and then immediately replaying its unfocused look, with no keyboard. `LibraryScreen.kt` (tv flavor) now tracks
  whether the field has genuinely gained focus at least once this editing session and only collapses on a loss of
  focus after that.

### Added

- **`/tv`'s Guide is now its own route, matching desktop (Hades)**: `TvGuideSection` used to be embedded inline at the
  bottom of Home, reached via a quick-action button that `scrollIntoView`'d + refocused it — the one place `/tv` hadn't
  gotten the same standalone-route treatment desktop Guide already has. New `TvGuidePage.tsx` at `/tv/guide` renders it
  full-page instead, with its own Home/Library quick-action row (reusing `TvHome.tsx`'s own
  `quickActionRow`/`quickActionButton` styling rather than duplicating it). `TvGuideSection` itself is unchanged. The
  native Android
  client's Guide screen was already its own nav destination on both flavors, but only had a single generic "← Back"
  button — it now gets the same Home + Library pair, styled like its own Home screen's quick-action row.
- **`detail-meta-block` zone gains a `fields` array (Kairos, v97 migration + Hades)**: same principle as v82/v83's
  `filterFields`/`sortOptions` on the library zones — *which* fields Detail's meta-block shows (year/rating/
  content_type) is now server-owned data a client renders, not a fixed hardcoded row per platform. Values match the
  existing hardcoded set exactly, so this is additive only. `TvLibraryDetail.tsx` now renders from `zone('meta-block')
  ?.fields` (falling back to the old fixed set for a manifest that predates it) instead of a hardcoded `Row`; part of a
  wider pass hardening manifest adoption on both `/tv` and the native Android client (Home row iteration, real
  theme-token reads, Guide zone gating, and a full channel×time EPG grid matching Hades' own Guide redesign — see the
  `pantheon-android` repo for the client-side half of that work).
- **Guide is now its own page, with a real progress-aware EPG grid (Hades)**: previously embedded at the bottom of
  Home (reached by scrolling), Guide is now a standalone `/guide` route with its own sidebar nav entry — matching
  Android. The grid itself got a real fix, not just polish: each channel column's header used to be `position: sticky`
  inside the *same* vertically-scrolling region as the program blocks, so blocks routinely rendered underneath it (
  worst-case on load, since the old auto-scroll-to-now effect left only 60px of clearance against an 84px header).
  Headers now live in their own fixed, horizontally-synced row (`GuideGrid.tsx`), so program content never shares a
  scroll axis with them — no more overlap by construction. Behind the grid, a subtle repeating-gradient background marks
  real 30-minute intervals, anchored to true wall-clock half-hour boundaries. The currently-airing block per channel
  replaced its old decorative shimmer (zero connection to elapsed time) with a computed dark/light purple split at the
  real "now" position within that block, plus a pulsing line exactly on the split — an actual progress indicator for the
  first time. Focusing any block (now or future) updates the hero's title/description/rating and, for a future block,
  a "starts in Nm" countdown — but the live video preview always stays locked to the focused *channel*, never a specific
  block, since you can't actually preview something that hasn't aired. The preview itself became a full-bleed hero (live
  video backdrop + scrim + overlaid text, mirroring Home's own `HeroBanner`) instead of a small side-by-side video+info
  row, with the channel's own rating (`Channel.content_tag`, already existed server-side, just never had a UI) shown in
  the header.
- **Default landing page — global + per-user (Kairos + Hades)**: Guide becoming a first-class destination makes "which
  page do I land on" a real choice for the first time. New `app_config` key `default_landing_page` (admin,
  `Settings → Interface`, same shape as the existing `cast_app_id`) plus a per-user `user.default_landing_page`
  override (`Account → General`, empty = inherit the global default, same shape as `default_audio_lang`). Applied at the
  two places that actually decide where a session lands post-auth — `LoginPage`'s deep-link fallback (previously
  hardcoded to `/sources`, which looks like a stale leftover rather than an intentional default — a viewer landing on an
  admin-only route was itself a minor existing bug this replaces) and `ProfileSelectPage`'s three post-switch
  redirects — resolved via a new `resolveLandingPath()` on `AuthContext`, not a permanent identity swap of what `/`
  renders: the sidebar's "Home" link always means Home, only the *initial* landing after login/profile-switch is
  affected.
- **Watch Together — Home shelf + player integration (Hades)**: a new "Watch Together" button (`LibraryDetailActions.tsx`, next to Play) creates a Kairos session for whatever `resolvePlayTarget` would resolve to (a show's actual next episode, same as a plain Play click) and opens it as host. A new Home shelf lists every currently-open session (title, host, live "N watching" count); clicking one joins and opens it as a follower. In the player, `usePlaybackSession`'s existing manifest/session plumbing is untouched — Watch Together layers on top via a `?wt=` query param: the host posts an explicit command (`seek`/`pause`/`play`) on every local action plus a heartbeat every 4s, a follower subscribes to Hermes' SSE stream and applies every event identically (`sync` ticks only correct once drift exceeds 1.5s, explicit commands apply immediately), and a follower's own scrub bar is a no-op (the next correction would just override it anyway). Host vs. follower is decided by comparing the logged-in user against Kairos's own `host_user_id` for the session — never assumed client-side.
- **Watch Together live session coordination (Hermes)**: new `WatchTogetherSession`/`WatchTogetherManager` (`hermes/src/watchtogether/`) hold the in-memory playback state Kairos deliberately never persists — position, paused, who's hosting. `GET /watch-together/:id/stream` is an SSE feed (same chunked-provider + seq/`waitAfter` pull pattern as `/api/logs/stream`): a fresh subscriber's first message is a synthesized `sync` event for the *current* authoritative position, not a backlog of old commands. `POST /watch-together/:id/command` (host-only: `seek`/`pause`/`play`) applies immediately and broadcasts right away; `POST /watch-together/:id/heartbeat` (host-only, cheap/frequent) just updates the extrapolation baseline. A background tick every 4s appends a `sync` event to every live session regardless of host activity — a follower who stalled on a rebuffer or just joined gets corrected without the host touching anything — and a 45s-grace reaper drops sessions whose host has gone quiet, best-effort closing them on Kairos too (using whichever host bearer token the session last saw, since Hermes has no service-account token of its own — Kairos's own age-based sweep from Phase C is the backstop when no host token was ever seen). Getting a session's `host_user_id` on first touch, and forwarding the eventual close, both go through a small new `GET /api/watch-together/:id` Kairos added alongside Phase C's other routes.
- **Watch Together persistence + discovery API (Kairos)**: new `watch_together_session`/`watch_together_member` tables and a `WatchTogetherService` (`POST /api/watch-together` create, `GET /api/watch-together/active` shelf feed, `POST /api/watch-together/:id/join|leave|close`, close host-only). Kairos owns identity/discovery only — no position/paused columns; live playback state stays in-memory on Hermes once that lands, and a closed session's participants just fall back to their own individual `watch_progress`. No visibility gate beyond "logged in" (no household/friend concept exists to key off, and the shelf itself is the discovery surface). A lazy age-based sweep (open >24h with no explicit close) keeps an abandoned session from lingering on the shelf forever until Hermes' own live-presence-based reaping (Phase D) can close these more promptly in practice. First phase of the shared-VOD-stream groundwork landing as an actual feature; join/player wiring (Hades) and live sync (Hermes) are still to come.
- **Smart transcode muxing (Hephaestus)**: when a VOD re-encode is unavoidable for a reason unrelated to codec support (resolution mismatch, subtitle burn-in, SDR tone-map), the transcode target now follows the requesting client's own declared decode capability instead of unconditionally hardcoding H.264 video/stereo AAC audio. `chooseVideoCodec()`/`chooseAudioCodec()` (`EncoderArgs.cpp`) pick the best codec the client has declared support for from a priority list (video: `[hevc, h264]`; audio: `[eac3, ac3, aac]`), each entry owning its own per-`HwAccel` arg-building — bounded by the source's own position in the list, so a re-encode never targets something more "exotic"/modern than the source already was (a HEVC source never transcodes to some hypothetical future AV1 target even if the client supports it — re-encoding up a generation the source never had the headroom for buys nothing but encode time), and a surround audio codec is only ever considered when the source actually has more than stereo to preserve. Falls back to today's exact H.264/stereo-AAC behavior for any client that hasn't declared capabilities, and live-channel/preview code paths (no single-viewer capability to target — a live channel fans one encode out to N simultaneous viewers) are unaffected. Plain AC3 (spec-capped at 5.1) now explicitly downmixes a 7.1 source instead of handing the encoder more channels than the format allows; E-AC-3 carries 7.1 through uncapped.
- **Client-declared resolution cap (Hephaestus)**: `ClientCapabilities` gains an optional `max_height`, accepted by `POST /stream/client-capabilities`. A source taller than the declared cap is no longer eligible for direct play (a stream copy can't downscale) and gets a scale filter applied on the transcode path — `pushScaleFilter`/`resolveMaxHeight` existed already but had no caller anywhere; every VOD transcode previously output at full source resolution regardless of client screen size or bandwidth. No client currently sends `max_height` yet (Hades doesn't declare any capabilities today, video/audio codecs included) — this is the server-side half only.

### Changed
- **VOD audio/video are now independent encoded streams (Hephaestus)**: previously one ffmpeg process muxed video and audio together, so an audio-track switch (`VodSession::ensureAudioTrack`) meant restarting the whole encoder, and an incompatible audio codec forced a video re-encode even when the video itself was perfectly direct-playable. `VodSession` now runs one `VodEncodeStream` per elementary stream — direct-play and transcode-target decisions are made independently per stream, and switching audio tracks only touches `audio_stream_`, leaving video (and its `-hls_start_number`-aligned segments) completely undisturbed.
- **VOD seeking now uses bounded-window "heads" instead of one long-lived paused process (Hephaestus)**: `VodEncodeStream` (new class, replacing the segment/restart machinery `VodSession` used to own directly) covers a stream with a small set of real ffmpeg processes, each responsible for a fixed ~100-segment window rather than the whole remaining file. Revisiting recently-played territory (ordinary scrub back-and-forth) now costs nothing — the head covering it is still there, at worst paused, no kill/restart/wait — and two viewers of a future shared stream sitting far apart in the same file no longer have to fight over one process, each restart evicting the other's progress. A live head — even paused — still holds its NVENC/VAAPI slot, so total live heads per stream is capped, evicting the least-recently-requested one (via the same spawn-before-kill overlap below) once at the limit.
- **`#EXT-X-DISCONTINUITY` replaced with `-output_ts_offset` (Hephaestus)**: every head (video or audio) now rebases its own output onto the real absolute file position it's encoding from, instead of each restart resetting its PTS clock near zero — segment boundaries between different heads now carry genuinely continuous timestamps, so the discontinuity bookkeeping the static playlist used to carry is gone rather than papering over a real jump. Not independently verified against a live player — worth a real playback check.
- **VOD seek latency (Hephaestus)**: `VodSession::restartAt()` (every seek beyond the already-generated range, and a track switch) used to kill the old main-encoder ffmpeg process and wait for it to fully exit (`FfmpegProcess::kill()`'s up-to-2s SIGTERM grace period) before even starting the replacement — the old process's output is worthless the moment a seek is requested, so that wait bought nothing. It now spawns the replacement first and only kills the old one once a short liveness probe (~150ms) confirms the new process didn't immediately die, letting the new process's own startup/first-segment work run in the background during what used to be pure dead time. Falls back to the original kill-then-spawn ordering if the probe fails — the one real risk case, a transcode failing to acquire an NVENC/VAAPI encoder session because the old process hadn't released its slot yet, which the fallback sidesteps since the slot really is free by the time it retries. Direct-play never touches an encoder slot, so it always takes the fast path. Phase timing (spawn/probe/old-kill/fallback durations) is now logged per restart when **Settings → verbose transcode logs** is on, to make future latency work measurable instead of guessed at.
- **VOD video/audio streams are now shared across viewers of the same content (Hephaestus)**: `VodEncodeStream` instances were previously owned outright by a single `VodSession` (one viewer). `VodSessionManager` now keys them by content + the *resolved* transcode decision — `VideoStreamKey` (content_id, direct-play, resolved HDR-passthrough, burn-in track, resolved target codec, resolved max height) and `AudioStreamKey` (content_id, audio track, direct-play, resolved target codec) — and hands out `shared_ptr`s via `getOrCreateVideoStream`/`getOrCreateAudioStream`, stored internally as `weak_ptr`s so a stream nobody's watching anymore tears itself down exactly like a session already did. Two viewers of the same file/quality now share one encode regardless of which audio/subtitle track either picked; only a burn-in selection (baked into the video frame itself) still routes to its own separate video stream. Each `VodSession` facade keeps its own `session_id`/manifest — no client-visible protocol change. This is the foundation the planned Watch Together feature builds on (a synced-position group session needs no new stream plumbing once sharing already works). Segment files for a stream now live under a content+decision-keyed directory rather than any one viewer's session directory, since multiple sessions can reference the same stream.

### Fixed
- **Audio track switch stopped the video and showed a loading spinner (Hades)**: `PlayerPage`'s `handleSelectAudio` still routed every audio pick through `session.reload()` — a brand-new `/stream/vod/start` session, tearing down and reloading the whole player — left over from before audio became its own independent server-side stream. Now a pure client-side selection against the master manifest's `AUDIO` group, same shape as the existing subtitle-track switch: `VideoPlayer` matches the index to hls.js's own `audioTracks[]` via the `X-PANTHEON-INDEX` attribute and sets `hls.audioTrack` directly, which hls.js buffers and swaps in on its own without stopping playback. `usePlaybackSession` gains `selectAudioTrack()` alongside the existing `selectSubtitleTrack()`; `reload()` is now only reached by a burn-in subtitle switch or a genuine seek-driven restart.
- **Audio segment requests 404'd after the video/audio stream split (Hephaestus)**: once audio moved onto its own `aseg-NNNNN.ts`-prefixed segment files (distinct from video's `seg-` prefix, so the two can share a directory without colliding), the `/stream/vod/{id}/audio/{n}/seg-{n}.ts` route regex was never updated off the old literal `seg-` pattern — `aseg-00026.ts` doesn't start with `seg-`, so the route silently never matched at all (a bare 404, no handler ever invoked). Fixed to match `aseg-`.
- **Burn-in subtitle selection silently did nothing (Hades)**: bitmap (PGS/DVD/DVB) subtitle tracks are deliberately excluded from the HLS master manifest's `SUBTITLES` group (`VodSession::buildMasterPlaylist`) since compositing one into the video isn't something HLS's native subtitle mechanism can express — but `TrackMenu`'s selection handler routed every subtitle pick, burn-in included, through the same plain client-side `hls.subtitleTrack` toggle used for real (text) tracks. Since burn-in tracks were never in that manifest group, the selection matched nothing and was silently dropped — contradicting the menu's own "switching tracks will restart playback" tooltip. Burn-in picks now go through `session.reload()` (a real session-level reattach, the same mechanism an audio-track switch used until the fix above), which `usePlaybackSession`'s `reload()` now accepts an explicit `subtitleTrack` override for.
- **Audio/subtitle language options and pills silently empty for some library items**: `SyncManager::syncMediaProbeFromFiles`'s `needs_probe` gate only re-ran ffprobe (the only thing that populates `audio_languages`/`embedded_subtitle_languages`) when `resolution_label` was empty or `duration_ms` looked invalid. A file that arrived with a valid resolution/duration from the source's own metadata (e.g. a Plex-reported value on import) satisfied that gate without Kairos ever locally probing it, leaving those two columns permanently stuck at their untouched `'[]'` default — surfaced as a show with no audio-language pill at all, and the pre-playback language picker (`PlaybackPreferenceSelector`) having nothing to offer even though the file plainly has tracks. The probe now also re-runs whenever `audio_languages` is still at that never-probed default, independent of resolution/duration; self-heals on the next sync, no backfill needed.
- **Nav item re-click on an already-active section no-op'd instead of returning to the top level (Hades)**: Clicking "Library" while viewing an item's detail overlay, or "Review" while a queue item/tab-scoped selection was open, pushed a new history entry to the same URL — `NavLink`'s own click handling always does this — but neither page reacted to it, since their detail/selection state lives in local/store state, not the URL, and nothing was watching for a fresh navigation to the *same* route. `LibraryPage` and `ReviewPage` now reset their open-detail/selection state off `useLocation().key` changing, which fires on every real navigation (including a same-path re-click) but not on in-page filtering/selection, which never calls `navigate()`.
- **A seek during playback could get misread as the video finishing (Hades)**: The native `<video>` `ended` event was wired straight to advance-to-next/mark-completed with no check on where playback actually was — if it fired away from the real end (observed around a seek landing on the edge of the currently-buffered range, which the browser can read as "no more data" before Hephaestus has finished serving the rest of the file), the item got silently marked watched/advanced mid-episode. `PlayerPage.tsx`'s `handleVideoEnded` now only treats the event as real completion when position is within `NATURAL_END_TOLERANCE_MS` (5s) of the session's real duration; otherwise it's treated as spurious and the same `<video>` element is just nudged to resume.
- **Subtitle track potentially desyncing after an initial continue-watching seek (Hades)**: Hades applies the selected `hls.subtitleTrack` as soon as hls.js's `SUBTITLE_TRACKS_UPDATED` fires (right after manifest parse), which can be before hls.js's own internal seek to `startPositionSec` has actually landed on a non-zero resume position — leaving the subtitle-stream-controller's position bookkeeping keyed off wherever `currentTime` happened to be at that earlier moment. `VideoPlayer.tsx` now also re-applies the subtitle track selection on the video's `seeked` event, which reliably fires once the real resume position has been reached.

### Changed
- **Hades: Review page layout**: Tabs (Queue/Groups/Requests/Duplicates/Chapters/Subtitles) moved from a strip inside the list column to a full-width bar across the top of the page, matching the Activity page's tab pattern — the list/detail split now sits underneath as its own row instead of the tabs being scoped to just the list side.
- **Hades: profile/account nav affordance**: The sidebar's "My Account" entry was just the plain username text, with nothing marking it as clickable. Added a small circular person-icon badge in front of the username in both the desktop sidebar and mobile drawer.

### Added
- **Show watched-episode indicator (Kairos + Hades)**: Movies already had a per-user `watched`/`view_count` (rewatch count) exposed on both the list and detail endpoints; shows had no equivalent at all. `ShowRow`/`ShowDetail` gain a `watched_episode_count` (distinct episodes with a completed `watch_progress` row for the caller, aggregated across the show's episodes via a scalar subquery — there's no single show-level file to key a rewatch count off the way a movie has). Library tiles (`MediaCard`) now show a watched checkmark for both movies and shows at every density level (previously gated behind `density === 'rich'` only, so it was invisible in the standard/minimal tile sizes most libraries actually browse in) — a partially-watched show shows an "N/M watched" fraction instead of a plain checkmark. On the detail page, the watched indicator moved out of the admin match-status row next to the Fix Match button (where it read as match-fixing tooling) into the general meta-chip row alongside year/rating/runtime, visible to every viewer.
- **Broken-subtitle review queue now shows file size**: `GET /api/subtitles/broken` (and the recheck/patch responses) stat the sidecar file on read and include `file_size` — not persisted, since the list is small and admin-only; an unusually tiny file size is itself a strong tell for why a subtitle was flagged broken. Shown in both the Review > Subtitles list rows and the file's own inspector panel.

### Added
- **User audio/subtitle language preferences**: Previously, a preferred audio/subtitle track for a show was only ever recorded as a side effect of manually switching tracks mid-playback (`PlayerPage.tsx`'s `saveTrackPreference`), and movies had no equivalent at all — there was no way to choose ahead of pressing play. Kairos gains a `movie_track_preference` table (mirroring the existing `show_track_preference`) plus per-user library-wide default `default_audio_lang`/`default_subtitle_lang` columns; `GET /api/playback/:content_type/:id` now resolves audio/subtitle selection through a three-tier chain — per-item preference, then the viewer's own library-wide default, then server defaults — falling back a level whenever a tier isn't set. Hades exposes both ends of this: a `PlaybackPreferenceSelector` on the show/movie detail hero (auto-saving Audio/Subtitle dropdowns, backed by `GET/PUT /api/{shows,movies}/:id/track-preference`) and a new self-service **My Account** page (`/account`, reachable by any user via clicking their username in the sidebar — not admin-gated, unlike Settings) holding the library-wide defaults (`PATCH /api/users/me/track-preference`).

### Fixed
- **Subtitle track selection silently doing nothing (Hades)**: `VideoPlayer.tsx`'s subtitle-off check was `subtitleTrack < 0`, but external sidecar tracks are encoded as `<= -2` (only `-1` means "off") — selecting any external subtitle track hit the same branch as "disable subtitles." The manifest's `DEFAULT=YES` track happened to mask this on initial load (hls.js re-selects it on its own), which is why only *switching* to a non-default external track looked broken. Fixed both the hls.js and Safari-native-HLS code paths to check `=== -1`.
- **ExoPlayer thrashing every audio track into a real encoder restart (Hephaestus + Android)**: With no `CODECS` attribute on the HLS master playlist, ExoPlayer can't statically know each `AUDIO` group rendition's format and has to open/probe every one — and since `VodSession::ensureAudioTrack` treats any playlist/segment fetch for a non-active track exactly like a real switch, each probe triggered a genuine full `-map 0:a:N` encoder restart. Observed as 20+ back-to-back restarts within seconds of starting playback. `VodSession::buildCodecsAttribute()` now derives an RFC 6381 `CODECS` string (h264 video via a profile-name→profile_idc table + level; audio via a per-codec table covering aac/ac3/eac3/mp3/flac/opus/vorbis) for direct-play sessions, so ExoPlayer no longer needs to probe. Scoped to direct-play only — a transcoded session's real output codec isn't what the source probe describes, so this intentionally omits `CODECS` there rather than guess wrong.
- **cpp-httplib chunked responses (live channels + subtitle pipe) misreported as canceled (Hephaestus)**: Both chunked-content-provider routes called `data_sink.done(); return false;` on a clean finish — `write_content_chunked` treats a `false` return as `Error::Canceled` regardless of `done()` having already written the correct terminating chunk, so every successful stream end logged as a cancellation. Didn't corrupt what reached clients (the terminator was already written), but made diagnosing real subtitle-delivery issues impossible since "response finished success=no" was the *normal* outcome. Both routes now `return true` after `done()`.
- **Broken subtitle sidecar files silently offered as playable tracks**: A file matching the `<video>.<lang>.srt` naming convention isn't necessarily a real subtitle track — found in the wild, a `.es.srt` with a single line in it, which ffmpeg's srt demuxer "extracts" from without erroring (exit code 0, ~0 real cues). Selecting it played nothing, with no indication why. `kairos/src/source/SubtitleValidation.h` now does a cheap content check at sync time (counts real cue/dialogue markers per format) and flags anything implausibly low; `SubtitleTrackRepository::get()` (the playback-serving path) excludes `valid=0` rows so a broken file can no longer be selected at all, while still being recorded (not silently dropped) for a new admin **Review > Subtitles** tab (list, re-check, edit, and delete). Delete removes the actual file from disk (not just the DB row, and gated to `.srt/.ass/.ssa/.vtt` extensions only) — the point is clearing the slot so a downloader like Bazarr treats the language as missing again and fetches a fresh copy; the UI makes that explicit with a named confirm, since a DB-only delete just left the broken file to get silently re-flagged (and re-shown as unfixed) on the next sync.
- **`subtitle_language` filter/facets not excluding broken subtitle files**: The filter's live SQL (`FilterExpr.cpp`) and the language-facet/dropdown queries (`ContentRepository.cpp`, `ContentService.cpp`) all query `subtitle_track` directly rather than through the repository, so they kept matching/listing languages that only existed via a now-invalid sidecar file. All three now require `valid = 1`.
- **A stray/premature `ended` event could wipe real watch progress (Hades)**: `handleAdvanceToNext` (wired to the `<video>` element's native `onEnded`) wrote `completed:true, position_ms: session.durationMs` with no guard — unlike its sibling `handleNaturalEnd`. If a stream failed to fully start, a retry cycle flipping `manifestUrl` could fire a stray `ended` on the outgoing element before the session ever had a real duration, writing `position_ms:0, duration_ms:0, completed:true` and silently overwriting whatever progress was already saved. Added the same `session.durationMs > 0` guard `handleNaturalEnd` already had.
- **Sync could silently resurrect a fixed episode poster/title ("hentai poster" bug)**: The confirmed-match ownership guard that protects show/movie fields from being reclaimed by a lower-trust source after a human confirms a match (`incomingWins()`) was never extended to episodes — `episode.title`/`overview`/`thumb` were gated on the manual `locked` flag alone, so *any* source touching an episode on *any* sync pass could overwrite its thumbnail even when the parent show's match was confirmed. Episodes now reuse the same per-show `incomingWins()` decision shows/movies already enforce.
- **Android track dialog not D-pad focusable, hardware Back exiting the player instead of closing it**: The new track-selection dialog (see Added, below) was a plain `Box` — the exact bug `TvFilterPanel.kt` already hit and documented: painted on top visually (last in composition order) but not a separate focus/window scope, so D-pad input kept landing on whatever the transport controls had focused underneath, and Back fell through to the player's own `BackHandler` (exiting playback). Converted to a real `Dialog` (matching the established pattern elsewhere in the app), which becomes its own focus window and handles the hardware Back button automatically via `onDismissRequest`; added an initial `FocusRequester` on the first row so D-pad has somewhere to land immediately on open.

### Added
- **Telemetry: crash status + play history + cross-platform device list**: Continuing the local-only telemetry groundwork (crash markers, `playback_history` table) — Hades' Activity page gains a new **Telemetry** tab with a crash-status card (aggregated per-service markers via Hermes) and a Tautulli-style play-history table (title, user, device, direct-play, progress, filterable by user). A new `GET /api/activity/active` (Kairos) reads back `playback_history` rows pinged within the last 45s — the same watch-progress data every platform already sends, no new heartbeat mechanism — and **Connected Devices** now merges that with Roku's existing ECP heartbeat into one cross-platform list (web/Android/Roku/Cast), instead of being Roku-only.
- **Android: in-app track-selection dialog**: Replaces ExoPlayer's native settings-gear track dialog, which was collapsing every subtitle entry to the same-looking label and never listing audio languages at all. Built directly off ExoPlayer's live `Tracks`/`Format` objects (no index-translation layer needed, unlike Hades' hls.js integration) — labels resolve through `java.util.Locale` when the manifest only carries a raw language code, and selection applies `TrackSelectionOverride` straight onto the real `TrackGroup`.
- **Telemetry: crash marker acknowledge**: `readCrashMarker`'s own comment always pointed at this — an admin can now actually clear a crash marker (`clearCrashMarker()`, `shared/crash/CrashHandler.h`) instead of it just persisting until an unrelated future crash happens to overwrite it. New admin-gated `DELETE /stream/activity/crash` (Hephaestus), `DELETE /api/activity/crash` (Kairos), and an aggregated `DELETE /api/activity/crash` on Hermes that clears all three at once (own marker + forwards to the other two) — surfaced as an "Acknowledge" action on Hades' `CrashStatusPanel`.
- **Android: broad client-error forwarding**: This app had zero `Log.e`/`Log.w` call sites anywhere — every failure path was a `runCatching{}.getOrNull()`/`.catch{}` with nothing surfaced locally or remotely, so only a literal *uncaught* crash (already forwarded by `PantheonApplication`'s exception handler) was ever visible server-side. A new `RemoteLog` utility, wired into `ApiClient`'s own OkHttp interceptor, now forwards network failures and 5xx responses on every API call to the same `POST /api/logs/client` Hades' `console.error` override already uses — one central hook rather than touching every ViewModel, mirroring `remoteLog.ts`'s own approach. 4xx is deliberately excluded (routinely expected/handled outcomes, not genuine failures) to avoid burying real problems in noise.

### Fixed
- **VOD resume/track-switch restarting playback from 0:00**: Resuming "Continue Watching," switching the audio track, or switching the subtitle track — all three start a brand-new VOD session at a non-zero `position_ms` — could silently reset to the very beginning instead. Root cause: `VodSession::prepareSegment()`'s "hole before this run's start" path unconditionally restarted the main encoder at *whatever* earlier segment index got requested, with no way to tell a genuine backward seek apart from a stray/bootstrap request for an earlier segment (e.g. seg-00000.ts) landing before the real resume target was ever reached — confirmed happening on Safari/AVPlayer's native `<video>` HLS path, which fetches an early segment to resolve initial video metadata before `VideoPlayer.tsx`'s own `currentTime` seek runs. That request tore down the correct in-progress encode and restarted the whole session at position 0. `VodSession` now remembers the segment its session actually started at (`initial_start_segment_`) and only allows the destructive restart-at-an-earlier-segment path once a request at or after that real target has been seen at least once — before that, an early out-of-range request just waits (and 503s if it never materializes) instead of resetting the session.
- **VOD embedded subtitles broken by the sliding-window rearchitecture**: Extracting an *embedded* subtitle track (as opposed to an external `.srt`/`.ass` sidecar) stopped working entirely after VOD's encoder was split into an independent lookahead process. Root cause: `/stream/vod/:id/subs.vtt` (Router.cpp) was tightened from a ~130s combined wait (old single-process design) down to ~9s, on the mistaken assumption that decoupling subtitle extraction from the main encoder made it "small and fast" regardless of source size. It doesn't — `buildVodSubtitleArgs`'s embedded-track ffmpeg invocation (`-map 0:s:N`, no `-ss`, since it now has to cover the whole file for the sliding-window encoder's ability to seek anywhere) is the *only* mapped stream from the primary input, so ffmpeg's demuxer must still sequentially read every interleaved video/audio packet in the entire container to reach it — as slow as a full direct-play remux of a real, possibly many-GB file. External sidecar extraction (`-map 1:s:0`) never had this problem: the big video file is passed as an unused input purely for probing, so ffmpeg's demux loop never reads through it. The handler also silently discarded the wait's completion result and served whatever partial bytes were on disk regardless — since a `<track>`/ExoPlayer sideloaded fetch happens exactly once, a truncated mid-extraction file read as "subtitles don't work" rather than "subtitles are late." Restored the completion-wait budget to the old ~120s and made the handler actually 503 (rather than serve a partial file) if extraction genuinely hasn't finished in time.
- **Subtitles not rendering (Hades + Android)**: A selected sidecar subtitle track (embedded-extracted or external `.srt`/`.ass`) was correctly generated as a WebVTT sidecar and attached to the player, but never actually appeared. On Hades, the `<track>` element is only added to the DOM once the async session-start response resolves `subtitleUrl` — well after the `<video>` has mounted — and browsers don't reliably honor the `default` attribute for a track added after initial parse, so its `TextTrack.mode` is now set to `'showing'` explicitly. On Android, ExoPlayer's `DefaultTrackSelector` treats a sideloaded `SubtitleConfiguration` as available-but-inactive unless it carries `C.SELECTION_FLAG_DEFAULT`, which was missing.
- **Release images**: `docker-{kairos,hermes,hephaestus,hades}.yml` only ever triggered on pushes to `master`, so cutting a `vX.Y.Z` release tag never built or published a matching image — only the always-moving `:latest` and opaque `:sha-<hash>` tags existed, with no way to pin a compose file to a specific stable release. All four now also trigger on `v*.*.*` tag pushes and publish a `:vX.Y.Z` image tag matching the release.

### Added
- **Kairos + Hades: per-operation hot-zone metrics**: Structured timing/CPU/RAM/thread stats for the actual expensive operations — full sync and each of its 7 phases individually, EPG regeneration, scraper matching, and per-source chapter sync — not just the whole-process CPU/RAM gauge the Activity page already had. A new `shared/metrics/OperationMetrics.h` (`OperationRecorder`, RAII, own dedicated ~200ms sampling thread per instance; `OperationMetricsStore`, a bounded in-memory history keyed by operation name) wraps each call site and exposes a new `GET /api/metrics/operations` endpoint on Kairos. EPG regeneration is only instrumented on the actual cache-miss/regenerate branch — `ensureScheduled()` is hit on every `/now` poll, and the existing horizon-covered fast path already skips the expensive part, so wrapping the whole function would've meant spawning a sampler thread on a sub-millisecond hot path for no reason. Surfaced on Hades' Activity page as a new "Hot Zones" table (`OperationMetricsPanel.tsx`) showing last-run time, duration, peak thread count, and avg/max CPU + RAM per operation.
- **Hades: Activity page tabs**: Split into "Monitor" (Sync Status, Writeback, Now Playing, Device Connections, System Resources) and "Debugging" (Hot Zones, Engine Logs) so the page stops growing unboundedly tall as more dashboard cards get added — each tab now scrolls/fills independently instead of every card stacking on one page.
- **Docker Compose variants**: `docker-compose.yml` (plain CPU-only) is joined by five ready-to-use variants — `docker-compose.{nvenc,vaapi}.yml` for NVIDIA/AMD-VAAPI hardware transcoding and a `.cloudflared` version of each (plus the plain one) for Cloudflare Tunnel remote access — so users don't have to hand-edit comments to get a working config for their setup. All six differ only in which lines are commented out (generated from `docker-compose.yml` via `scripts/generate-compose-variants.py`), so switching later is a matter of copying the toggled block into your own already-customized file rather than re-fetching a different one.

### Added
- **Hades**: Activity page's System Resources card now plots per-component RAM usage (Kairos/Hermes/Hephaestus) alongside CPU, in the same slot style as the GPU row. Data was already collected/normalized (`ram_bytes` on each component's metrics); this was a frontend-only addition.

### Fixed
- **Roku**: `TrackMenu.brs` was marking "Off" as the active subtitle row whenever an *external* sidecar subtitle track was actually selected (external tracks use negative indices ≤ -2, same scheme as embedded tracks use ≥ 0, and the check was `currentSubtitle < 0` instead of `= -1`). Selection itself was unaffected — only the highlighted row was wrong. Same bug class already fixed on the web client (`TrackMenu.tsx`, commit `bbf7708`) but never ported to Roku; ported the identical fix.
- **Casting**: Selecting a non-default audio or subtitle track before casting to a Chromecast or Roku device was silently dropped — the cast payload only ever carried content identity/position, so the receiver's fresh session always opened with server defaults (audio auto-select, subtitles off) regardless of what was selected on the sending tab. `CastCustomData`/`CastMediaArgs` now carry the sender's current `audioTrack`/`subtitleTrack`; the Cast receiver's `/player/*` re-navigation and the Roku "load" device command (which `PlayerScreen.brs`'s `startPlayback` already read these fields from, just never received them) both now seed the new session with the same tracks. Mid-cast track switching is still out of scope, unchanged from before.
- **Sync status**: `"[sync] all sources done"` printed before chapter sync (the last and by far longest-running phase) actually ran, so the Activity log claimed sync had finished while the slowest part was still in progress, and the reported total time excluded it. Moved to print after chapter sync instead.

### Changed
- **Sync logging**: The media-probe phase now logs a running `probed N/M: <path> (resolution, audio tracks, external subtitles found)` line per file instead of a plain per-file line with a misleadingly-cumulative subtitle counter, and smart-playlist refresh now logs a "nothing to refresh" line instead of going silent when no smart playlists exist, plus a final done/elapsed-time summary — so the Activity log reflects what each phase is actually doing, not just that "sync" is running somewhere.
- **Sync log tiers**: `[sync]` is now always visible on the Activity log (previously the entire tag was gated behind debug logging, so effectively none of it reached a normal user by default); the per-item/per-batch detail that would otherwise flood the log at that visibility (per-show/per-episode/per-file lines, batch-write progress ticks) moved to a new `[sync-advanced]` tag, gated behind debug logging same as before. The phase-boundary banners (`=== phase N: ... ===`) are also no longer debug-gated, so users can see which phase is actually running. Errors/warnings stayed at `[sync]` regardless of volume.

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
