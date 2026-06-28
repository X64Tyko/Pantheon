#pragma once
#include "../IKairosService.h"

class ScraperManager;

class ScraperService : public IKairosService {
public:
    explicit ScraperService(ScraperManager& scraper);
    void registerRoutes(httplib::Server& svr) override;

private:
    ScraperManager& scraper_;
};
