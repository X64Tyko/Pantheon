#pragma once
#include "DeviceSessionManager.h"
#include "../Config.h"
#include <httplib.h>

void registerDeviceRoutes(httplib::Server& svr, DeviceSessionManager& devices, const Config& cfg);
