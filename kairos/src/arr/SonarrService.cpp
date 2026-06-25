#include "SonarrService.h"
#include <iostream>

static std::string pctEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else { char buf[4]; snprintf(buf, sizeof(buf), "%%%02X", c); out += buf; }
    }
    return out;
}

SonarrService::SonarrService(const std::string& base_url, const std::string& api_key)
    : base_url_(base_url), api_key_(api_key), client_(base_url)
{
    client_.set_default_headers({{"X-Api-Key", api_key_}, {"Accept", "application/json"}});
    client_.set_connection_timeout(10);
    client_.set_read_timeout(30);
}

httplib::Result SonarrService::get(const std::string& path) {
    auto res = client_.Get(path);
    if (!res) std::cerr << "[sonarr] GET " << path << " — " << httplib::to_string(res.error()) << '\n';
    else if (res->status != 200) std::cerr << "[sonarr] GET " << path << " — HTTP " << res->status << '\n';
    return res;
}

httplib::Result SonarrService::post(const std::string& path, const std::string& body) {
    auto res = client_.Post(path, body, "application/json");
    if (!res) std::cerr << "[sonarr] POST " << path << " — " << httplib::to_string(res.error()) << '\n';
    return res;
}

std::vector<ArrLookupResult> SonarrService::lookup(const std::string& term) {
    auto res = get("/api/v3/series/lookup?term=" + pctEncode(term));
    if (!res || res->status != 200) return {};

    std::vector<ArrLookupResult> results;
    try {
        auto j = json::parse(res->body);
        if (!j.is_array()) return {};
        for (const auto& item : j) {
            ArrLookupResult r;
            r.title        = item.value("title", "");
            r.year         = item.value("year", 0);
            r.external_id  = std::to_string(item.value("tvdbId", 0));
            r.poster_url   = item.value("remotePoster", "");
            r.already_added = item.value("id", 0) > 0;
            r.add_data     = item;
            results.push_back(std::move(r));
        }
    } catch (const json::exception& e) {
        std::cerr << "[sonarr] parse error (lookup): " << e.what() << '\n';
    }
    return results;
}

ArrServiceOptions SonarrService::getOptions() {
    ArrServiceOptions opts;
    if (auto res = get("/api/v3/qualityprofile"); res && res->status == 200) {
        try {
            for (const auto& p : json::parse(res->body))
                opts.quality_profiles.push_back({p["id"].get<int>(), p.value("name", "")});
        } catch (...) {}
    }
    if (auto res = get("/api/v3/rootfolder"); res && res->status == 200) {
        try {
            for (const auto& f : json::parse(res->body))
                opts.root_folders.push_back(f.value("path", ""));
        } catch (...) {}
    }
    return opts;
}

bool SonarrService::add(const json& add_data, const ArrAddOptions& opts) {
    json body = add_data;
    body["qualityProfileId"] = opts.quality_profile_id;
    body["rootFolderPath"]   = opts.root_folder;
    body["monitored"]        = opts.monitored;
    body["seasonFolder"]     = true;
    body["addOptions"] = {
        {"searchForMissingEpisodes", opts.search_on_add},
        {"ignoreEpisodesWithFiles", false},
        {"ignoreEpisodesWithoutFiles", false},
    };
    auto res = post("/api/v3/series", body.dump());
    if (!res || (res->status != 200 && res->status != 201)) {
        std::cerr << "[sonarr] add failed (HTTP " << (res ? res->status : 0) << "): "
                  << (res ? res->body.substr(0, 300) : "no response") << '\n';
        return false;
    }
    return true;
}
