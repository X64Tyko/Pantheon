#include "SidecarMetadata.h"
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

const std::unordered_set<std::string> kImageExts = {".jpg", ".jpeg", ".png"};

std::string toLowerStr(std::string s) {
    for (char& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Case-insensitive: first file directly in dir whose stem is one of `stems`
// and whose extension is a recognised image type.
std::string findNamedImage(const fs::path& dir, const std::vector<std::string>& stems) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (!kImageExts.count(toLowerStr(entry.path().extension().string()))) continue;
        std::string stem = toLowerStr(entry.path().stem().string());
        for (const auto& want : stems)
            if (stem == want) return "local:" + fs::absolute(entry.path()).string();
    }
    return "";
}

// Matches "<video_stem>-<suffix>.<ext>" for each of `suffixes`, or (when
// allow_bare) a bare "<video_stem>.<ext>" — the convention used when a movie
// is a lone file with no folder of its own to hold a fixed poster.jpg name.
std::string findStemImage(const fs::path& dir, const std::string& video_stem,
                           const std::vector<std::string>& suffixes, bool allow_bare) {
    std::error_code ec;
    const std::string want_stem = toLowerStr(video_stem);
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (!kImageExts.count(toLowerStr(entry.path().extension().string()))) continue;
        std::string stem = toLowerStr(entry.path().stem().string());
        if (allow_bare && stem == want_stem) return "local:" + fs::absolute(entry.path()).string();
        for (const auto& suf : suffixes)
            if (stem == want_stem + "-" + suf) return "local:" + fs::absolute(entry.path()).string();
    }
    return "";
}

std::string trimmed(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

std::string tag(const pugi::xml_node& root, const char* name) {
    return trimmed(root.child_value(name));
}

// JSON array string from every occurrence of a repeated tag, e.g. <genre>.
std::string tagList(const pugi::xml_node& root, const char* name) {
    json out = json::array();
    for (auto node : root.children(name)) {
        std::string v = trimmed(node.text().as_string());
        if (!v.empty()) out.push_back(v);
    }
    return out.dump();
}

// JSON array of <actor><name>...</name></actor> names.
std::string actorList(const pugi::xml_node& root) {
    json out = json::array();
    for (auto node : root.children("actor")) {
        std::string name = trimmed(node.child_value("name"));
        if (!name.empty()) out.push_back(name);
    }
    return out.dump();
}

// <uniqueid type="imdb">tt123</uniqueid>, falling back to legacy flat tags
// (<imdbid>/<tmdbid>/<tvdbid>, or a bare <id> defaulting to imdb) that older
// scrapers/tools emit instead of the modern <uniqueid> form.
std::string uniqueId(const pugi::xml_node& root, const char* type, const char* legacy_tag) {
    for (auto node : root.children("uniqueid"))
        if (std::string(node.attribute("type").value()) == type) {
            std::string v = trimmed(node.text().as_string());
            if (!v.empty()) return v;
        }
    std::string legacy = tag(root, legacy_tag);
    if (!legacy.empty()) return legacy;
    if (std::string(type) == "imdb") return tag(root, "id");
    return "";
}

std::optional<float> ratingOf(const pugi::xml_node& root) {
    std::string flat = tag(root, "rating");
    if (!flat.empty()) {
        try { return std::stof(flat); } catch (...) {}
    }
    auto nested = root.child("ratings").child("rating").child("value");
    if (nested) {
        try { return std::stof(trimmed(nested.text().as_string())); } catch (...) {}
    }
    return std::nullopt;
}

std::optional<int> yearOf(const pugi::xml_node& root) {
    std::string y = tag(root, "year");
    if (y.empty()) return std::nullopt;
    try { return std::stoi(y); } catch (...) { return std::nullopt; }
}

std::string releaseDateOf(const pugi::xml_node& root) {
    std::string v = tag(root, "premiered");
    if (!v.empty()) return v;
    return tag(root, "releasedate");
}

} // namespace

std::optional<NfoMovie> loadMovieSidecar(const fs::path& video_path, const fs::path& art_dir,
                                          bool has_own_folder) {
    std::error_code ec;
    fs::path nfo_path = fs::path(video_path).replace_extension(".nfo");
    if (!fs::exists(nfo_path, ec)) {
        if (!has_own_folder) return std::nullopt;
        nfo_path = video_path.parent_path() / "movie.nfo";
        if (!fs::exists(nfo_path, ec)) return std::nullopt;
    }

    pugi::xml_document doc;
    if (!doc.load_file(nfo_path.string().c_str())) return std::nullopt;
    auto root = doc.child("movie");
    if (!root) return std::nullopt;

    NfoMovie m;
    m.title           = tag(root, "title");
    m.year            = yearOf(root);
    m.overview        = tag(root, "plot");
    if (m.overview.empty()) m.overview = tag(root, "outline");
    m.tagline         = tag(root, "tagline");
    m.content_rating  = tag(root, "mpaa");
    m.genres          = tagList(root, "genre");
    m.studio          = tag(root, "studio");
    m.director        = tag(root, "director");
    m.actors          = actorList(root);
    m.countries       = tagList(root, "country");
    m.release_date    = releaseDateOf(root);
    m.imdb_id         = uniqueId(root, "imdb", "imdbid");
    m.tmdb_id         = uniqueId(root, "tmdb", "tmdbid");
    m.audience_rating = ratingOf(root);

    m.thumb = findNamedImage(art_dir, {"poster", "folder"});
    m.art   = findNamedImage(art_dir, {"fanart", "backdrop"});
    if (m.thumb.empty())
        m.thumb = findStemImage(art_dir, video_path.stem().string(), {"poster"}, true);
    if (m.art.empty())
        m.art = findStemImage(art_dir, video_path.stem().string(), {"fanart", "backdrop"}, false);

    return m;
}

std::optional<NfoShow> loadShowSidecar(const fs::path& show_dir) {
    std::error_code ec;
    fs::path nfo_path = show_dir / "tvshow.nfo";
    if (!fs::exists(nfo_path, ec)) return std::nullopt;

    pugi::xml_document doc;
    if (!doc.load_file(nfo_path.string().c_str())) return std::nullopt;
    auto root = doc.child("tvshow");
    if (!root) return std::nullopt;

    NfoShow s;
    s.title           = tag(root, "title");
    s.year            = yearOf(root);
    s.overview        = tag(root, "plot");
    s.content_rating  = tag(root, "mpaa");
    s.genres          = tagList(root, "genre");
    s.network         = tag(root, "studio"); // Kodi has no separate "network" tag for shows
    s.status          = tag(root, "status");
    s.actors          = actorList(root);
    s.countries       = tagList(root, "country");
    s.release_date    = releaseDateOf(root);
    s.imdb_id         = uniqueId(root, "imdb", "imdbid");
    s.tmdb_id         = uniqueId(root, "tmdb", "tmdbid");
    s.tvdb_id         = uniqueId(root, "tvdb", "tvdbid");
    s.audience_rating = ratingOf(root);

    s.thumb = findNamedImage(show_dir, {"poster", "folder"});
    s.art   = findNamedImage(show_dir, {"fanart", "backdrop"});

    return s;
}

std::optional<NfoEpisode> loadEpisodeSidecar(const fs::path& video_path) {
    std::error_code ec;
    fs::path nfo_path = fs::path(video_path).replace_extension(".nfo");
    if (!fs::exists(nfo_path, ec)) return std::nullopt;

    pugi::xml_document doc;
    if (!doc.load_file(nfo_path.string().c_str())) return std::nullopt;
    auto root = doc.child("episodedetails");
    if (!root) return std::nullopt;

    NfoEpisode e;
    e.title    = tag(root, "title");
    e.overview = tag(root, "plot");
    e.air_date = tag(root, "aired");
    e.thumb    = findStemImage(video_path.parent_path(), video_path.stem().string(), {"thumb"}, false);
    return e;
}
