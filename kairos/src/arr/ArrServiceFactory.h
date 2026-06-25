#pragma once
#include "IArrService.h"
#include <memory>
#include <string>

class Database;

class ArrServiceFactory {
public:
    // Returns nullptr if the requested type is not configured in app_config.
    // type: "show" → SonarrService, "movie" → RadarrService
    static std::unique_ptr<IArrService> make(const std::string& type, Database& db);
};
