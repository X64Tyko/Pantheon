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
- [ ] **Multi-User & Parental Controls:** Multi-account support with rating-based content restrictions <experimental>.
- [ ] **Bi-Directional Metadata Sync:** Writeback of matches and status to sources (Plex/Jellyfin) <experimental>.
- [ ] **Hardware Acceleration Polish:** Broaden support for VAAPI and QuickSync profiles.

## Phase 2: Beta (Q3 2026)
*Focus: Feature completeness, user experience, and expanded hardware support.*

- [ ] **Chapter Detection:** Automatic scene/intro/outro detection for media files <experimental>.
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
- [ ] **Client Ecosystem:** Development of native applications for TV (Android TV/Apple TV) and mobile.

---

*Note: This roadmap is subject to change based on architectural requirements and system performance findings.*
