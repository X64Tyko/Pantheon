# Security Policy

## Reporting a vulnerability

Email **security@pantheonmedia.app**. Please don't open a public issue for anything that isn't already public
knowledge — give us a chance to ship a fix first.

Include what you'd include in a good bug report: the affected component (Kairos/Hades/Hermes/Hephaestus), a
reproduction case, and the impact as you understand it. We'll acknowledge receipt and follow up as the investigation
progresses; Pantheon is maintained by a small team, so please expect a personal response rather than an automated
SLA.

## Supported versions

Only the latest tagged release is supported. Pantheon is pre-1.0 and moves quickly — if a report affects an older
version, first check whether it still reproduces on the current release before reporting.

## Scope

- The Kairos/Hades/Hermes/Hephaestus services in this repository, and the `pantheon-android`/`pantheon-roku`/
  `pantheon-relay` client apps.
- The public demo at `pantheonmedia.app` is an ordinary self-hosted deployment of this same code, not a separate
  service — a vulnerability that affects it affects every self-hosted install too, and should be reported the same
  way.

Out of scope: third-party services Pantheon integrates with (Plex, Jellyfin, Emby, TMDB, etc.) — report those to
their own maintainers.

## Past disclosures

Pantheon underwent a full internal security audit prior to `v0.3.0` (auth, DDoS/resource-exhaustion, secrets
storage, injection/path-traversal, and deployment/CORS) — see the `## [0.3.0]` entry in
[CHANGELOG.md](CHANGELOG.md) for what was found and fixed. We'd rather find the next one privately, first.
