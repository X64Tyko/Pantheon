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

*The checklist below is generated from the [Beta Complete milestone](https://github.com/X64Tyko/Pantheon/milestone/1)
— don't hand-edit it, edit the issue instead. See `.github/scripts/generate_roadmap.sh`.*

<!-- roadmap-sync:start beta -->
- [ ] [Multi-part movie support (playback + scheduling)](https://github.com/X64Tyko/Pantheon/issues/3) (#3)
- [ ] [fMP4 (fragmented MP4/CMAF) HLS segment support](https://github.com/X64Tyko/Pantheon/issues/4) (#4)
- [ ] [True Direct Play for VOD and channels](https://github.com/X64Tyko/Pantheon/issues/5) (#5)
- [ ] [Bitrate management and user-configurable quality/bitrate caps](https://github.com/X64Tyko/Pantheon/issues/6) (#6)
- [ ] [Native casting (Chromecast) support from pantheon-android](https://github.com/X64Tyko/Pantheon/issues/7) (#7)
- [ ] [External ratings sources (IMDb, Rotten Tomatoes, Metacritic)](https://github.com/X64Tyko/Pantheon/issues/9) (#9)
- [ ] [Webhook-driven sync ingestion (Plex library.new/media.play/media.scrobble, extend to Jellyfin/Emby)](https://github.com/X64Tyko/Pantheon/issues/13) (#13)
- [ ] [Chapter classification quality: reduce the Unclassified bucket](https://github.com/X64Tyko/Pantheon/issues/14) (#14)
- [ ] [User-adjustable subtitle timing offset](https://github.com/X64Tyko/Pantheon/issues/15) (#15)
- [ ] [Player-side subtitle style customization (font, color, outline, drop shadow)](https://github.com/X64Tyko/Pantheon/issues/16) (#16)
- [x] [Demo mode: rate-limit account creation per IP](https://github.com/X64Tyko/Pantheon/issues/17) (#17)
- [x] [Watch Together for Android](https://github.com/X64Tyko/Pantheon/issues/20) (#20)
- [x] [Advanced Search operators (watch state, episode sort, etc.)](https://github.com/X64Tyko/Pantheon/issues/21) (#21)
- [x] [Scheduled Sync & Scan Jobs](https://github.com/X64Tyko/Pantheon/issues/22) (#22)
- [x] [Offline Backup & Restore](https://github.com/X64Tyko/Pantheon/issues/23) (#23)
- [x] [Scheduling robustness for placeholder/empty content](https://github.com/X64Tyko/Pantheon/issues/24) (#24)
- [x] [Home screen shelf reordering](https://github.com/X64Tyko/Pantheon/issues/25) (#25)
- [ ] [Plex external-ID writeback](https://github.com/X64Tyko/Pantheon/issues/26) (#26)
- [ ] [Full .ass/.ssa subtitle styling preservation](https://github.com/X64Tyko/Pantheon/issues/27) (#27)
- [ ] [TTML subtitle support](https://github.com/X64Tyko/Pantheon/issues/28) (#28)
- [ ] [Expanded IPTV support (M3U8 attributes, XMLTV extensions)](https://github.com/X64Tyko/Pantheon/issues/29) (#29)
- [ ] [Performance benchmarking for 100+ concurrent channels](https://github.com/X64Tyko/Pantheon/issues/30) (#30)
- [ ] [User notifications for sync failures and stream interruptions](https://github.com/X64Tyko/Pantheon/issues/31) (#31)
- [ ] [Roku per-client capability manifest update](https://github.com/X64Tyko/Pantheon/issues/32) (#32)
- [ ] [Cast session skip-to-player for authenticated senders](https://github.com/X64Tyko/Pantheon/issues/33) (#33)
- [ ] [Linked-ID editor: resolve and show matched title inline](https://github.com/X64Tyko/Pantheon/issues/34) (#34)
- [ ] [VR content detection and filtering](https://github.com/X64Tyko/Pantheon/issues/35) (#35)
- [ ] [Media downloads for offline playback](https://github.com/X64Tyko/Pantheon/issues/36) (#36)
- [ ] [Plex sign-in (PIN-based OAuth) instead of manual token/URL entry](https://github.com/X64Tyko/Pantheon/issues/43) (#43)
- [ ] [Channel launch seeding: stagger rerun cursors so new channels don't start every show at episode 1](https://github.com/X64Tyko/Pantheon/issues/44) (#44)
<!-- roadmap-sync:end beta -->

## Phase 3: V1.0 Release (Late 2026)
*Focus: Production readiness, security auditing, and long-term support.*

*The checklist below is generated from the [V1.0 milestone](https://github.com/X64Tyko/Pantheon/milestone/2) — don't
hand-edit it, edit the issue instead. See `.github/scripts/generate_roadmap.sh`.*

<!-- roadmap-sync:start v1 -->
- [ ] [Self-hosted watch telemetry to power real recommendations](https://github.com/X64Tyko/Pantheon/issues/8) (#8)
- [ ] [Split Users/Sources/Media into separate databases (blast-radius isolation)](https://github.com/X64Tyko/Pantheon/issues/10) (#10)
- [ ] [Tailscale as an alternative/complement to Cloudflare Tunnel](https://github.com/X64Tyko/Pantheon/issues/11) (#11)
- [ ] [Native (non-Docker) hosting on Windows/Linux/macOS](https://github.com/X64Tyko/Pantheon/issues/12) (#12)
- [ ] [Chapter-aware scheduling](https://github.com/X64Tyko/Pantheon/issues/37) (#37)
- [ ] [Watch-history-driven automated channels](https://github.com/X64Tyko/Pantheon/issues/38) (#38)
- [ ] [Security audit: Hermes gateway and auth flow](https://github.com/X64Tyko/Pantheon/issues/39) (#39)
- [ ] [Public plugin API (scrapers and sources)](https://github.com/X64Tyko/Pantheon/issues/40) (#40)
- [ ] [Versioned stable REST API for third-party integrations](https://github.com/X64Tyko/Pantheon/issues/41) (#41)
- [ ] [Client ecosystem: Apple TV client + pantheon-relay connection-broker mode](https://github.com/X64Tyko/Pantheon/issues/42) (#42)
<!-- roadmap-sync:end v1 -->

---

*Hardware acceleration (VAAPI/QuickSync) support is treated as continuous hardening rather than a roadmap milestone — driver and vendor fragmentation make it an ongoing effort, not a one-time checkbox.*

*Note: This roadmap is subject to change based on architectural requirements and system performance findings.*
