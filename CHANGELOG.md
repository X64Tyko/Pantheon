# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [Unreleased]

### Added
- **Hermes**: Added `/api/images/proxy` endpoint to safely fetch external media art (like Plex thumbnails) from public domains, bypassing browser Private Network Access (PNA) and CORS restrictions.
- **Hades**: Implemented `mediaUrl` helper to automatically route external URLs through the Hermes image proxy.
- **Testing**: Expanded Kairos test suite with unit and integration tests for image URL generation (TMDB) and database persistence across shows, movies, and episodes.

### Fixed
- **Hades TV**: Harmonized TV mode visuals with the main Hades design system, including hero banner vignettes and glassmorphism effects.
- **Hades TV**: Relocated the library button and optimized vertical shelf spacing for better 10-foot navigation.
- **CI**: Fixed `NOT_BUILT` test errors by scoping CTest execution to specific targets in component workflows.

## [v0.1.0-alpha.1] - 2024-05-20

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
