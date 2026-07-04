#pragma once
#include <string>

// Maps content_rating strings onto small ordered severity scales so restricted
// accounts can be gated by a single ceiling, despite content_rating itself
// being unstructured free text (TMDB gives real values like "TV-MA"/"R", but
// Plex/Jellyfin pass through whatever their own library uses verbatim, and
// AniDB doesn't populate it at all outside the synthetic "Adult" tier below).
//
// TV and movie ratings are different systems, so there are two scales, both
// topped by the synthetic "Adult" tier (see AnidbScraper.cpp) so restricted
// AniDB content is always the single most-restricted category regardless of
// which scale a comparison uses.
//
// Anything not found in the relevant table returns the same severity as the
// top ("Adult") tier — fail closed. An unrecognized or blank rating from an
// inconsistent source library must never silently pass a restricted account.
namespace RatingSeverity {

// TV-Y(0) < TV-Y7(1) < TV-G(2) < TV-PG(3) < TV-14(4) < TV-MA(5) < Adult(6)
int tvRatingSeverity(const std::string& rating);

// G(0) < PG(1) < PG-13(2) < R(3) < NC-17(4) < Adult(5)
int movieRatingSeverity(const std::string& rating);

} // namespace RatingSeverity
