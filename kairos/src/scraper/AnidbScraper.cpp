#include "AnidbScraper.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Minimal XML helpers (no external parser dependency)
// ---------------------------------------------------------------------------

namespace {

// Text content of the FIRST occurrence of <tag ...>text</tag>.
std::string xmlText(const std::string& xml, const std::string& tag) {
    const std::string open = "<" + tag;
    auto p1 = xml.find(open);
    if (p1 == std::string::npos) return {};
    auto gt = xml.find('>', p1);
    if (gt == std::string::npos) return {};
    if (xml[gt - 1] == '/') return {};  // self-closing
    const std::string close = "</" + tag + ">";
    auto p2 = xml.find(close, gt);
    if (p2 == std::string::npos) return {};
    return xml.substr(gt + 1, p2 - gt - 1);
}

// All occurrences of <tag ...>text</tag> starting from position `from`.
// Returns pairs of (full opening tag string, inner text).
std::vector<std::pair<std::string, std::string>>
xmlAll(const std::string& xml, const std::string& tag, size_t from = 0) {
    const std::string open  = "<" + tag;
    const std::string close = "</" + tag + ">";
    std::vector<std::pair<std::string, std::string>> out;
    size_t pos = from;
    while (pos < xml.size()) {
        auto p1 = xml.find(open, pos);
        if (p1 == std::string::npos) break;
        auto gt = xml.find('>', p1);
        if (gt == std::string::npos) break;
        if (xml[gt - 1] == '/') { pos = gt + 1; continue; }  // self-closing
        std::string tag_str = xml.substr(p1, gt - p1 + 1);
        auto p2 = xml.find(close, gt);
        if (p2 == std::string::npos) break;
        out.emplace_back(std::move(tag_str), xml.substr(gt + 1, p2 - gt - 1));
        pos = p2 + close.size();
    }
    return out;
}

// Attribute value from a tag string like <foo bar="baz">.
std::string xmlAttr(const std::string& tag_str, const std::string& attr) {
    const std::string key = attr + "=\"";
    auto pos = tag_str.find(key);
    if (pos == std::string::npos) return {};
    pos += key.size();
    auto end = tag_str.find('"', pos);
    if (end == std::string::npos) return {};
    return tag_str.substr(pos, end - pos);
}

// Text content between <tag>…</tag> (first occurrence).
std::string xmlSection(const std::string& xml, const std::string& tag) {
    const std::string open  = "<" + tag;
    const std::string close = "</" + tag + ">";
    auto p1 = xml.find(open);
    if (p1 == std::string::npos) return {};
    auto gt = xml.find('>', p1);
    if (gt == std::string::npos) return {};
    auto p2 = xml.find(close, gt);
    if (p2 == std::string::npos) return {};
    return xml.substr(gt + 1, p2 - gt - 1);
}

// Decode common XML entities.
std::string xmlDecode(std::string s) {
    for (auto [ent, rep] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
            {"&amp;","&"}, {"&lt;","<"}, {"&gt;",">"}, {"&quot;","\""}, {"&apos;","'"}})
    {
        size_t pos = 0;
        while ((pos = s.find(ent, pos)) != std::string::npos) {
            s.replace(pos, ent.size(), rep);
            pos += rep.size();
        }
    }
    return s;
}

int yearFromDate(const std::string& d) {
    if (d.size() >= 4) { try { return std::stoi(d.substr(0, 4)); } catch (...) {} }
    return 0;
}

// Levenshtein-based title similarity [0, 1].
double titleSim(const std::string& a, const std::string& b) {
    auto norm = [](const std::string& s) {
        std::string r;
        for (char c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ') r += c;
        }
        return r;
    };
    std::string na = norm(a), nb = norm(b);
    if (na == nb) return 1.0;
    if (na.empty() || nb.empty()) return 0.0;
    const size_t m = na.size(), n = nb.size();
    std::vector<int> prev(n + 1), curr(n + 1);
    for (size_t j = 0; j <= n; ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= m; ++i) {
        curr[0] = static_cast<int>(i);
        for (size_t j = 1; j <= n; ++j) {
            int cost = (na[i-1] == nb[j-1]) ? 0 : 1;
            curr[j] = std::min({prev[j]+1, curr[j-1]+1, prev[j-1]+cost});
        }
        std::swap(prev, curr);
    }
    return 1.0 - static_cast<double>(prev[n]) / static_cast<double>(std::max(m, n));
}

} // namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

AnidbScraper::AnidbScraper(std::string client_name)
    : client_name_(std::move(client_name))
    , api_client_("http://api.anidb.net:9001")   // AniDB HTTP API is plain HTTP on port 9001
    , dump_client_("https://anidb.net")
{
    api_client_.set_connection_timeout(15);
    api_client_.set_read_timeout(30);
    dump_client_.set_connection_timeout(30);
    dump_client_.set_read_timeout(120);
    dump_client_.set_default_headers({{"User-Agent", "kairos/1.0 (https://github.com/X64Tyko/Pantheon)"}});
    // Allow an immediate first request
    last_api_call_ = std::chrono::steady_clock::now() - std::chrono::seconds(10);
}

// ---------------------------------------------------------------------------
// Rate limiting — 2.1 s between API calls
// ---------------------------------------------------------------------------

void AnidbScraper::rateLimitWait() {
    std::lock_guard lock(rate_mu_);
    auto now     = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_api_call_);
    constexpr auto kGap = std::chrono::milliseconds(2100);
    if (elapsed < kGap)
        std::this_thread::sleep_for(kGap - elapsed);
    last_api_call_ = std::chrono::steady_clock::now();
}

// ---------------------------------------------------------------------------
// Title dump — download + cache 24 h
// ---------------------------------------------------------------------------

bool AnidbScraper::ensureTitleDump() {
    std::error_code ec;
    if (fs::exists(kTitlesXml, ec)) {
        auto ftime = fs::last_write_time(kTitlesXml, ec);
        if (!ec) {
            auto age = std::chrono::duration_cast<std::chrono::hours>(
                fs::file_time_type::clock::now() - ftime);
            if (age.count() < 24) return true;
        }
    }

    std::cout << "[anidb] downloading title dump\n";
    auto res = dump_client_.Get("/api/anime-titles.xml.gz");
    if (!res || res->status != 200) {
        std::cerr << "[anidb] title dump download failed (HTTP "
                  << (res ? res->status : 0) << ")\n";
        return fs::exists(kTitlesXml, ec);  // fall back to stale file
    }

    {
        std::ofstream f(kTitlesGz, std::ios::binary);
        if (!f) { std::cerr << "[anidb] cannot write " << kTitlesGz << '\n'; return false; }
        f.write(res->body.data(), static_cast<std::streamsize>(res->body.size()));
    }

    if (std::system(("gunzip -f " + std::string(kTitlesGz)).c_str()) != 0) {
        std::cerr << "[anidb] gunzip failed\n";
        return fs::exists(kTitlesXml, ec);
    }

    std::cout << "[anidb] title dump updated\n";
    return true;
}

// State-machine parser for the title dump XML.
// Each <anime aid="N"> block is on its own line; titles follow one per line.
std::vector<AnidbScraper::TitleMatch>
AnidbScraper::searchTitleDump(const std::string& query) const {
    std::ifstream f(kTitlesXml);
    if (!f.is_open()) return {};

    std::vector<TitleMatch> out;
    std::string line, cur_aid, en_official, en_any, main_title;

    auto finishEntry = [&]() {
        if (cur_aid.empty()) return;
        const std::string& best = !en_official.empty() ? en_official
                                : !en_any.empty()      ? en_any
                                                       : main_title;
        if (!best.empty()) {
            double sc = titleSim(query, best);
            if (sc > 0.35) out.push_back({cur_aid, best, sc});
        }
        cur_aid.clear(); en_official.clear(); en_any.clear(); main_title.clear();
    };

    while (std::getline(f, line)) {
        if (line.find("<anime aid=\"") != std::string::npos) {
            finishEntry();
            auto p1 = line.find("aid=\"");
            auto p2 = line.find('"', p1 + 5);
            if (p1 != std::string::npos && p2 != std::string::npos)
                cur_aid = line.substr(p1 + 5, p2 - p1 - 5);
            continue;
        }
        if (line.find("</anime>") != std::string::npos) { finishEntry(); continue; }
        if (cur_aid.empty() || line.find("<title") == std::string::npos) continue;

        const bool is_en      = line.find("xml:lang=\"en\"") != std::string::npos;
        const bool is_official = line.find("type=\"official\"") != std::string::npos;
        const bool is_main    = line.find("type=\"main\"") != std::string::npos;

        auto gt = line.find('>');
        auto cl = line.rfind("</title>");
        if (gt == std::string::npos || cl == std::string::npos || cl <= gt) continue;
        std::string t = xmlDecode(line.substr(gt + 1, cl - gt - 1));

        if (is_en && is_official)           en_official = t;
        else if (is_en && en_any.empty())   en_any      = t;
        else if (is_main && main_title.empty()) main_title = t;
    }
    finishEntry();

    std::sort(out.begin(), out.end(), [](const TitleMatch& a, const TitleMatch& b){
        return a.score > b.score;
    });
    if (out.size() > 15) out.resize(15);
    return out;
}

// ---------------------------------------------------------------------------
// Fetch anime detail XML from AniDB HTTP API
// ---------------------------------------------------------------------------

std::string AnidbScraper::fetchAnimeXml(const std::string& aid) {
    // Check 24-hour disk cache before hitting the rate-limited API.
    fs::path cache_dir  = kXmlCacheDir;
    fs::path cache_file = cache_dir / (aid + ".xml");
    try {
        fs::create_directories(cache_dir);
        struct stat st{};
        if (stat(cache_file.c_str(), &st) == 0 &&
            time(nullptr) - st.st_mtime < 86400) {
            std::ifstream f(cache_file);
            return std::string((std::istreambuf_iterator<char>(f)), {});
        }
    } catch (...) {}

    rateLimitWait();
    const std::string path = "/httpapi?request=anime&aid=" + aid
        + "&client=" + client_name_ + "&clientver=1&protover=1";
    auto res = api_client_.Get(path);
    if (!res || res->status != 200) {
        std::cerr << "[anidb] fetchAnimeXml aid=" << aid
                  << " failed (HTTP " << (res ? res->status : 0) << ")\n";
        return {};
    }
    if (res->body.find("<error>") != std::string::npos ||
        res->body.find("<error ") != std::string::npos) {
        std::cerr << "[anidb] API error for aid=" << aid
                  << ": " << res->body << '\n';
        return {};
    }

    try {
        std::ofstream f(cache_file);
        f << res->body;
    } catch (...) {}

    return res->body;
}

std::string AnidbScraper::posterUrl(const std::string& aid) {
    auto xml = fetchAnimeXml(aid);
    if (xml.empty()) return {};
    const std::string pic = xmlText(xml, "picture");
    if (pic.empty()) return {};
    return std::string(kImgBase) + pic;
}

// ---------------------------------------------------------------------------
// Parse anime detail XML → Show
// ---------------------------------------------------------------------------

Show AnidbScraper::showFromXml(const std::string& xml, const std::string& aid) {
    Show s;
    s.show_id = aid;  // caller replaces with kairos_id after resolution

    // Best English title
    const std::string titles_sec = xmlSection(xml, "titles");
    for (auto& [tag, text] : xmlAll(titles_sec, "title")) {
        const bool en  = tag.find("xml:lang=\"en\"") != std::string::npos;
        const bool off = tag.find("type=\"official\"") != std::string::npos;
        const bool main = tag.find("type=\"main\"") != std::string::npos;
        if (en && off)             { s.title = xmlDecode(text); break; }
        if (en && s.title.empty()) s.title = xmlDecode(text);
        if (main && s.title.empty()) s.title = xmlDecode(text);
    }
    if (s.title.empty()) s.title = aid;

    s.overview = xmlDecode(xmlText(xml, "description"));

    const std::string start = xmlText(xml, "startdate");
    s.originally_available_at = start;
    const int y = yearFromDate(start);
    if (y > 0) s.year = y;

    // Status: if enddate is present and non-empty, the series ended
    s.status = xmlText(xml, "enddate").empty() ? "Continuing" : "Ended";

    const std::string pic = xmlText(xml, "picture");
    if (!pic.empty()) s.thumb = std::string(kImgBase) + pic;

    // Rating: <ratings><permanent>8.52</permanent></ratings>
    const std::string ratings_sec = xmlSection(xml, "ratings");
    if (!ratings_sec.empty()) {
        const std::string rating = xmlText(ratings_sec, "permanent");
        if (!rating.empty()) {
            try { s.audience_rating = std::stof(rating); } catch (...) {}
        }
    }

    // External IDs from <resources><resource type="4"> = TheTVDB
    const std::string res_sec = xmlSection(xml, "resources");
    for (auto& [rtag, rcontent] : xmlAll(res_sec, "resource")) {
        if (xmlAttr(rtag, "type") == "4") {
            s.tvdb_id = xmlText(rcontent, "identifier");
        }
    }

    return s;
}

// ---------------------------------------------------------------------------
// Parse anime detail XML → Movie
// ---------------------------------------------------------------------------

Movie AnidbScraper::movieFromXml(const std::string& xml, const std::string& aid) {
    Movie m;
    m.movie_id = aid;

    const std::string titles_sec = xmlSection(xml, "titles");
    for (auto& [tag, text] : xmlAll(titles_sec, "title")) {
        const bool en  = tag.find("xml:lang=\"en\"") != std::string::npos;
        const bool off = tag.find("type=\"official\"") != std::string::npos;
        const bool main = tag.find("type=\"main\"") != std::string::npos;
        if (en && off)             { m.title = xmlDecode(text); break; }
        if (en && m.title.empty()) m.title = xmlDecode(text);
        if (main && m.title.empty()) m.title = xmlDecode(text);
    }
    if (m.title.empty()) m.title = aid;

    m.overview = xmlDecode(xmlText(xml, "description"));

    const std::string start = xmlText(xml, "startdate");
    const int y = yearFromDate(start);
    if (y > 0) m.year = y;

    const std::string pic = xmlText(xml, "picture");
    if (!pic.empty()) m.thumb = std::string(kImgBase) + pic;

    const std::string ratings_sec = xmlSection(xml, "ratings");
    if (!ratings_sec.empty()) {
        const std::string rating = xmlText(ratings_sec, "permanent");
        if (!rating.empty()) {
            try { m.audience_rating = std::stof(rating); } catch (...) {}
        }
    }

    return m;
}

// ---------------------------------------------------------------------------
// Parse anime detail XML → Episodes
// Only regular episodes (epno type="1") are returned; specials/trailers skipped.
// ---------------------------------------------------------------------------

std::vector<Episode> AnidbScraper::episodesFromXml(const std::string& xml,
                                                     const std::string& show_id) {
    const std::string eps_sec = xmlSection(xml, "episodes");
    std::vector<Episode> out;

    for (auto& [etag, econtent] : xmlAll(eps_sec, "episode")) {
        // Only regular episodes
        std::string epno_text;
        for (auto& [pt, ptext] : xmlAll(econtent, "epno")) {
            if (xmlAttr(pt, "type") == "1") { epno_text = ptext; break; }
        }
        if (epno_text.empty()) continue;

        Episode ep;
        ep.episode_id = "anidb:" + xmlAttr(etag, "id");
        ep.show_id    = show_id;
        ep.season     = 1;
        try { ep.episode = std::stoi(epno_text); } catch (...) { continue; }

        ep.air_date = xmlText(econtent, "airdate");

        // AniDB stores episode length in minutes
        const std::string len = xmlText(econtent, "length");
        if (!len.empty()) {
            try { ep.duration_ms = std::stoll(len) * 60 * 1000; } catch (...) {}
        }

        // English title, falling back to first available
        bool got_title = false;
        for (auto& [ttag, ttext] : xmlAll(econtent, "title")) {
            if (ttag.find("xml:lang=\"en\"") != std::string::npos) {
                ep.title = xmlDecode(ttext);
                got_title = true;
                break;
            }
        }
        if (!got_title) {
            for (auto& [ttag, ttext] : xmlAll(econtent, "title")) {
                ep.title = xmlDecode(ttext);
                break;
            }
        }

        out.push_back(std::move(ep));
    }

    std::sort(out.begin(), out.end(), [](const Episode& a, const Episode& b){
        return a.episode < b.episode;
    });
    return out;
}

// ---------------------------------------------------------------------------
// IMetadataScraper implementation
// ---------------------------------------------------------------------------

std::vector<Show> AnidbScraper::searchShows(const std::string& title, int year) {
    if (!ensureTitleDump()) return {};
    auto matches = searchTitleDump(title);
    std::vector<Show> out;
    out.reserve(matches.size());
    for (const auto& m : matches) {
        Show s;
        // show_id stores the AID so ScraperManager can use it as the external_id
        s.show_id = m.aid;
        s.title   = m.title;
        (void)year;
        out.push_back(std::move(s));
    }
    return out;
}

std::optional<Show> AnidbScraper::fetchShow(const std::string& external_id) {
    auto xml = fetchAnimeXml(external_id);
    if (xml.empty()) return std::nullopt;
    return showFromXml(xml, external_id);
}

std::vector<Episode> AnidbScraper::fetchEpisodes(const std::string& external_id) {
    auto xml = fetchAnimeXml(external_id);
    if (xml.empty()) return {};
    return episodesFromXml(xml, external_id);
}

std::vector<Movie> AnidbScraper::searchMovies(const std::string& title, int year) {
    if (!ensureTitleDump()) return {};
    auto matches = searchTitleDump(title);
    std::vector<Movie> out;
    out.reserve(matches.size());
    for (const auto& m : matches) {
        Movie mv;
        mv.movie_id = m.aid;  // AID stored as movie_id for external_id extraction
        mv.title    = m.title;
        (void)year;
        out.push_back(std::move(mv));
    }
    return out;
}

std::optional<Movie> AnidbScraper::fetchMovie(const std::string& external_id) {
    auto xml = fetchAnimeXml(external_id);
    if (xml.empty()) return std::nullopt;
    return movieFromXml(xml, external_id);
}
