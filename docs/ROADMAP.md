# Pantheon Roadmap

This document outlines the planned development trajectory for Pantheon. As an architect-led project, priorities are focused on stability, deterministic scheduling, and high-performance transcoding.

## Phase 1: Alpha (Current)
*Focus: Core stability, multi-source synchronization, and basic linear scheduling.*

- [x] **Multi-Source Sync:** Support for Plex, Jellyfin, Emby, and Local Filesystem.
- [x] **EPG Materialization:** In-memory projection of complex scheduling rules.
- [x] **Memory-First State:** Tracking playback and cursors without DB bottlenecks.
- [x] **Discovery & Requests:** Search external sources and push to Sonarr/Radarr <experimental>.
- [x] **Initial Scraper Priority:** Multi-source metadata aggregation (TMDB/TVDB/AniDB) <experimental>.
- [x] **Chromecast Support:** Basic casting to Google Cast devices <experimental>.
- [x] **Log Centralization:** unify frontend and backend logs for better debugging.
- [x] **Metadata Manager:** user-controlled priority reordering and multi-source linking.
- [x] **Cross-Source Merge:** Manual link/merge of duplicate shows and movies discovered across Plex/Jellyfin/local sources.
- [x] **Canonical Settings Model:** Per-item settings (library type, scraper priority, language, etc.) merge into one conflict-checked row instead of being tracked per-source.
- [x] **Multi-User & Parental Controls:** Admin/viewer roles with per-user rating ceilings (TV/movie/channel), enforced on both API and stream requests <experimental>.
- [x] **Bi-Directional Metadata Sync:** Writeback of confirmed matches to Plex/Jellyfin, gated strictly on human-confirmed matches — broad field coverage (title, genres, cast/crew, ratings, labels, collections, art, and on Jellyfin external IDs), per-source auto-writeback and per-field opt-out settings, plus a bulk "Writeback All" scoped by library/source <experimental>.
- [x] **Chapter Detection:** Automatic ad-break/intro-outro detection, with a read-only review tab for QA against the source video <experimental>.
- [x] **Show Specials Linking:** Link a show's OVA/special episode to a movie-library file so it plays via live join while the movie stays independently scheduled.
- [x] **Sync Controls:** Per-library scraper priority, per-item "Skip Scraping" exemption, and "Hard Sync" / "Hard Sync All" to force a full re-sync.
- [x] **Admin Tooling:** Downloadable debug dump and a metrics dashboard on the Activity page.
- [x] **Playlists & Home Shelves:** Static or filter-driven ("smart") named lists, two-way sync + writeback to Plex/Jellyfin/Emby playlists and Plex collections, portable export/import, and Home page shelves (a smart playlist flagged to show on Home, with an optional seasonal `MM-DD` active window).
- [x] **Scraper-Derived Tags:** TMDB keywords, AniList tags, and Wikidata's main-subject claim surfaced as a filterable `tag:` field, usable directly in smart playlists and Home shelves (e.g. an auto-populating `tag:christmas` seasonal shelf).
- [x] **Audio/Subtitle Language Filtering:** Embedded track languages plus external subtitle sidecar files (`.srt`/`.ass`/`.ssa`/`.vtt`) are both filterable and browsable in the player's track menu.
- [x] **Per-Client Direct-Stream Capability:** Clients (currently pantheon-android) can declare their real decode
  capabilities so Hephaestus can direct-stream more than the previous fixed h264/aac allowlist.
- [x] **Advanced Scheduling UI:** Drag-and-drop (click-and-drag paint) EPG builder with priority-based overlap resolution and a live EPG preview.
- [x] **Collection Management:** Covered by the existing Playlists & Home Shelves system — a smart/filter-driven playlist *is* an auto-updating dynamic collection.
- [x] **Watch Together (Synchronized VOD Viewing):** Real-time synchronized on-demand playback across multiple viewers,
  mirroring the wall-clock sync channels already have for live playback — host/follower session state (position, paused,
  join-catch-up), an End/Leave control, and a close (×) affordance from the Home shelf card <experimental>. Landed ahead
  of its original Phase 3 placement below; still Hades (web) only — no Android or Roku support yet, and no automated
  test coverage yet either, so treat it as the newest and least-hardened part of the stack.

## Phase 2: Beta (Q3 2026)
*Focus: Feature completeness, user experience, and expanded hardware support.*

- [x] **Watch Together for Android:** The web (Hades) implementation shipped in Alpha (see above) — bringing
  the same host/follower session sync to the native clients is still open.
- [ ] **True Direct Play:** Direct streaming is up and running, but requires the server to do work that the end client
  can do. We need Direct Play to serve files directly to clients, this should work for VOD and channels when possible.
- [ ] **Plex External-ID Writeback:** Plex's GUID is only changeable via a "match" call that re-scrapes the whole item under a new agent result, not a field edit like everything else in the writeback system — needs its own design before it's safe to automate.
- [ ] **Full .ass/.ssa Subtitle Styling:** Embedded/sidecar `.ass`/`.ssa` tracks are already recognized as text
  subtitles and extracted into an HLS sidecar, but only as plain dialogue — ffmpeg's WebVTT conversion doesn't preserve
  ASS/SSA's own styling (positioning, karaoke, custom fonts/colors), so anything relying on those renders as flat text
  today.
- [ ] **TTML Subtitle Support:** Timed Text Markup Language — a common broadcast/DASH subtitle format not currently
  recognized as either a text or bitmap subtitle codec at all, as an additional format alongside the existing `.srt`/
  `.ass`/`.ssa`/`.vtt` set.
- [ ] **fMP4 Segment Support:** Fragmented MP4 (CMAF-style `.m4s` segments) as an alternative to the current MPEG-TS (
  `.ts`) HLS segment output — broader codec compatibility, needed for some codec/player combinations MPEG-TS doesn't
  cleanly support, and a step toward Low-Latency HLS.
- [ ] **Expanded IPTV Support:** Native support for more complex M3U8 attributes and XMLTV extensions.
- [ ] **Performance Benchmarking:** Optimization of the `kairos_core` scheduler for 100+ concurrent channels.
- [ ] **User Notifications:** Alerts for sync failures or stream interruptions.
- [ ] **Roku Manifest Update:** `pantheon-roku` has been tested and confirmed functional on real hardware, but needs to be updated to use the per-client capability manifest (see above) and re-verified before any public release.
- [ ] **Scheduled Sync & Scan Jobs:** Time-based (cron-style) triggering of the existing sync/scan operations, so
  libraries stay current without a manual sync.
- [ ] **Offline Backup & Restore:** On-demand and schedulable backup/restore of the Kairos database and configuration — reuses the scheduled-jobs mechanism above for the "schedulable" part.
- [ ] **Scheduling Robustness for Placeholder Content:** Shows with no synced episodes yet, and movies still sitting on the Discovery "added" list, shouldn't be able to break channel scheduling if they end up selected into a block.
- [ ] **Cast Session Skip-to-Player:** Skip the profile picker when a cast originates from an already-authenticated account, especially when casting directly to a specific piece of media.
- [ ] **Linked-ID Editor Title Confirmation:** The editable external-ID list (add/reorder/remove) currently shows only raw `source:id` pairs; it should resolve and display the matched title inline so a typo'd ID is caught before saving, not after. (The read-only badge row elsewhere already links out to the source — this only affects the editor.)
- [ ] **VR Content Detection:** File-probe heuristic to flag VR video, plus a library toggle/filter to browse it — same tag/filter infrastructure as Scraper-Derived Tags.
- [ ] **Chapter Classification Quality:** Reduce the "Unclassified" bucket in chapter detection — Episode/Credits already resolve well, but a lot of detected chapters fall through uncategorized. Worth doing before Chapter-Aware Scheduling (V1.0) depends on classification being trustworthy.
- [ ] **Media Downloads:** Download media to device for offline playback.
- [ ] **External ratings:** Add support for exteral rating systems (IMDb, Rotten Tomatoes, Metacritic, etc.)
- [ ] **Advanced Search:** Add more advanced search operators (e.g. filtering by watch state, sorting by episode, etc.)
- [ ] **Home Screen Management:** Reorder shelves on the home screen.

## Phase 3: V1.0 Release (Late 2026)
*Focus: Production readiness, security auditing, and long-term support.*

- [ ] **Chapter-Aware Scheduling:** Use detected chapters for precise program start/end and bumper placement. Deferred out of Beta — this consumes chapter data as a scheduling input rather than just displaying it, which touches the scheduler core and EPG projection directly; architecturally it belongs with the other foundational items below, not alongside Beta's smaller UX/format items.
- [ ] **Watch-History-Driven Automated Channels:** Let a channel's rerun/advancement logic read from actual VOD watch history instead of only shuffle/sequential rules. Bigger than a normal advancement mode: channels are shared broadcast entities, so it needs its own design pass on whose watch state drives a shared schedule before it's safe to build — similar in spirit to the Plex external-ID deferral above, but larger in scope.
- [ ] **Security Audit:** Full review of the Hermes gateway and authentication flow.
- [ ] **Public Plugin API:** Allow community-developed scrapers and source providers.
- [ ] **Stable API:** Versioned REST API for third-party integrations.
- [ ] **Client Ecosystem:** Native applications for TV and mobile. `pantheon-android` (4 build flavors) and
  `pantheon-relay` are public and installable today — Android is verified on real Android/Android TV hardware, with only
  the Amazon/Fire TV flavor still emulator-only and unverified on real Fire TV hardware. `pantheon-roku` is functionally
  verified on real hardware but stays private pending the per-client capability manifest update tracked in Beta above.
  Apple TV and a self-hosted/managed relay connection-broker mode remain future work.

---

*Hardware acceleration (VAAPI/QuickSync) support is treated as continuous hardening rather than a roadmap milestone — driver and vendor fragmentation make it an ongoing effort, not a one-time checkbox.*

*Note: This roadmap is subject to change based on architectural requirements and system performance findings.*
