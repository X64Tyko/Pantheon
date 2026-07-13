#include "TitleMatch.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <regex>
#include <vector>

namespace titlematch {

namespace {

// A standalone 4-digit release year (1900-2099), whether bare ("Title 2023 ...")
// or parenthesised ("Title (2023)"). Word-bounded so it can't fire inside a
// resolution/codec tag like "2160p" or "x264".
const std::regex kYearTokenRe(R"(\b(19\d{2}|20\d{2})\b)");

// Scene-release quality/source/codec/audio/edition tags. The *earliest* match
// marks where junk starts when no year token is present to anchor the cut —
// e.g. "The.Toxic.Avenger.UNRATED.BluRay.x264-GROUP" has no year at all.
const std::regex kJunkTagRe(
    R"(\b(2160p|1080p|720p|480p|360p|4k|8k|uhd|hdr10?|dolby ?vision|)"
    R"(bluray|blu-ray|bdrip|brrip|bdremux|remux|webrip|web-?dl|webdl|hdtv|pdtv|)"
    R"(dvdrip|dvdscr|dvd5|dvd9|hdcam|camrip|cam|telesync|hdrip|)"
    R"(x264|x265|h ?264|h ?265|hevc|avc1?|xvid|divx|)"
    R"(aac(?:2 ?0)?|ac3|eac3|dts(?:-?hd)?|ddp?5 ?1|ddp?7 ?1|atmos|truehd|flac|)"
    R"(proper|repack|internal|limited|extended(?:\s?cut)?|unrated|uncut|)"
    R"(director'?s cut|theatrical(?:\s?cut)?|remastered|imax|10bit|8bit|)"
    R"(yify|yts(?:\.mx)?|rarbg)\b)",
    std::regex::icase
);

// Collection/season/episode descriptors manual renamers or scene uploaders tack onto
// a title that aren't part of the actual title a scraper search needs —
// "Show Name Complete Season 1", "Show.Name.Complete.Seasons.1-3", "Batman
// TV Series 1966". Same role as kJunkTagRe (earliest match becomes a title
// cut point) but kept separate since it's a different category of token,
// not a release/quality tag.
const std::regex kCollectionTagRe(
    R"(\b((?:the\s+)?complete(?:\s+series|\s+collection)?|)"
    R"(tv\s+series|)"
    R"((?:seasons?|series?|episodes)\s*0*\d+(?:\s*-\s*0*\d+)?|)"
    R"(collection)\b|)"
    R"(\d+[_\s]*-[_\s]*\d+)",
    std::regex::icase
);

} // namespace

std::string normalizeTitle(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) r += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto* art : { "the ", "a ", "an " }) {
        if (r.starts_with(art)) { r = r.substr(std::strlen(art)); break; }
    }
    std::string out;
    for (char c : r) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ') out += c;
    }
    return out;
}

// Levenshtein similarity [0,1]
double titleSimilarity(const std::string& a, const std::string& b) {
    std::string na = normalizeTitle(a), nb = normalizeTitle(b);
    if (na == nb) return 1.0;
    if (na.empty() || nb.empty()) return 0.0;

    const size_t m = na.size(), n = nb.size();
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));
    for (size_t i = 0; i <= m; ++i) dp[i][0] = static_cast<int>(i);
    for (size_t j = 0; j <= n; ++j) dp[0][j] = static_cast<int>(j);
    for (size_t i = 1; i <= m; ++i) {
        for (size_t j = 1; j <= n; ++j) {
            int cost = (na[i - 1] == nb[j - 1]) ? 0 : 1;
            dp[i][j] = std::min({ dp[i-1][j]+1, dp[i][j-1]+1, dp[i-1][j-1]+cost });
        }
    }
    int dist = dp[m][n];
    int maxLen = static_cast<int>(std::max(m, n));
    return 1.0 - static_cast<double>(dist) / maxLen;
}

std::pair<std::string, std::optional<int>> parseReleaseTitle(const std::string& raw) {
    // Scene releases use '.'/'_' as word separators; normalise to spaces so
    // the regexes below see real word boundaries ("The.Thing" -> "The Thing").
    // Collapse runs of spaces this creates (also tidies "Mr. Robot"-style names).
    std::string name;
    name.reserve(raw.size());
    bool prev_space = false;
    for (char c : raw) {
        if (c == '.' || c == '_') c = ' ';
        if (c == ' ') {
            if (prev_space) continue;
            prev_space = true;
        } else {
            prev_space = false;
        }
        name += c;
    }

    auto trimmed = [](std::string s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '.' ||
                               s.back() == '-' || s.back() == '('))
            s.pop_back();
        size_t start = s.find_first_not_of(' ');
        return start == std::string::npos ? std::string() : s.substr(start);
    };

    // Look for a year token, skipping one that sits at the very start of the
    // name — that's almost always the title itself ("1917", "2001 A Space
    // Odyssey"), not a release-year marker, so keep scanning for a later one.
    // This only decides the `year` value — where the title actually gets cut
    // is decided below, independently, since a collection/junk tag can sit
    // *before* the year ("Batman TV Series 1966") and still needs to win.
    std::optional<int> year;
    size_t year_pos = std::string::npos;
    for (auto it = std::sregex_iterator(name.begin(), name.end(), kYearTokenRe);
         it != std::sregex_iterator(); ++it) {
        size_t pos = static_cast<size_t>(it->position());
        if (trimmed(name.substr(0, pos)).empty()) continue;
        year = std::stoi((*it)[1].str());
        year_pos = pos;
        break;
    }

    size_t junk_pos = std::string::npos;
    std::smatch jm;
    if (std::regex_search(name, jm, kJunkTagRe) && jm.position() > 0)
        junk_pos = static_cast<size_t>(jm.position());

    size_t coll_pos = std::string::npos;
    std::smatch cm;
    if (std::regex_search(name, cm, kCollectionTagRe) && cm.position() > 0)
        coll_pos = static_cast<size_t>(cm.position());

    // Whichever marker — release year, quality/edition tag, or collection/
    // season descriptor — appears earliest in the name is what the title
    // actually ends at; the year value found above still applies regardless
    // of whether it was the one that won the cut.
    size_t cut = std::min({ year_pos, junk_pos, coll_pos });

    std::string title = trimmed(cut == std::string::npos ? name : name.substr(0, cut));
    return {title, year};
}

} // namespace titlematch
