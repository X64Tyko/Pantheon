# Contributing to Pantheon

Thank you for your interest in Pantheon. This project is a **source-available engineering artifact**. It is developed and maintained with a specific architectural vision, and while we welcome feedback and bug reports, we manage contributions differently than a traditional community-led open-source project.

## Governance & Interaction Model

Pantheon is led by a Principal Architect who defines the technical roadmap and implementation details. That's a
bandwidth reality, not a closed-door policy: reviewing and integrating a pull request against a fast-moving,
tightly-coupled architecture takes real, uninterruptible focus time, and there's only one person doing that review
today. To keep that sustainable:

1. **Pull requests come from two paths:** official contributors with standing write access, and invited
   contributions — when a reported issue is verified and a fix fits a specific, pre-approved design, the Architect
   will invite someone to submit exactly that. A PR opened outside either path will be closed, not out of
   unfriendliness, just because it wasn't scoped against the current architecture first — open an issue instead and
   it may well turn into an invitation.
2. **Issue-Driven Fixes:** If you find a bug or have a specific technical fix in mind, please **open an issue**
   first. Provide detailed logs, environment information, and clear reproduction steps — this is genuinely the
   highest-value contribution available to anyone outside the two paths above, and a good one is very likely to get
   picked up.
3. **Architectural Review:** If an issue is verified, the Architect will either implement the fix directly or issue
   one of the invitations described above.
4. **Feature Requests:** Feature requests are welcome as issues but will be prioritized strictly according to the
   Architect's roadmap.

See [About Pantheon](https://x64tyko.github.io/Pantheon/About.html) for the full contribution model, financial
support (not yet set up), and contributor credits.

## Rules of Engagement

*   **Focus on Data:** All bug reports must include logs from `kairos` (typically found in `data/kairos.log`). Issues without logs or reproduction steps may be closed as "incomplete."
*   **Respect the Vision:** Pantheon is built on specific principles like recursive windowing and drift correction. Discussions should focus on how to improve the implementation of these principles rather than suggesting their removal.
*   **Professionalism:** We maintain a strictly technical and professional environment. Please refer to the [Code of Conduct](CODE_OF_CONDUCT.md) for more details.

## Bug Reporting Process

1.  Check the existing issues to see if your problem has already been reported.
2.  Use the [Bug Report Template](.github/ISSUE_TEMPLATE/bug_report.yml) when opening a new issue.
3.  Include your environment details (Docker version, GPU drivers, Host OS).
4.  Attach the relevant sections of your `kairos.log`.

By following these guidelines, you help us maintain Pantheon as a high-quality, professional-grade media automation system.
