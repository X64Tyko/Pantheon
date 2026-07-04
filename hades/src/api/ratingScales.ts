// Mirrors kairos/src/scraper/RatingSeverity.cpp — keep both in sync.
// "Adult" is a synthetic top tier (see AnidbScraper.cpp), not a real-world
// rating string; it's included here so ceiling dropdowns can express
// "allow everything, including restricted AniDB matches."
export const TV_RATINGS    = ['TV-Y', 'TV-Y7', 'TV-G', 'TV-PG', 'TV-14', 'TV-MA', 'Adult']
export const MOVIE_RATINGS = ['G', 'PG', 'PG-13', 'R', 'NC-17', 'Adult']

// Channels are tagged on the TV scale — see RestrictionRepository.cpp's
// isAllowed() comment for why channels never need the movie scale.
export const CHANNEL_RATINGS = TV_RATINGS
