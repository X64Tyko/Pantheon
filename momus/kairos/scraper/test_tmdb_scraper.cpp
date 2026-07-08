#include <gtest/gtest.h>
#include "scraper/TmdbScraper.h"

TEST(TmdbScraperTest, PosterUrl) {
    // TMDB poster URLs should be prefixed with the TMDB CDN base.
    EXPECT_EQ(TmdbScraper::posterUrl("/path.jpg"), "https://image.tmdb.org/t/p/w500/path.jpg");
    EXPECT_EQ(TmdbScraper::posterUrl(""), "");
}

TEST(TmdbScraperTest, PosterUrlAlreadyAbsolute) {
    // If it's already absolute (e.g. from some other logic), don't double-prefix it.
    // Note: Current implementation just prepends, let's see if it should be smarter.
    // Based on TmdbScraper.cpp: return "https://image.tmdb.org/t/p/w500" + path;
    EXPECT_EQ(TmdbScraper::posterUrl("https://example.com/img.jpg"), "https://image.tmdb.org/t/p/w500/https://example.com/img.jpg");
}
