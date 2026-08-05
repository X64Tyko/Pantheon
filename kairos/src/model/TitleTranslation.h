#pragma once
#include <string>

// One localized title/overview a scraper found for an item, in an
// unspecified language other than (or including) the one actually applied to
// `Show::title`/`Movie::title`. `language` is an ISO 639-1 code when the
// source tags one, empty for untagged entries (e.g. AniList synonyms).
// Transient scraper-fetch output only — never written to the show/movie
// table directly; ScraperManager upserts these into item_alternate_title.
struct TitleTranslation
{
	std::string language;
	std::string title;
	std::string overview;
};