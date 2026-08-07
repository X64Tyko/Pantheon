#pragma once
#include "../broadcast/BroadcasterManager.h"
#include "../devices/DeviceSessionManager.h"
#include "../kairos/KairosClient.h"
#include "../watchtogether/WatchTogetherManager.h"
#include "log/LogBuffer.h"
#include "../Config.h"
#include <httplib.h>

// logs: the "combined" buffer /api/logs/stream actually serves (no file of
// its own — see main.cpp's own comment on why). local_log: the file-backed
// buffer holding Hermes's *own* lines only (hermes.log) — used solely to back
// /api/logs/file's own-log export and /api/logs/export's zip, both of which
// need a real on-disk path, unlike everything else here.
void registerRoutes(httplib::Server& svr, BroadcasterManager& broadcasters,
					KairosClient& kairos, LogBuffer& logs, LogBuffer& local_log, const Config& cfg,
					DeviceSessionManager& devices, WatchTogetherManager& watch_together);