#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

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
