#pragma once
#include "../Config.h"
#include "../stream/SessionManager.h"
#include "../stream/VodSessionManager.h"
#include "../stream/PreviewSessionManager.h"
#include "../kairos/KairosClient.h"
#include "../log/LogBuffer.h"
#include <httplib.h>

void registerRoutes(httplib::Server& svr, SessionManager& sessions, VodSessionManager& vodSessions,
                    PreviewSessionManager& previewSessions,
                    KairosClient& kairos, LogBuffer& logs, const Config& cfg);
