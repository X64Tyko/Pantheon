#pragma once
#include "../broadcast/BroadcasterManager.h"
#include "../kairos/KairosClient.h"
#include "../Config.h"
#include <httplib.h>

void registerRoutes(httplib::Server& svr, BroadcasterManager& broadcasters,
                    KairosClient& kairos, const Config& cfg);
