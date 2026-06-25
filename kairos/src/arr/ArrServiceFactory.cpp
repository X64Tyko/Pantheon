#include "ArrServiceFactory.h"
#include "RadarrService.h"
#include "SonarrService.h"
#include "db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>

static std::string getConfig(Database& db, const std::string& key) {
    SQLite::Statement q(db.get(), "SELECT value FROM app_config WHERE key = ?");
    q.bind(1, key);
    return q.executeStep() ? q.getColumn(0).getString() : "";
}

std::unique_ptr<IArrService> ArrServiceFactory::make(const std::string& type, Database& db) {
    if (type == "show") {
        auto url = getConfig(db, "sonarr_url");
        auto key = getConfig(db, "sonarr_api_key");
        if (url.empty() || key.empty()) return nullptr;
        return std::make_unique<SonarrService>(url, key);
    }
    if (type == "movie") {
        auto url = getConfig(db, "radarr_url");
        auto key = getConfig(db, "radarr_api_key");
        if (url.empty() || key.empty()) return nullptr;
        return std::make_unique<RadarrService>(url, key);
    }
    return nullptr;
}
