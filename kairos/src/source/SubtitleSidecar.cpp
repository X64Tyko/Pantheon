#include "SubtitleSidecar.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

const std::unordered_set<std::string> kSubtitleExts = { ".srt", ".ass", ".ssa", ".vtt" };

// ISO 639-1 (2-letter) -> ISO 639-2 (3-letter), covering the same language
// set as hades/src/channel/ChannelDefaultsPanel.tsx's LANG_NAMES dict, so a
// filename using either convention normalizes to the same code ffprobe
// already reports for embedded streams.
const std::unordered_map<std::string, std::string> kIso6391To6392 = {
    {"en", "eng"}, {"ja", "jpn"}, {"fr", "fre"}, {"de", "ger"}, {"es", "spa"},
    {"pt", "por"}, {"it", "ita"}, {"zh", "chi"}, {"ko", "kor"}, {"ru", "rus"},
    {"ar", "ara"}, {"nl", "dut"}, {"pl", "pol"}, {"sv", "swe"}, {"no", "nor"},
    {"da", "dan"}, {"fi", "fin"}, {"hu", "hun"}, {"cs", "cze"}, {"sk", "slk"},
    {"hr", "hrv"}, {"sr", "srp"}, {"bg", "bul"}, {"ro", "rum"}, {"tr", "tur"},
    {"he", "heb"}, {"hi", "hin"}, {"th", "tha"}, {"vi", "vie"}, {"id", "ind"},
    {"ms", "msa"}, {"uk", "ukr"}, {"ca", "cat"}, {"la", "lat"},
};

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isAlpha(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(),
        [](unsigned char c) { return std::isalpha(c) != 0; });
}

std::vector<std::string> splitTags(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t dot = s.find('.', start);
        if (dot == std::string::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, dot - start));
        start = dot + 1;
    }
    return out;
}

// Bare "hi" is deliberately NOT treated as a hearing-impaired marker here —
// it collides with ISO 639-1 Hindi, and a lone two-letter tag is far more
// commonly a language code than an HI marker (which convention almost always
// spells out "sdh" or "cc" instead).
void applyTag(const std::string& raw, ExternalSubtitle& sub) {
    const std::string t = toLower(raw);
    if (t == "forced") { sub.forced = true; return; }
    if (t == "sdh" || t == "cc") { sub.sdh = true; return; }
    if (!sub.language.empty() || !isAlpha(t)) return;
    if (t.size() == 3) { sub.language = t; return; }
    if (t.size() == 2) {
        auto it = kIso6391To6392.find(t);
        if (it != kIso6391To6392.end()) sub.language = it->second;
    }
}

// Longest video_stem that stem_and_tags is exactly, or is prefixed by
// followed by a literal '.' — never a bare textual prefix (so a stem
// "Movie" never matches a file actually belonging to "Movie 2"). Longest
// first so a more specific stem wins when one stem happens to be a prefix
// of another in the same folder.
std::string matchVideoStem(const std::string& stem_and_tags, const std::vector<std::string>& video_stems) {
    std::string best;
    for (const auto& stem : video_stems) {
        if (stem.size() <= best.size()) continue;
        if (stem_and_tags == stem ||
            (stem_and_tags.size() > stem.size() &&
             stem_and_tags.compare(0, stem.size(), stem) == 0 &&
             stem_and_tags[stem.size()] == '.'))
            best = stem;
    }
    return best;
}

} // namespace

std::unordered_map<std::string, std::vector<ExternalSubtitle>> scanDirectoryForSubtitles(
        const std::string& dir, const std::vector<std::string>& video_stems) {
    std::unordered_map<std::string, std::vector<ExternalSubtitle>> result;
    if (video_stems.empty()) return result;

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        std::error_code file_ec;
        if (!entry.is_regular_file(file_ec) || file_ec) continue;

        const fs::path& p = entry.path();
        std::string ext = toLower(p.extension().string());
        if (!kSubtitleExts.count(ext)) continue;

        // Strip only the recognised subtitle extension — the remainder can
        // still contain dots from the video's own filename (scene-release
        // names routinely do), so this is matched against known stems next
        // rather than split naively on the first dot.
        const std::string stem_and_tags = p.filename().stem().string();

        const std::string video_stem = matchVideoStem(stem_and_tags, video_stems);
        if (video_stem.empty()) continue;

        const std::string tags_suffix = stem_and_tags.size() > video_stem.size()
            ? stem_and_tags.substr(video_stem.size() + 1) // skip the separating '.'
            : "";

        ExternalSubtitle sub;
        sub.file_path = p.string();
        for (const auto& tag : splitTags(tags_suffix))
            if (!tag.empty()) applyTag(tag, sub);

        result[video_stem].push_back(std::move(sub));
    }
    return result;
}
