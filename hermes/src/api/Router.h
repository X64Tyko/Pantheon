#pragma once
#include "../broadcast/BroadcasterManager.h"
#include "../kairos/KairosClient.h"
#include "../log/LogBuffer.h"
#include "../Config.h"
#include <httplib.h>

void registerRoutes(httplib::Server& svr, BroadcasterManager& broadcasters,
                    KairosClient& kairos, LogBuffer& logs, const Config& cfg);
