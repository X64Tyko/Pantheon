#pragma once
#include "../broadcast/BroadcasterManager.h"
#include "../devices/DeviceSessionManager.h"
#include "../kairos/KairosClient.h"
#include "../watchtogether/WatchTogetherManager.h"
#include "log/LogBuffer.h"
#include "../Config.h"
#include <httplib.h>

void registerRoutes(httplib::Server& svr, BroadcasterManager& broadcasters,
                    KairosClient& kairos, LogBuffer& logs, const Config& cfg,
                    DeviceSessionManager& devices, WatchTogetherManager& watch_together);
