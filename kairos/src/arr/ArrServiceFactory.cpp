#include "ArrServiceFactory.h"
#include "RadarrService.h"
#include "SonarrService.h"
#include "../db/ConfigRepository.h"
#include "../db/Database.h"

std::unique_ptr<IArrService> ArrServiceFactory::make(const std::string& type, Database& db) {
    ConfigRepository cfg(db);
    if (type == "show") {
        auto url = cfg.getValue("sonarr_url");
        auto key = cfg.getValue("sonarr_api_key");
        if (url.empty() || key.empty()) return nullptr;
        return std::make_unique<SonarrService>(url, key);
    }
    if (type == "movie") {
        auto url = cfg.getValue("radarr_url");
        auto key = cfg.getValue("radarr_api_key");
        if (url.empty() || key.empty()) return nullptr;
        return std::make_unique<RadarrService>(url, key);
    }
    return nullptr;
}
