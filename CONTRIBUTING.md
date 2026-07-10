# Contributing to Pantheon

Thank you for your interest in Pantheon. This project is a **source-available engineering artifact**. It is developed and maintained with a specific architectural vision, and while we welcome feedback and bug reports, we manage contributions differently than a traditional community-led open-source project.

## Governance & Interaction Model

Pantheon is led by a Principal Architect who defines the technical roadmap and implementation details. To ensure the stability and architectural integrity of the system:

1.  **No Unsolicited Pull Requests:** We do **not** accept direct pull requests for features or refactors. Unsolicited PRs will be closed.
2.  **Issue-Driven Fixes:** If you find a bug or have a specific technical fix, please **open an issue** first. Provide detailed logs, environment information, and clear reproduction steps.
3.  **Architectural Review:** If an issue is verified, the Architect will either implement the fix or, in rare cases, invite a specific contribution based on a pre-approved design.
4.  **Feature Requests:** Feature requests are welcome as issues but will be prioritized strictly according to the Architect's roadmap.

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
