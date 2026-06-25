#pragma once
#include "IArrService.h"
#include <httplib.h>
#include <string>

class SonarrService final : public IArrService {
public:
    SonarrService(const std::string& base_url, const std::string& api_key);

    bool isConfigured() const override { return !base_url_.empty() && !api_key_.empty(); }

    std::vector<ArrLookupResult> lookup(const std::string& term) override;
    ArrServiceOptions            getOptions()                     override;
    bool                         add(const json& add_data, const ArrAddOptions& opts) override;

private:
    httplib::Result get(const std::string& path);
    httplib::Result post(const std::string& path, const std::string& body);

    std::string     base_url_;
    std::string     api_key_;
    httplib::Client client_;
};
