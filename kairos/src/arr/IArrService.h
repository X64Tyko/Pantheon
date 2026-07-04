#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

// Sonarr/Radarr reject an add with either a single {"message": "..."} object
// or an array of per-field validation failures
// ([{"propertyName":"RootFolderPath","errorMessage":"..."}, ...]) — both
// shapes need to be tried, or a rejection like "already added" / "root
// folder does not exist" gets silently swallowed into a generic failure.
inline std::string parseArrError(int status, const std::string& body) {
    try {
        auto j = json::parse(body);
        std::vector<std::string> msgs;
        if (j.is_array()) {
            for (const auto& e : j) {
                std::string m = e.value("errorMessage", "");
                if (m.empty()) m = e.value("message", "");
                if (!m.empty()) msgs.push_back(m);
            }
        } else if (j.is_object()) {
            std::string m = j.value("message", "");
            if (m.empty()) m = j.value("error", "");
            if (!m.empty()) msgs.push_back(m);
        }
        if (!msgs.empty()) {
            std::string out;
            for (size_t i = 0; i < msgs.size(); ++i) { if (i) out += "; "; out += msgs[i]; }
            return out;
        }
    } catch (...) {}
    if (status == 0) return "no response from arr server (connection failed)";
    return "HTTP " + std::to_string(status) + (body.empty() ? "" : (": " + body.substr(0, 200)));
}

struct ArrLookupResult {
    std::string title;
    int         year          = 0;
    std::string external_id;    // tvdb_id for shows, tmdb_id for movies
    std::string poster_url;
    bool        already_added = false;
    json        add_data;       // full arr response body; sent back verbatim on add
};

struct ArrQualityProfile {
    int         id;
    std::string name;
};

struct ArrServiceOptions {
    std::vector<ArrQualityProfile> quality_profiles;
    std::vector<std::string>       root_folders;
};

struct ArrAddOptions {
    int         quality_profile_id = 1;
    std::string root_folder;
    bool        monitored     = true;
    bool        search_on_add = true;
};

class IArrService {
public:
    virtual ~IArrService() = default;

    virtual bool isConfigured() const = 0;

    // Search the arr server; no side effects.
    virtual std::vector<ArrLookupResult> lookup(const std::string& term) = 0;

    // Fetch quality profiles and root folders for the confirmation dialog.
    virtual ArrServiceOptions getOptions() = 0;

    // Add the item. add_data is the opaque blob from ArrLookupResult::add_data.
    virtual bool add(const json& add_data, const ArrAddOptions& opts) = 0;

    // Convenience: parse options from a request body and call add().
    bool addFromRequest(const json& body) {
        if (!body.contains("add_data")) return false;
        ArrAddOptions opts;
        opts.quality_profile_id = body.value("quality_profile_id", 1);
        opts.root_folder        = body.value("root_folder", "");
        opts.search_on_add      = body.value("search_on_add", true);
        return add(body["add_data"], opts);
    }
};
