#pragma once
#include "WatchTogetherManager.h"
#include "../Config.h"
#include <httplib.h>

void registerWatchTogetherRoutes(httplib::Server& svr, WatchTogetherManager& watch_together, const Config& cfg);
