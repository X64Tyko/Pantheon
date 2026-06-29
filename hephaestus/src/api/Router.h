#pragma once
#include "../stream/SessionManager.h"
#include "../kairos/KairosClient.h"
#include "../log/LogBuffer.h"
#include <httplib.h>

void registerRoutes(httplib::Server& svr, SessionManager& sessions,
                    KairosClient& kairos, LogBuffer& logs);
