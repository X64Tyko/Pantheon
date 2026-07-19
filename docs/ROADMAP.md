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
- [ ] **Hardware Acceleration Polish:** Broaden support for VAAPI and QuickSync profiles.

## Phase 2: Beta (Q3 2026)
*Focus: Feature completeness, user experience, and expanded hardware support.*

- [ ] **Plex External-ID Writeback:** Plex's GUID is only changeable via a "match" call that re-scrapes the whole item under a new agent result, not a field edit like everything else in the writeback system — needs its own design before it's safe to automate.
- [ ] **Chapter-Aware Scheduling:** Use detected chapters for precise program start/end and bumper placement.
- [ ] **Advanced Scheduling UI:** Drag-and-drop EPG builder with real-time collision detection.
- [ ] **Collection Management:** Dynamic collections that auto-update based on metadata filters.
- [ ] **Expanded IPTV Support:** Native support for more complex M3U8 attributes and XMLTV extensions.
- [ ] **Performance Benchmarking:** Optimization of the `kairos_core` scheduler for 100+ concurrent channels.
- [ ] **User Notifications:** Alerts for sync failures or stream interruptions.

## Phase 3: V1.0 Release (Late 2026)
*Focus: Production readiness, security auditing, and long-term support.*

- [ ] **Security Audit:** Full review of the Hermes gateway and authentication flow.
- [ ] **Public Plugin API:** Allow community-developed scrapers and source providers.
- [ ] **Stable API:** Versioned REST API for third-party integrations.
- [ ] **Client Ecosystem:** Native applications for TV and mobile. A native Roku channel (`pantheon-roku`) is feature-complete and verified across multiple real-hardware rounds; a native Android/Fire TV app (`pantheon-android`, 4 build flavors) is verified on emulator with the Amazon/Fire TV flavor still unverified on real hardware; a Chromecast HTTPS relay (`pantheon-relay`) is live. Apple TV and a self-hosted/managed relay connection-broker mode remain future work. None of the three client repos are public yet — distribution today is login-gated CI sideload artifacts only.

---

*Note: This roadmap is subject to change based on architectural requirements and system performance findings.*
